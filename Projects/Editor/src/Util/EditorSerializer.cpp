// FBZZ Engine
// EditorSerializer.cpp | fbzz::editor
// Scene に付随する Editor 専用メタデータの TOML 永続化
//
// 保存先は Scene の "<scene>.meta" サイドカー内の [editor] テーブル。
//
// WHY サイドカーを丸ごと上書きしないか (不具合修正):
//   "<scene>.meta" は Editor だけのものではない。AssetDatabase が同じファイルの
//   [meta] guid にアセット GUID を書いており、.scene は GUID を持つ対象
//   (AssetDatabase::ShouldHaveMeta) に含まれる。以前の実装は toml::table を新規に
//   作って WriteText で上書きしていたため、シーンを保存するたびに [meta] guid が
//   消えていた。次のスキャンで AssetDatabase が「GUID の無い .meta」を見つけて
//   新しい乱数 GUID を振り直すため、そのシーンを指す guid: 参照が保存のたびに
//   壊れる。逆に AssetDatabase が先に書けば Editor の並び順が読めなくなる
//   (実際、並び替えても次回起動で元へ戻る状態だった)。
//   AssetDatabase 側の WriteGuidToMeta と同じく「既存テーブルを読んで自分の
//   セクションだけ差し替える」方式に揃え、2 者が同じファイルに同居できるようにする。
#include <Editor/Util/EditorSerializer.hpp>
#include <Editor/Util/EditorSceneState.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <sstream>
#include <string_view>
#include <utility>

namespace fbzz::editor {

namespace {

// サイドカー内で Editor 専用データを収めるテーブル名。
constexpr std::string_view kEditorTable = "editor";

// 既存のサイドカーを読み、他の書き手のセクション ([meta] 等) を保った table を返す。
// 読めない / 壊れている場合は空の table (新規作成扱い)。
toml::table ReadSidecar(const std::string& path)
{
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) return {};
    std::istringstream stream(text);
    const auto parsed = toml::parse(stream);
    if (!parsed) {
        FBZZ_LOG_WARN("EditorSerializer: sidecar is not valid TOML, other sections may be lost: %s",
                      path.c_str());
        return {};
    }
    return parsed.table();
}

// 1 GameObject ぶんの並び順を読み取って state へ流し込む。
// キー名は形式ごとに違うため呼び出し側から渡す。
void ReadComponentOrders(const toml::array& objectArray,
                         const char* instanceIdKey,
                         const char* orderKey,
                         EditorSceneState& state)
{
    for (const auto& node : objectArray) {
        const auto* objectTable = node.as_table();
        if (!objectTable) continue;
        const std::string instanceId = (*objectTable)[instanceIdKey].value_or(std::string{});
        const auto* orderArray = (*objectTable)[orderKey].as_array();
        if (instanceId.empty() || !orderArray) continue;

        EditorSceneState::ComponentOrder order;
        order.reserve(orderArray->size());
        for (const auto& orderNode : *orderArray)
            order.push_back(orderNode.value_or(std::string{}));
        state.SetComponentOrder(instanceId, std::move(order));
    }
}

} // namespace

std::string EditorSerializer::MetadataPath(const std::string& scenePath)
{
    // .meta を末尾に置くことで AssetBrowser の通常アセット一覧から隠し、
    // Scene の移動・リネーム時も既存のサイドカー処理で追随させる。
    return scenePath + ".meta";
}

bool EditorSerializer::Save(const EditorSceneState& state, const std::string& scenePath)
{
    if (scenePath.empty()) return false;

    toml::array objectArray;
    for (const auto& [instanceId, order] : state.GetAllComponentOrders()) {
        if (instanceId.empty() || order.empty()) continue;
        toml::table objectTable;
        objectTable.insert("instance_id", instanceId);
        toml::array orderArray;
        for (const std::string& key : order)
            orderArray.push_back(key);
        objectTable.insert("component_order", std::move(orderArray));
        objectArray.push_back(std::move(objectTable));
    }

    toml::table editorTable;
    editorTable.insert("format_version", 2);
    editorTable.insert("game_objects", std::move(objectArray));

    const std::string path = MetadataPath(scenePath);
    if (!util::FileSystem::EnsureDirectory(util::FileSystem::GetDirectory(path))) {
        FBZZ_LOG_ERROR("EditorSerializer: failed to create metadata directory: %s", path.c_str());
        return false;
    }

    toml::table root = ReadSidecar(path);
    // 旧形式 (v1) がトップレベルに残っていれば取り除く。移行はこの 1 回で完了する。
    root.erase("format_version");
    root.erase("game_objects");
    root.insert_or_assign(kEditorTable, std::move(editorTable));

    std::ostringstream stream;
    stream << root;
    if (!util::FileSystem::WriteText(path, stream.str())) {
        FBZZ_LOG_ERROR("EditorSerializer: failed to save metadata: %s", path.c_str());
        return false;
    }
    return true;
}

bool EditorSerializer::Load(EditorSceneState& state, const std::string& scenePath)
{
    state.Clear();
    if (scenePath.empty()) return false;

    const std::string path = MetadataPath(scenePath);
    std::string text;
    if (util::FileSystem::ReadText(path, text)) {
        auto result = toml::parse(text);
        if (!result) {
            FBZZ_LOG_WARN("EditorSerializer: failed to parse metadata: %s", path.c_str());
            return false;
        }

        // 現行 (v2): [editor] テーブル配下。
        if (const auto* editorTable = result.table()[kEditorTable].as_table()) {
            if (const auto* objectArray = (*editorTable)["game_objects"].as_array())
                ReadComponentOrders(*objectArray, "instance_id", "component_order", state);
            return true;
        }
        // 旧 (v1): サイドカーのトップレベルに直接書いていた形式。
        if (const auto* objectArray = result.table()["game_objects"].as_array()) {
            ReadComponentOrders(*objectArray, "instance_id", "component_order", state);
            return true;
        }
        // サイドカーはあるが Editor セクションが無い = GUID だけの .meta。
        // 並び順は未保存として扱い、Scene 本体の最旧形式へは落ちない
        // (サイドカーが正であり、そこに無いなら「まだ並べ替えていない」が正しい)。
        return true;
    }

    // さらに前の暫定実装では componentOrder を Scene 本体へ書いていた。
    // サイドカーがまったく無い場合だけ読み取り、次回保存時に移行する。
    if (!util::FileSystem::ReadText(scenePath, text))
        return true; // 旧 Scene / 初回利用ではどちらも無い。

    auto legacy = toml::parse(text);
    if (!legacy) return false;
    if (const auto* objectArray = legacy.table()["gameobjects"].as_array())
        ReadComponentOrders(*objectArray, "instanceId", "componentOrder", state);
    return true;
}

} // namespace fbzz::editor
