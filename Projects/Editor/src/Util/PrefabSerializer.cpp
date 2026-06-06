// FBZZ Engine
// PrefabSerializer.cpp | fbzz::editor
// TOML-based prefab save and instantiate helpers
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/Uuid.hpp>
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

    const std::string sceneText = SceneIO::Serialize(scene);
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

    const std::string sceneText = SceneIO::Serialize(scene);
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
    // guidMap: プレファブ内の instanceId (旧) → 新規 UUID (新)
    // WHY: インスタンス化のたびに新しい UUID を割り当てることで、
    //      同一プレファブを複数インスタンス化した場合でも GUID が衝突しない。
    std::unordered_map<std::string, std::string> guidMap;
    std::vector<std::string> rootNames;

    for (const auto& item : *prefabObjects) {
        const auto* source = item.as_table();
        if (!source) continue;

        const std::string oldName = (*source)["name"].value_or(std::string{"GameObject"});
        nameMap[oldName] = UniqueName(oldName, usedNames);

        const std::string oldGuid = (*source)["instanceId"].value_or(std::string{});
        if (!oldGuid.empty())
            guidMap[oldGuid] = util::GenerateUUID();
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

        // instanceId: インスタンスごとに新規 UUID を割り当てる。
        // WHY: 同じプレファブを複数インスタンス化したとき instanceId が重複すると
        //      FindByGuid が誤った GO を返すため、必ず一意の値に置き換える。
        {
            const std::string oldGuid = copied["instanceId"].value_or(std::string{});
            const std::string newGuid = guidMap.contains(oldGuid)
                ? guidMap.at(oldGuid)
                : util::GenerateUUID();
            copied.erase("instanceId");
            copied.insert("instanceId", newGuid);
        }

        // IKSolverComponent: targetName / poleName を nameMap でリマップする。
        // WHY: プレファブを複数インスタンス化すると KneePole_L → KneePole_L (1) のように
        //      名前が変わるが、chains 内の参照文字列を更新しないと Pass 3 が別インスタンスの
        //      Pole を解決してしまい、膝が逆方向に折れる。
        //      nameMap に含まれる (プレファブ内部の) 参照のみ更新し、プレファブ外部を指す
        //      参照はそのまま保持してシーン上の既存オブジェクトを参照させる。
        if (auto* ikTbl = copied["IKSolverComponent"].as_table()) {
            if (auto* chainsArr = (*ikTbl)["chains"].as_array()) {
                for (auto& chainElem : *chainsArr) {
                    auto* chainTbl = chainElem.as_table();
                    if (!chainTbl) continue;

                    // 名前リマップ (nameMap に含まれる = プレファブ内部の参照のみ対象)
                    const std::string oldTarget = (*chainTbl)["targetName"].value_or(std::string{});
                    if (!oldTarget.empty() && nameMap.contains(oldTarget)) {
                        chainTbl->erase("targetName");
                        chainTbl->insert("targetName", nameMap.at(oldTarget));
                    }

                    const std::string oldPole = (*chainTbl)["poleName"].value_or(std::string{});
                    if (!oldPole.empty() && nameMap.contains(oldPole)) {
                        chainTbl->erase("poleName");
                        chainTbl->insert("poleName", nameMap.at(oldPole));
                    }

                    // GUID リマップ (guidMap に含まれる = プレファブ内部の参照のみ対象)
                    const std::string oldTargetGuid = (*chainTbl)["targetGuid"].value_or(std::string{});
                    if (!oldTargetGuid.empty() && guidMap.contains(oldTargetGuid)) {
                        chainTbl->erase("targetGuid");
                        chainTbl->insert("targetGuid", guidMap.at(oldTargetGuid));
                    }

                    const std::string oldPoleGuid = (*chainTbl)["poleGuid"].value_or(std::string{});
                    if (!oldPoleGuid.empty() && guidMap.contains(oldPoleGuid)) {
                        chainTbl->erase("poleGuid");
                        chainTbl->insert("poleGuid", guidMap.at(oldPoleGuid));
                    }
                }
            }
        }

        // BoneComponent: skinnedMeshOwner / skinnedMeshOwnerGuid をリマップする。
        // WHY: BoneComponent が指す SkinnedMeshRenderer オーナーの識別子も
        //      インスタンス化で変わるため、別インスタンスの SMR を指さないよう更新する。
        if (auto* boneTbl = copied["BoneComponent"].as_table()) {
            const std::string oldOwner = (*boneTbl)["skinnedMeshOwner"].value_or(std::string{});
            if (!oldOwner.empty() && nameMap.contains(oldOwner)) {
                boneTbl->erase("skinnedMeshOwner");
                boneTbl->insert("skinnedMeshOwner", nameMap.at(oldOwner));
            }
            const std::string oldOwnerGuid = (*boneTbl)["skinnedMeshOwnerGuid"].value_or(std::string{});
            if (!oldOwnerGuid.empty() && guidMap.contains(oldOwnerGuid)) {
                boneTbl->erase("skinnedMeshOwnerGuid");
                boneTbl->insert("skinnedMeshOwnerGuid", guidMap.at(oldOwnerGuid));
            }
        }

        mergedObjects->push_back(std::move(copied));
    }

    std::ostringstream ss;
    ss << merged;
    if (!SceneIO::Deserialize(scene, ss.str())) return false;

    for (const std::string& rootName : rootNames) {
        if (auto* go = scene.Find(rootName))
            outRootEntities.push_back(go->GetID());
    }

    FBZZ_LOG_INFO("Instantiated prefab: %s", path.c_str());
    return !outRootEntities.empty();
}

} // namespace fbzz::editor
