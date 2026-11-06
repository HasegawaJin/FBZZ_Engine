/// @file    EditorSerializer.cpp
/// @brief   Scene に付随する Editor 専用メタデータの TOML 永続化。
/// @author  Hasegawa Jin
/// @date    2026-08-19
/// @note [meta] guidと他の設定は保持し、[editor]だけを更新する。
#include <Editor/Util/EditorSerializer.hpp>
#include <Editor/Util/EditorSceneState.hpp>
#include <Engine/Asset/MetaFile.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::editor {

namespace {

constexpr std::string_view kEditorTable = "editor";

/// @note 読取・解析失敗時は空を返すが、MetaFile::Saveが既存ファイルの上書きを拒否する。
toml::table ReadSidecar(const std::string& path)
{
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) return {};
    std::istringstream stream(text);
    const auto parsed = toml::parse(stream);
    if (!parsed) {
        FBZZ_LOG_WARN("EditorSerializer: sidecar is not valid TOML: %s",
                      path.c_str());
        return {};
    }
    return parsed.table();
}

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

toml::array ToIdArray(const EditorSceneState::InstanceIds& ids)
{
    auto sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    toml::array array;
    for (const std::string& id : sorted)
        if (!id.empty()) array.push_back(id);
    return array;
}

EditorSceneState::InstanceIds ReadIdArray(const toml::node_view<const toml::node>& node)
{
    EditorSceneState::InstanceIds ids;
    if (const auto* array = node.as_array()) {
        ids.reserve(array->size());
        for (const auto& element : *array) {
            std::string id = element.value_or(std::string{});
            if (!id.empty()) ids.push_back(std::move(id));
        }
    }
    return ids;
}

} /// @note namespace

std::string EditorSerializer::MetadataPath(const std::string& scenePath)
{
    /// @note .metaの移動・リネームはScene本体と対で扱う。
    return scenePath + ".meta";
}

bool EditorSerializer::Save(const EditorSceneState& state, const std::string& scenePath)
{
    if (scenePath.empty()) return false;

    const auto& componentOrders = state.GetAllComponentOrders();
    std::vector<std::string> instanceIds;
    instanceIds.reserve(componentOrders.size());
    for (const auto& [instanceId, order] : componentOrders)
        if (!instanceId.empty() && !order.empty()) instanceIds.push_back(instanceId);
    /// @note unordered_mapの挿入順・再ハッシュに依存させず、Componentの表示順自体は維持する。
    std::sort(instanceIds.begin(), instanceIds.end());
    toml::array objectArray;
    for (const auto& instanceId : instanceIds) {
        const auto& order = componentOrders.at(instanceId);
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
    editorTable.insert("hidden_objects", ToIdArray(state.GetHiddenObjects()));
    editorTable.insert("locked_objects", ToIdArray(state.GetLockedObjects()));

    const std::string path = MetadataPath(scenePath);
    if (!util::FileSystem::EnsureDirectory(util::FileSystem::GetDirectory(path))) {
        FBZZ_LOG_ERROR("EditorSerializer: failed to create metadata directory: %s", path.c_str());
        return false;
    }

    toml::table root = ReadSidecar(path);
    /// @note 旧形式 (v1) がトップレベルに残っていれば取り除く。移行はこの 1 回で完了する。
    root.erase("format_version");
    root.erase("game_objects");
    root.insert_or_assign(kEditorTable, std::move(editorTable));

    std::ostringstream stream;
    stream << root;
    if (!asset::MetaFile::Save(path, stream.str())) {
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

        /// @note 現行 (v2): [editor] テーブル配下。
        if (const auto* editorTable = result.table()[kEditorTable].as_table()) {
            if (const auto* objectArray = (*editorTable)["game_objects"].as_array())
                ReadComponentOrders(*objectArray, "instance_id", "component_order", state);
            state.SetHiddenObjects(ReadIdArray((*editorTable)["hidden_objects"]));
            state.SetLockedObjects(ReadIdArray((*editorTable)["locked_objects"]));
            return true;
        }
        /// @note 旧 (v1): サイドカーのトップレベルに直接書いていた形式。
        if (const auto* objectArray = result.table()["game_objects"].as_array()) {
            ReadComponentOrders(*objectArray, "instance_id", "component_order", state);
            return true;
        }
        /// @note GUIDだけの.metaは正当な未設定データとして扱い、Scene本体の旧設定を復活させない。
        return true;
    }

    /// @note サイドカーが無い場合だけScene本体の旧設定を移行する。
    if (!util::FileSystem::ReadText(scenePath, text))
        /// @note 旧 Scene / 初回利用ではどちらも無い。
        return true;

    auto legacy = toml::parse(text);
    if (!legacy) return false;
    if (const auto* objectArray = legacy.table()["gameobjects"].as_array())
        ReadComponentOrders(*objectArray, "instanceId", "componentOrder", state);
    return true;
}

} /// @note namespace fbzz::editor
