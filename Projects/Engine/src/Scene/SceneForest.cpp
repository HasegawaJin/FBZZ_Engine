/// @file    SceneForest.cpp
/// @brief   Passive preparation and checked publication of closed object hierarchies.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <Engine/Scene/SceneForest.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/MeshResolver.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <toml++/toml.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fbzz::scene {
namespace {

struct ForestObject {
    std::string id;
    std::string name;
    std::string parent;
};

const std::unordered_set<std::string>& ObjectKeys()
{
    static const auto keys = [] {
        std::unordered_set<std::string> result{"name", "tag", "layer", "active", "instanceId", "parent",
            "parentInstanceId", "prefabAssetPath", "prefabSourceId", "prefabSourceSnapshot", "transform", "ScriptComponents",
            "ForceField", "ParticleForceField"};
        ForEachRegisteredComponent([&]<typename T, typename Registration>() {
            if constexpr (!std::is_same_v<T, ScriptComponent> && !std::is_same_v<T, ProceduralMeshComponent>)
                result.emplace(Registration::serializedName);
        });
        return result;
    }();
    return keys;
}

bool FiniteValues(const toml::node& node, unsigned depth = 0)
{
    if (depth > 64) return false;
    if (const auto* value = node.as_floating_point())
        return std::isfinite(value->get()) && std::abs(value->get()) <= (std::numeric_limits<float>::max)();
    if (const auto* array = node.as_array()) {
        for (const auto& element : *array)
            if (!FiniteValues(element, depth + 1)) return false;
    }
    if (const auto* table = node.as_table()) {
        for (const auto& [key, value] : *table) {
            (void)key;
            if (!FiniteValues(value, depth + 1)) return false;
        }
    }
    return true;
}

bool ValidateArray(const toml::table& table, const char* key, size_t count)
{
    const auto* node = table.get(key);
    if (!node) return true;
    const auto* values = node->as_array();
    if (!values || values->size() != count) return false;
    for (const auto& value : *values)
        if (!value.is_number()) return false;
    return true;
}

bool ValidateObjectFields(const toml::table& table, std::string& error)
{
    for (const auto& [key, value] : table) {
        const auto name = key.str();
        if (!ObjectKeys().contains(std::string(name))) {
            error = "Unsupported forest object field or component: " + std::string(name);
            return false;
        }
        bool valid = false;
        if (name == "name" || name == "tag" || name == "instanceId" || name == "parent"
            || name == "parentInstanceId" || name == "prefabAssetPath" || name == "prefabSourceId"
            || name == "prefabSourceSnapshot") valid = value.is_string();
        else if (name == "layer") valid = value.is_integer();
        else if (name == "active") valid = value.is_boolean();
        else if (name == "ScriptComponents") {
            if (const auto* scripts = value.as_array()) {
                valid = true;
                std::unordered_set<std::string> identities;
                for (const auto& script : *scripts) {
                    const auto* fields = script.as_table();
                    const auto type = fields ? (*fields)["type"].value<std::string>() : std::nullopt;
                    if (!type || type->empty()) { valid = false; break; }
                    if (const auto* enabled = fields->get("enabled"); enabled && !enabled->is_boolean()) valid = false;
                    if (const auto* settings = fields->get("fields"); settings && !settings->is_table()) valid = false;
                    if (const auto* idNode = fields->get("savedScriptId")) {
                        const auto id = idNode->value<std::string>();
                        /// @note This format predicate has no registry, factory or mutable global access.
                        if (!id || !ScriptFactory::IsValidSavedScriptId(*id) || !identities.insert(*id).second) valid = false;
                    }
                    if (!valid) break;
                }
            }
        } else valid = value.is_table();
        if (!valid) {
            error = "Invalid forest object field or component value: " + std::string(name);
            return false;
        }
    }
    if (const auto* terrain = table["TerrainComponent"].as_table()) {
        if (const auto* inlineData = terrain->get("inlineData"); inlineData && !inlineData->is_table()) {
            error = "Inline Terrain data must be a table.";
            return false;
        }
    }
    if (const auto* grid = table["TerrainGridComponent"].as_table()) {
        constexpr int64_t maxGridCells = 1'048'576;
        const auto dimension = [&](const char* key) {
            const auto* node = grid->get(key);
            return node ? node->value<int64_t>().value_or(int64_t{0}) : int64_t{4};
        };
        const auto x = dimension("cellCountX"), z = dimension("cellCountZ");
        if (x < 1 || z < 1 || x > maxGridCells || z > maxGridCells || x > maxGridCells / z) {
            error = "TerrainGrid dimensions must contain between 1 and 1048576 cells.";
            return false;
        }
        if (const auto* cellsNode = grid->get("cells")) {
            const auto* cells = cellsNode->as_array();
            if (!cells || cells->size() > static_cast<size_t>(x * z)) {
                error = "TerrainGrid cells must be a string array fitting the grid dimensions.";
                return false;
            }
            for (const auto& cell : *cells) {
                if (!cell.is_string()) {
                    error = "TerrainGrid saved cell identities must be strings.";
                    return false;
                }
            }
        }
    }
    return true;
}

bool DestinationAccepts(Scene& destination, const std::vector<ForestObject>& objects, std::string& error)
{
    std::unordered_set<std::string> identities;
    identities.reserve(destination.GameObjectCount() + objects.size());
    for (const auto& object : destination.GameObjects()) {
        if (!object.instanceId.empty() && !identities.insert(object.instanceId).second) {
            error = "Destination contains duplicate saved object identities.";
            return false;
        }
    }
    for (const auto& object : objects) {
        if (!identities.insert(object.id).second) {
            error = "Forest object identity already exists in the destination: " + object.id;
            return false;
        }
    }
    if (!destination.CanCreateGameObjects(objects.size())) {
        error = "Insufficient Scene entity capacity for the complete forest.";
        return false;
    }
    return true;
}

bool ValidateDocument(toml::table& document,
    std::vector<ForestObject>& objects, std::string& error)
{
    auto* entries = document["gameobjects"].as_array();
    if (!entries || entries->empty()) {
        error = "Forest requires a nonempty gameobjects array.";
        return false;
    }
    std::unordered_map<std::string, size_t> ids;
    std::unordered_map<std::string, std::vector<size_t>> names;
    objects.reserve(entries->size());
    for (const auto& entry : *entries) {
        const auto* table = entry.as_table();
        if (!table || !FiniteValues(entry)) {
            error = "Forest objects must be tables containing finite numeric values.";
            return false;
        }
        if (table->contains("WorldPartitionComponent") || table->contains("WorldPartition")) {
            error = "A streamed forest cannot contain a nested world-partition controller.";
            return false;
        }
        if (!ValidateObjectFields(*table, error)) return false;
        const auto id = (*table)["instanceId"].value<std::string>();
        if (!id || id->empty() || !ids.emplace(*id, objects.size()).second) {
            error = "Every forest object requires a unique nonempty saved identity.";
            return false;
        }
        if (const auto* field = table->get("name"); field && !field->is_string()) {
            error = "Forest object names must be strings.";
            return false;
        }
        const auto name = (*table)["name"].value_or(std::string{"GameObject"});
        if (name.starts_with("__")) {
            error = "Reserved runtime-only object names cannot be persisted in a forest.";
            return false;
        }
        for (const char* key : {"parent", "parentInstanceId"}) {
            if (const auto* field = table->get(key); field && !field->is_string()) {
                error = "Forest parent identities and names must be strings.";
                return false;
            }
        }
        if (const auto* node = table->get("transform")) {
            const auto* transform = node->as_table();
            if (!transform || !ValidateArray(*transform, "position", 3) || !ValidateArray(*transform, "localPosition", 3)
                || !ValidateArray(*transform, "rotation", 4) || !ValidateArray(*transform, "localRotation", 4)
                || !ValidateArray(*transform, "scale", 3) || !ValidateArray(*transform, "localScale", 3)) {
                error = "Forest transforms require numeric position/scale triples and quaternion quadruples.";
                return false;
            }
        }
        names[name].push_back(objects.size());
        objects.push_back({*id, name, (*table)["parentInstanceId"].value_or(std::string{})});
    }

    std::vector<size_t> parents(objects.size(), objects.size());
    for (size_t index = 0; index < objects.size(); ++index) {
        auto& object = objects[index];
        auto* table = (*entries)[index].as_table();
        if (object.parent.empty()) {
            const auto legacyName = (*table)["parent"].value_or(std::string{});
            if (!legacyName.empty()) {
                const auto found = names.find(legacyName);
                if (found == names.end() || found->second.size() != 1) {
                    error = "A legacy parent name must identify exactly one object inside the forest.";
                    return false;
                }
                object.parent = objects[found->second.front()].id;
            }
        }
        if (!object.parent.empty()) {
            const auto parent = ids.find(object.parent);
            if (parent == ids.end()) {
                error = "A forest parent cannot be outside its owning document: " + object.parent;
                return false;
            }
            parents[index] = parent->second;
            table->insert_or_assign("parent", objects[parent->second].name);
        } else {
            table->insert_or_assign("parent", std::string{});
        }
        table->insert_or_assign("parentInstanceId", object.parent);
    }
    std::vector<uint8_t> marks(objects.size());
    for (size_t start = 0; start < objects.size(); ++start) {
        std::vector<size_t> path;
        size_t current = start;
        while (current < objects.size() && marks[current] == 0) {
            marks[current] = 1;
            path.push_back(current);
            current = parents[current];
        }
        if (current < objects.size() && marks[current] == 1) {
            error = "Forest hierarchy contains a parent cycle.";
            return false;
        }
        for (const auto index : path) marks[index] = 2;
    }
    return true;
}

bool ParseDocument(const std::string& document, toml::table& normalized,
    std::vector<ForestObject>& objects, std::string& error)
{
    auto parsed = toml::parse(document);
    if (!parsed) {
        error = "Invalid forest TOML: " + std::string(parsed.error().description());
        return false;
    }
    normalized = std::move(parsed.table());
    return ValidateDocument(normalized, objects, error);
}

} /// @note namespace

bool ValidateSceneForestDocument(const std::string& document, std::vector<std::string>& outIds, std::string& error)
{
    outIds.clear();
    error.clear();
    toml::table normalized;
    std::vector<ForestObject> objects;
    if (!ParseDocument(document, normalized, objects, error)) return false;
    outIds.reserve(objects.size());
    for (const auto& object : objects) outIds.push_back(object.id);
    return true;
}

bool AppendSceneForest(Scene& destination, const std::string& document, const std::string& sourcePath,
    renderer::ResourceManager* resources, std::vector<EntityID>& outRoots,
    std::vector<EntityID>& outEntities, std::string& error)
{
    outRoots.clear();
    outEntities.clear();
    error.clear();
    toml::table normalized;
    std::vector<ForestObject> objects;
    if (!ParseDocument(document, normalized, objects, error)
        || !DestinationAccepts(destination, objects, error)) return false;
    std::ostringstream encoded;
    encoded << normalized;
    auto prepared = SceneSerializer::LoadPassiveDataFromText(encoded.str(), sourcePath);
    if (!prepared || prepared->GameObjectCount() != objects.size()) {
        error = "Forest preparation failed; check component assets and saved Script identities.";
        return false;
    }
    if (!DestinationAccepts(destination, objects, error)) return false;

    std::unordered_map<std::string, EntityID> preparedIds;
    for (const auto& object : prepared->GameObjects()) preparedIds.emplace(object.instanceId, object.GetID());
    std::unordered_map<std::string, EntityID> publishedIds;
    std::vector<std::pair<EntityID, EntityID>> replacements;
    replacements.reserve(objects.size());
    const auto rollback = [&] {
        destination.DestroyGameObjects(outEntities);
        outEntities.clear();
        outRoots.clear();
    };
    for (const auto& identity : objects) {
        auto* source = prepared->GetGameObject(preparedIds.at(identity.id));
        auto* target = destination.TryCreateGameObject(source->name);
        if (!target) {
            rollback();
            error = "Scene capacity changed while publishing the forest.";
            return false;
        }
        outEntities.push_back(target->GetID());
        publishedIds.emplace(identity.id, target->GetID());
        replacements.emplace_back(source->GetID(), target->GetID());
        target->instanceId = source->instanceId;
        target->tag = source->tag;
        target->layer = source->layer;
        target->prefabAssetPath = source->prefabAssetPath;
        target->prefabSourceId = source->prefabSourceId;
        target->prefabSourceSnapshot = source->prefabSourceSnapshot;
        target->transform.position = source->transform.position;
        target->transform.rotation = source->transform.rotation;
        target->transform.scale = source->transform.scale;
        target->SetActive(source->activeSelf());
    }
    for (const auto& identity : objects) {
        auto* target = destination.GetGameObject(publishedIds.at(identity.id));
        if (identity.parent.empty()) outRoots.push_back(target->GetID());
        else if (!target->SetParent(destination.GetGameObject(publishedIds.at(identity.parent)))) {
            rollback();
            error = "Forest hierarchy changed while publishing its parents.";
            return false;
        }
    }
    for (const auto& [source, target] : replacements)
        destination.CopyComponentsFromPassive(*prepared, source, target);
    RemapCopiedObjectReferences(*prepared, destination, replacements, outEntities);
    for (const auto entity : outEntities) {
        auto* object = destination.GetGameObject(entity);
        if (auto* grid = object->GetComponent<TerrainGridComponent>()) grid->ResolveFromScene(destination);
        if (resources) {
            if (auto* mesh = object->GetComponent<MeshRenderer>(); mesh && !mesh->mesh && !mesh->meshPath.empty())
                mesh->mesh = ResolveMeshPath(mesh->meshPath, *resources);
            if (auto* skin = object->GetComponent<SkinnedMeshRenderer>(); skin && !skin->model && !skin->modelPath.empty())
                skin->model = asset::AssetManager::LoadAndGet<asset::Model>(skin->modelPath);
        }
    }
    FlushWorldTransforms(destination);
    destination.PreparePassiveCopiedScripts(outEntities);
    destination.FinalizePassiveCopiedScripts(outEntities);
    return true;
}

} /// @note namespace fbzz::scene
