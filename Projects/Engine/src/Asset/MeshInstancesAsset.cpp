/// @file    MeshInstancesAsset.cpp
/// @brief   .meshinstances の検証付き入出力と旧配置アセットの互換解決。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Asset/TerrainPaintAsset.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include "MeshInstancesSerialization.hpp"
#include <limits>
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace fbzz::asset {

bool MeshInstanceDefinitionsEqual(const InstanceSetAsset& first, const InstanceSetAsset& second)
{
    if (first.Parts().size() != second.Parts().size() || first.LodHeights().size() != second.LodHeights().size()
        || !std::equal(first.LodHeights().begin(), first.LodHeights().end(), second.LodHeights().begin())) return false;
    for (size_t i = 0; i < first.Parts().size(); ++i) {
        const auto& a = first.Parts()[i]; const auto& b = second.Parts()[i];
        if (a.meshPath != b.meshPath || a.materialPath != b.materialPath || a.lod != b.lod) return false;
    }
    const auto a = first.Snapshot(), b = second.Snapshot();
    if (!a || !b) return a == b;
    if (a->placements.size() != b->placements.size()) return false;
    for (size_t i = 0; i < a->placements.size(); ++i) {
        const auto& x = a->placements[i]; const auto& y = b->placements[i];
        if (x.id != y.id || x.position != y.position || x.eulerRadians != y.eulerRadians || x.scale != y.scale || x.active != y.active) return false;
    }
    return true;
}

bool MeshInstancesAssetsEqual(const MeshInstancesAsset& first, const MeshInstancesAsset& second)
{
    return first.terrainPaintRef == second.terrainPaintRef && MeshInstanceDefinitionsEqual(first.instances, second.instances);
}

bool LoadMeshInstancesAsset(std::string_view path, MeshInstancesAsset& out)
{
    using namespace mesh_instances_serialization;
    std::string text;
    if (!ReadFile(path, text)) return false;
    const auto parsed = toml::parse(text, std::string(path));
    if (!parsed || parsed["format"].value<std::string>() != "fbzz.meshinstances" || parsed["version"].value<int64_t>() != 1) return false;
    MeshInstancesAsset candidate;
    const auto source = parsed["terrainPaintRef"].value<std::string>();
    if (!source) return false;
    candidate.terrainPaintRef = *source;
    std::vector<InstanceMeshPart> parts;
    std::vector<float> heights;
    if (!ReadDefinition(parsed.table(), parts, heights)) return false;
    const auto* placements = parsed["placements"].as_table();
    const auto* ids = placements ? (*placements)["ids"].as_array() : nullptr;
    const auto* positions = placements ? (*placements)["positions"].as_array() : nullptr;
    const auto* rotations = placements ? (*placements)["rotations"].as_array() : nullptr;
    const auto* scales = placements ? (*placements)["scales"].as_array() : nullptr;
    if (!ids || ids->size() > 1048576 || !positions || !rotations || !scales
        || positions->size() != ids->size() || rotations->size() != ids->size() || scales->size() != ids->size()) return false;
    std::vector<renderer::InstancePlacement> values;
    values.reserve(ids->size());
    for (size_t i = 0; i < ids->size(); ++i) {
        renderer::InstancePlacement value;
        const auto id = (*ids)[i].value<int64_t>();
        if (!id || *id <= 0 || *id > std::numeric_limits<int>::max()
            || !ReadVector((*positions)[i], value.position) || !ReadVector((*rotations)[i], value.eulerRadians)
            || !ReadVector((*scales)[i], value.scale)) return false;
        value.id = static_cast<uint32_t>(*id); value.active = true;
        values.push_back(value);
    }
    if (!candidate.instances.Replace(parts, heights, values)) return false;
    out = std::move(candidate);
    return true;
}

bool SaveMeshInstancesAsset(std::string_view path, const MeshInstancesAsset& asset)
{
    const auto snapshot = asset.instances.Snapshot();
    if (!snapshot) return false;
    toml::table table{{"format", "fbzz.meshinstances"}, {"version", 1}, {"terrainPaintRef", asset.terrainPaintRef}};
    mesh_instances_serialization::WriteDefinition(table, asset.instances);
    toml::array ids, positions, rotations, scales;
    for (const auto& value : snapshot->placements) {
        if (!value.active) continue;
        ids.push_back(value.id);
        positions.push_back(mesh_instances_serialization::WriteVector(value.position));
        rotations.push_back(mesh_instances_serialization::WriteVector(value.eulerRadians));
        scales.push_back(mesh_instances_serialization::WriteVector(value.scale));
    }
    table.insert("placements", toml::table{{"ids", std::move(ids)}, {"positions", std::move(positions)},
        {"rotations", std::move(rotations)}, {"scales", std::move(scales)}});
    return mesh_instances_serialization::WriteFile(path, table);
}

const InstanceSetAsset* ResolveMeshInstanceDefinition(const std::string& reference)
{
    if (reference.empty()) return nullptr;
    struct Loaded {
        AssetHandle<MeshInstancesAsset> placements;
        AssetHandle<TerrainPaintAsset> paint;
    };
    /// @note 世代付き handle のみを保持し、再読込後の実体は毎回 Get する。GUID 索引の変更で移動・プロジェクト切替を反映する。
    static std::unordered_map<std::string, Loaded> loaded;
    static uint64_t revision = UINT64_MAX;
    const auto currentRevision = AssetDatabase::Revision();
    if (revision != currentRevision) { loaded.clear(); revision = currentRevision; }
    const bool guid = AssetDatabase::IsGuidRef(reference);
    if (guid) {
        if (const auto found = loaded.find(reference); found != loaded.end()) {
            if (const auto* value = AssetManager::Get(found->second.placements)) return &value->instances;
            if (const auto* value = AssetManager::Get(found->second.paint)) return &value->definition;
            loaded.erase(found);
        }
    }
    /// @note 旧 .fzdata の温まった Registry 解決に、ディスク確認を追加しない。
    auto path = guid ? AssetDatabase::PathFromGuid(AssetDatabase::GuidFromRef(reference)) : reference;
    if (path.empty()) path = AssetDatabase::HintFromRef(reference);
    auto extension = util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(path).extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension == ".meshinstances") {
        const auto handle = AssetManager::Load<MeshInstancesAsset>(reference);
        const auto* asset = AssetManager::Get(handle);
        if (asset && guid && loaded.size() < 4096) loaded[reference].placements = handle;
        return asset ? &asset->instances : nullptr;
    }
    if (extension == ".terrainpaint") {
        const auto handle = AssetManager::Load<TerrainPaintAsset>(reference);
        const auto* asset = AssetManager::Get(handle);
        if (asset && guid && loaded.size() < 4096) loaded[reference].paint = handle;
        return asset ? &asset->definition : nullptr;
    }
    const auto* asset = DataAssetRegistry::Resolve(reference);
    return asset && std::string_view(asset->GetTypeName()) == InstanceSetAsset::TYPE_NAME
        ? static_cast<const InstanceSetAsset*>(asset) : nullptr;
}
} /// @note namespace fbzz::asset
