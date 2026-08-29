/// @file    PrefabOverrides.cpp
/// @brief   プレファブインスタンスとアセット定義の TOML 差分。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Editor/Util/PrefabOverrides.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace fbzz::editor {

namespace {

// 差分の対象外にするキー。
//
// WHY: これらはインスタンス化のたびに必ず作り替えられるため、差分として出すと
//      「全インスタンスが常に override だらけ」になり、一覧が意味を失う。
//        instanceId / parentInstanceId : インスタンスごとに新規採番
//        name / parent                 : UniqueName で "Box (1)" のように一意化される
//        prefabAssetPath/prefabSourceId: リンク情報そのもの
//      末尾が "Guid" のキー (targetGuid / poleGuid / skinnedMeshOwnerGuid) も
//      Instantiate がリマップするため同様に除外する。
bool IsIdentityKey(std::string_view key)
{
    static constexpr std::string_view kExcluded[] = {
        "instanceId", "parentInstanceId", "name", "parent",
        "prefabAssetPath", "prefabSourceId"
    };
    for (std::string_view e : kExcluded)
        if (key == e) return true;

    return key.size() > 4 && key.substr(key.size() - 4) == "Guid";
}

std::string JoinPath(const std::string& prefix, std::string_view key)
{
    if (prefix.empty()) return std::string(key);
    return prefix + "." + std::string(key);
}

// 2 つの node を「値として同じか」で比べる。
// WHY: toml++ の node には汎用の等値比較が無い。配列やテーブルまで含めて厳密に
//      比較したいので、シリアライズ表現を突き合わせる。ここは差分検出のたびに
//      走るが、対象は 1 プロパティぶんの小さな node なのでコストは問題にならない。
std::string NodeToString(const toml::node& node)
{
    std::ostringstream ss;
    ss << toml::toml_formatter{node};
    return ss.str();
}

bool NodesEqual(const toml::node& lhs, const toml::node& rhs)
{
    if (lhs.type() != rhs.type()) return false;
    return NodeToString(lhs) == NodeToString(rhs);
}

// prefabTable と instanceTable を再帰的に比べ、差分を entries へ積む。
void DiffTables(const toml::table& prefabTable,
                const toml::table& instanceTable,
                const std::string& pathPrefix,
                const std::string& prefabSourceId,
                const std::string& instanceGuid,
                const std::string& objectName,
                std::vector<PrefabOverride>& entries)
{
    // インスタンス側にある / 変わったキーを見る。
    for (const auto& [keyView, instanceNode] : instanceTable) {
        const std::string_view key = keyView.str();
        if (IsIdentityKey(key)) continue;

        const std::string path = JoinPath(pathPrefix, key);
        const toml::node* prefabNode = prefabTable.get(key);

        // 入れ子テーブルは 1 段掘る (コンポーネント単位ではなくプロパティ単位で出すため)。
        if (instanceNode.is_table() && prefabNode && prefabNode->is_table()) {
            DiffTables(*prefabNode->as_table(), *instanceNode.as_table(),
                       path, prefabSourceId, instanceGuid, objectName, entries);
            continue;
        }

        if (prefabNode && NodesEqual(*prefabNode, instanceNode)) continue;

        PrefabOverride ov;
        ov.prefabSourceId = prefabSourceId;
        ov.instanceGuid   = instanceGuid;
        ov.objectName     = objectName;
        ov.path           = path;
        ov.prefabValue    = prefabNode ? FormatNodeForDisplay(prefabNode) : "(added)";
        ov.instanceValue  = FormatNodeForDisplay(&instanceNode);
        entries.push_back(std::move(ov));
    }

    // プレファブ側にあってインスタンス側に無いキー (コンポーネントを外した等)。
    for (const auto& [keyView, prefabNode] : prefabTable) {
        const std::string_view key = keyView.str();
        if (IsIdentityKey(key)) continue;
        if (instanceTable.get(key) != nullptr) continue;

        PrefabOverride ov;
        ov.prefabSourceId = prefabSourceId;
        ov.instanceGuid   = instanceGuid;
        ov.objectName     = objectName;
        ov.path           = JoinPath(pathPrefix, key);
        ov.prefabValue    = FormatNodeForDisplay(&prefabNode);
        ov.instanceValue  = "(removed)";
        entries.push_back(std::move(ov));
    }
}

// rootEntity 以下の GO を GUID で集める。
void CollectHierarchyGuids(scene::GameObject& go, std::unordered_set<std::string>& out)
{
    if (!go.instanceId.empty()) out.insert(go.instanceId);
    for (int i = 0; i < go.GetChildCount(); ++i)
        if (auto* child = go.GetChild(i))
            CollectHierarchyGuids(*child, out);
}

} // namespace

std::string FormatNodeForDisplay(const toml::node* node)
{
    if (!node) return "(none)";

    std::string text = NodeToString(*node);
    // 1 行に収める (配列やインラインテーブルの改行を潰す)。
    std::string oneLine;
    oneLine.reserve(text.size());
    bool lastWasSpace = false;
    for (char c : text) {
        const char ch = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
        if (ch == ' ') {
            if (lastWasSpace) continue;
            lastWasSpace = true;
        } else {
            lastWasSpace = false;
        }
        oneLine.push_back(ch);
    }
    constexpr std::size_t kMaxLen = 64;
    if (oneLine.size() > kMaxLen) oneLine = oneLine.substr(0, kMaxLen - 3) + "...";
    return oneLine;
}

bool ComputePrefabOverrides(scene::Scene& scene,
                            scene::EntityID rootEntity,
                            const std::string& projectRoot,
                            PrefabOverrideSet& out)
{
    out = {};

    scene::GameObject* root = scene.GetGameObject(rootEntity);
    if (!root || root->prefabAssetPath.empty()) return false;
    out.prefabAssetPath = root->prefabAssetPath;

    // ── プレファブ定義を読む ────────────────────────────────────────────────
    const std::string diskPath = ToProjectAssetDiskPath(projectRoot, root->prefabAssetPath);
    std::string prefabText;
    if (!util::FileSystem::ReadText(diskPath, prefabText)) return false;

    toml::parse_result prefabParsed = toml::parse(prefabText);
    if (!prefabParsed) return false;

    std::unordered_map<std::string, const toml::table*> prefabById;
    if (auto* arr = prefabParsed.table()["gameobjects"].as_array()) {
        for (const auto& item : *arr) {
            const auto* tbl = item.as_table();
            if (!tbl) continue;
            const std::string id = (*tbl)["instanceId"].value_or(std::string{});
            if (!id.empty()) prefabById.emplace(id, tbl);
        }
    }
    if (prefabById.empty()) return false;

    // ── インスタンス側の現在状態をシリアライズして拾う ──────────────────────
    // WHY: ランタイム構造体を直接比べず、保存されるのと同じ表現で比べる。
    //      Save したら消える差分を override として出さないため。
    std::unordered_set<std::string> hierarchyGuids;
    CollectHierarchyGuids(*root, hierarchyGuids);

    const std::string sceneText = SceneIO::Serialize(scene);
    if (sceneText.empty()) return false;
    toml::parse_result sceneParsed = toml::parse(sceneText);
    if (!sceneParsed) return false;

    auto* sceneObjects = sceneParsed.table()["gameobjects"].as_array();
    if (!sceneObjects) return false;

    for (const auto& item : *sceneObjects) {
        const auto* tbl = item.as_table();
        if (!tbl) continue;

        const std::string instanceGuid = (*tbl)["instanceId"].value_or(std::string{});
        if (instanceGuid.empty() || !hierarchyGuids.contains(instanceGuid)) continue;

        const std::string sourceId = (*tbl)["prefabSourceId"].value_or(std::string{});
        if (sourceId.empty()) continue;   // 旧アセット由来 / 手で足した子は対応先が無い

        const auto found = prefabById.find(sourceId);
        if (found == prefabById.end()) continue;

        const std::string objectName = (*tbl)["name"].value_or(std::string{"GameObject"});
        out.instanceTables.emplace(sourceId, *tbl);
        DiffTables(*found->second, *tbl, {}, sourceId, instanceGuid, objectName, out.entries);
    }

    return true;
}

PrefabOverrideSet WithoutEntry(const PrefabOverrideSet& set, const PrefabOverride& removed)
{
    PrefabOverrideSet result;
    result.prefabAssetPath = set.prefabAssetPath;
    result.instanceTables  = set.instanceTables;
    for (const PrefabOverride& e : set.entries) {
        if (e.prefabSourceId == removed.prefabSourceId && e.path == removed.path) continue;
        result.entries.push_back(e);
    }
    return result;
}

} // namespace fbzz::editor
