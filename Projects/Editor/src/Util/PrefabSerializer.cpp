// FBZZ Engine
// PrefabSerializer.cpp | fbzz::editor
// TOML-based prefab save and instantiate helpers
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneSerializer.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fbzz::editor {

namespace {

std::string WithPrefabExtension(const std::string& path)
{
    if (!util::FileSystem::GetExtension(path).empty()) return path;
    return path + ".fbzzprefab";
}

bool ContainsEntity(const std::vector<scene::EntityID>& ids, scene::EntityID id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool HasSelectedAncestor(const scene::GameObject& go, const std::vector<scene::EntityID>& selected)
{
    const scene::GameObject* parent = go.GetParent();
    while (parent) {
        if (ContainsEntity(selected, parent->GetID())) return true;
        parent = parent->GetParent();
    }
    return false;
}

void CollectHierarchyNames(scene::GameObject& go, std::unordered_set<std::string>& names)
{
    names.insert(go.name);
    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (auto* child = go.GetChild(i))
            CollectHierarchyNames(*child, names);
    }
}

std::string UniqueName(const std::string& base, std::unordered_set<std::string>& used)
{
    if (!used.contains(base)) {
        used.insert(base);
        return base;
    }

    for (int i = 1; i < 10000; ++i) {
        const std::string candidate = base + " (" + std::to_string(i) + ")";
        if (!used.contains(candidate)) {
            used.insert(candidate);
            return candidate;
        }
    }
    return base + " (Prefab)";
}

bool ReadToml(const std::string& path, toml::table& outTable)
{
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) return false;

    toml::parse_result result = toml::parse(text);
    if (!result) return false;

    outTable = result.table();
    return true;
}

} // namespace

bool PrefabSerializer::SaveSelection(const scene::Scene& scene,
                                     const std::vector<scene::EntityID>& selectedEntities,
                                     const std::string& path)
{
    if (selectedEntities.empty()) return false;

    std::vector<scene::EntityID> rootSelection;
    for (scene::EntityID id : selectedEntities) {
        auto* go = scene.GetGameObject(id);
        if (!go || HasSelectedAncestor(*go, selectedEntities)) continue;
        rootSelection.push_back(id);
    }
    if (rootSelection.empty()) return false;

    std::unordered_set<std::string> includedNames;
    for (scene::EntityID id : rootSelection) {
        if (auto* go = scene.GetGameObject(id))
            CollectHierarchyNames(*go, includedNames);
    }

    const std::string sceneText = SceneSerializer::Serialize(scene);
    if (sceneText.empty()) return false;

    toml::parse_result sceneResult = toml::parse(sceneText);
    if (!sceneResult) return false;

    toml::table doc;
    toml::table sceneInfo;
    sceneInfo.insert("format_version", 1);
    doc.insert("scene", std::move(sceneInfo));

    toml::table prefabInfo;
    prefabInfo.insert("format_version", 1);
    prefabInfo.insert("root_count", static_cast<int64_t>(rootSelection.size()));
    doc.insert("prefab", std::move(prefabInfo));

    toml::array prefabObjects;
    if (auto* gameObjects = sceneResult.table()["gameobjects"].as_array()) {
        for (const auto& item : *gameObjects) {
            const auto* source = item.as_table();
            if (!source) continue;

            const std::string name = (*source)["name"].value_or(std::string{});
            if (!includedNames.contains(name)) continue;

            toml::table copied = *source;
            const std::string parent = copied["parent"].value_or(std::string{});
            if (!parent.empty() && !includedNames.contains(parent)) {
                copied.erase("parent");
                copied.insert("parent", std::string{});
            }
            prefabObjects.push_back(std::move(copied));
        }
    }

    if (prefabObjects.empty()) return false;
    doc.insert("gameobjects", std::move(prefabObjects));

    std::ostringstream ss;
    ss << doc;
    const std::string outputPath = WithPrefabExtension(path);
    if (!util::FileSystem::WriteText(outputPath, ss.str())) {
        FBZZ_LOG_ERROR("Prefab save failed: %s", outputPath.c_str());
        return false;
    }

    FBZZ_LOG_INFO("Saved prefab: %s", outputPath.c_str());
    return true;
}

bool PrefabSerializer::Instantiate(scene::Scene& scene,
                                   const std::string& path,
                                   std::vector<scene::EntityID>& outRootEntities)
{
    outRootEntities.clear();

    toml::table prefabDoc;
    if (!ReadToml(path, prefabDoc)) {
        FBZZ_LOG_ERROR("Prefab load failed: %s", path.c_str());
        return false;
    }

    auto* prefabObjects = prefabDoc["gameobjects"].as_array();
    if (!prefabObjects || prefabObjects->empty()) return false;

    const std::string sceneText = SceneSerializer::Serialize(scene);
    if (sceneText.empty()) return false;

    toml::parse_result sceneResult = toml::parse(sceneText);
    if (!sceneResult) return false;

    toml::table merged = sceneResult.table();
    auto* mergedObjects = merged["gameobjects"].as_array();
    if (!mergedObjects) {
        toml::array emptyObjects;
        merged.insert("gameobjects", std::move(emptyObjects));
        mergedObjects = merged["gameobjects"].as_array();
        if (!mergedObjects) return false;
    }

    std::unordered_set<std::string> usedNames;
    for (auto& go : scene.GameObjects())
        usedNames.insert(go.name);

    std::unordered_map<std::string, std::string> nameMap;
    std::vector<std::string> rootNames;

    for (const auto& item : *prefabObjects) {
        const auto* source = item.as_table();
        if (!source) continue;

        const std::string oldName = (*source)["name"].value_or(std::string{"GameObject"});
        nameMap[oldName] = UniqueName(oldName, usedNames);
    }

    for (const auto& item : *prefabObjects) {
        const auto* source = item.as_table();
        if (!source) continue;

        toml::table copied = *source;
        const std::string oldName = copied["name"].value_or(std::string{"GameObject"});
        const std::string oldParent = copied["parent"].value_or(std::string{});
        const std::string newName = nameMap.contains(oldName) ? nameMap[oldName] : oldName;

        copied.erase("name");
        copied.insert("name", newName);

        copied.erase("parent");
        if (!oldParent.empty() && nameMap.contains(oldParent)) {
            copied.insert("parent", nameMap[oldParent]);
        } else {
            copied.insert("parent", std::string{});
            rootNames.push_back(newName);
        }

        mergedObjects->push_back(std::move(copied));
    }

    std::ostringstream ss;
    ss << merged;
    if (!SceneSerializer::Deserialize(scene, ss.str())) return false;

    for (const std::string& rootName : rootNames) {
        if (auto* go = scene.Find(rootName))
            outRootEntities.push_back(go->GetID());
    }

    FBZZ_LOG_INFO("Instantiated prefab: %s", path.c_str());
    return !outRootEntities.empty();
}

} // namespace fbzz::editor
