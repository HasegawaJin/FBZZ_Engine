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
#include "CacheableLoadedReference.hpp"
#include <limits>
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace fbzz::asset {
namespace {

struct LoadedPlacementReference {
    AssetHandle<MeshInstancesAsset> placements;
    AssetHandle<TerrainPaintAsset> paint;
    std::string resolvedPlacementPath;
};

auto& LoadedPlacementReferences()
{
    /// @note Handles follow hot reload and Unload; index changes invalidate the resolved project paths.
    static std::unordered_map<std::string, LoadedPlacementReference> loaded;
    static uint64_t revision = UINT64_MAX;
    static int assetGeneration = -1;
    const auto currentRevision = AssetDatabase::Revision();
    const auto currentGeneration = AssetManager::GetAssetGeneration();
    if (revision != currentRevision || assetGeneration != currentGeneration) {
        loaded.clear();
        revision = currentRevision;
        assetGeneration = currentGeneration;
    }
    return loaded;
}

} /// @note namespace

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
    std::string text;
    return mesh_instances_serialization::ReadFile(path, text) && DeserializeMeshInstancesAsset(text, path, out);
}

bool DeserializeMeshInstancesAsset(std::string_view text, std::string_view sourcePath, MeshInstancesAsset& out)
{
    using namespace mesh_instances_serialization;
    if (text.size() > 512ull * 1024 * 1024) return false;
    const auto parsed = toml::parse(text, sourcePath);
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

MeshInstancesAsset* LoadMeshInstancesReference(const std::string& reference, std::string* resolvedPath)
{
    if (reference.empty()) return nullptr;
    auto& loaded = LoadedPlacementReferences();
    if (const auto found = loaded.find(reference); found != loaded.end() && !found->second.resolvedPlacementPath.empty()) {
        if (auto* value = AssetManager::Get(found->second.placements)) {
            AssetStore<MeshInstancesAsset>::Get().Pin(found->second.placements);
            if (resolvedPath) *resolvedPath = found->second.resolvedPlacementPath;
            return value;
        }
        loaded.erase(found);
    }
    const auto path = AssetManager::ResolveAssetPath(reference);
    if (util::FileSystem::GetExtension(path) != ".meshinstances") return nullptr;
    const auto handle = AssetManager::Load<MeshInstancesAsset>(reference);
    auto* value = AssetManager::Get(handle);
    if (!value) return nullptr;
    if (resolvedPath) *resolvedPath = path;
    if (loaded.size() < 4096 && detail::IsCacheableLoadedReference(reference, path))
        loaded[reference] = {handle, {}, path};
    return value;
}

std::string FindLoadedMeshInstancesPath(const std::string& reference)
{
    auto& loaded = LoadedPlacementReferences();
    const auto found = loaded.find(reference);
    if (found == loaded.end() || found->second.resolvedPlacementPath.empty()
        || !AssetManager::Get(found->second.placements)) return {};
    return found->second.resolvedPlacementPath;
}

const InstanceSetAsset* ResolveMeshInstanceDefinition(const std::string& reference)
{
    if (reference.empty()) return nullptr;
    auto& loaded = LoadedPlacementReferences();
    const bool guid = AssetDatabase::IsGuidRef(reference);
    {
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
        AssetHandle<MeshInstancesAsset> handle;
        auto& store = AssetStore<MeshInstancesAsset>::Get();
        const auto found = store.cache.find(AssetManager::CanonicalKey(reference));
        if (found != store.cache.end()) {
            handle = found->second;
            /// @note A renderer lookup must not turn an outstanding asynchronous placement request into a synchronous pin.
            if (store.IsPending(handle)) return nullptr;
        }
        if (!AssetManager::Get(handle)) handle = AssetManager::Load<MeshInstancesAsset>(reference);
        const auto* asset = AssetManager::Get(handle);
        std::string resolved;
        if (asset && loaded.size() < 4096 && detail::IsCacheableLoadedReference(reference,
                guid ? std::string_view(path) : std::string_view{}, &resolved)) {
            if (util::FileSystem::GetExtension(resolved) != ".meshinstances") resolved.clear();
            loaded[reference] = {handle, {}, std::move(resolved)};
        }
        return asset ? &asset->instances : nullptr;
    }
    if (extension == ".terrainpaint") {
        const auto handle = AssetManager::Load<TerrainPaintAsset>(reference);
        const auto* asset = AssetManager::Get(handle);
        if (asset && loaded.size() < 4096 && detail::IsCacheableLoadedReference(reference)) loaded[reference].paint = handle;
        return asset ? &asset->definition : nullptr;
    }
    const auto* asset = DataAssetRegistry::Resolve(reference);
    return asset && std::string_view(asset->GetTypeName()) == InstanceSetAsset::TYPE_NAME
        ? static_cast<const InstanceSetAsset*>(asset) : nullptr;
}
} /// @note namespace fbzz::asset
