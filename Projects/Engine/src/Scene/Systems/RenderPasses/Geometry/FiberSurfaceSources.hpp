/// @file    FiberSurfaceSources.hpp
/// @brief   地形パッチと繊維接触履歴のレンダースレッド用キャッシュ。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/FiberInteractorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Transform.hpp>
#include <array>
#include <bit>
#include <limits>
#include <map>
#include <vector>

namespace fbzz::scene {
namespace {
struct FiberTerrainPatch {
    renderer::Mesh m_mesh;
    math::Vector3 m_center{};
    uint64_t m_signature = 0, m_seen = 0;
    uint64_t m_surfaceRevision = UINT64_MAX, m_vegetationRevision = UINT64_MAX;
    uint64_t m_metadataRevision = UINT64_MAX, m_checkedFrame = UINT64_MAX;
    bool m_valid = false, m_generated = false;
};
struct FiberRetiredTerrainPatch {
    renderer::Mesh m_mesh;
    uint64_t m_frame = 0;
};
struct FiberTerrainCache {
    uint64_t m_seen = 0, m_metadataRevision = 0;
    std::array<uint32_t, 7> m_metadata{};
    std::map<std::pair<int, int>, FiberTerrainPatch> m_patches;
    std::vector<FiberRetiredTerrainPatch> m_retired;
    std::vector<renderer::Mesh*> m_visible;
};

struct FiberContactCache {
    FiberContactCB m_data;
    uint64_t m_frame = (std::numeric_limits<uint64_t>::max)();
    float m_time = 0;
    uint32_t m_cursor = 0;
};

/// @note 各ビュー・影パスで二重に進めない。32 接触を循環保持し、過去側は速度出力まで固定する。
void UpdateFiberContacts(FiberContactCache& cache, Scene& scene)
{
    if (cache.m_frame == Time::frameCount) return;
    if (Time::time < cache.m_time || Time::frameCount < cache.m_frame) cache = {};
    cache.m_frame=Time::frameCount;
    cache.m_time=Time::time;
    for (size_t i=0;i<32;++i) {
        cache.m_data.m_centers[i+32]=cache.m_data.m_centers[i];
        cache.m_data.m_times[i+32]=cache.m_data.m_times[i];
    }
    if (scene.GetEntities<FiberInteractorComponent>().empty()) {
        UpdateFiberContactCounts(cache.m_data, Time::time);
        return;
    }
    for (auto& go : scene.GameObjects()) {
        const auto* actor=go.GetComponent<FiberInteractorComponent>();
        if (!go.activeInHierarchy() || !actor || !actor->m_enabled) continue;
        const auto center=go.transform.worldPosition;
        if (!std::isfinite(center.x)||!std::isfinite(center.y)||!std::isfinite(center.z)
            || !std::isfinite(actor->m_radius)||!std::isfinite(actor->m_strength)
            || !std::isfinite(actor->m_recoverySeconds)) continue;
        const float radius=std::clamp(actor->m_radius,0.01f,10.0f);
        size_t slot=32;
        for (size_t i=0;i<32;++i) {
            const auto& old=cache.m_data.m_centers[i];
            if (old.w>0 && (math::Vector3{old.x,old.y,old.z}-center).LengthSq()<radius*radius*0.0625f) {slot=i;break;}
        }
        if (slot==32) slot=cache.m_cursor++%32;
        cache.m_data.m_centers[slot]={center.x,center.y,center.z,radius};
        cache.m_data.m_times[slot]={Time::time,std::clamp(actor->m_strength,0.0f,1.0f),
            std::clamp(actor->m_recoverySeconds,0.05f,30.0f),0};
    }
    UpdateFiberContactCounts(cache.m_data, Time::time);
}

bool FiberTerrainExpired(uint64_t frame, uint64_t seen)
{
    return frame > seen && frame - seen > 2;
}

/// @note 先に抽出した別ビューと GPU の前フレーム参照を保つため、置換・範囲外の面は 2 フレーム保持する。
template<class ReleaseMesh>
void SweepFiberTerrain(FiberTerrainCache& cache, uint64_t frame, ReleaseMesh&& releaseMesh)
{
    cache.m_visible.clear();
    for (auto it = cache.m_patches.begin(); it != cache.m_patches.end();) {
        if (FiberTerrainExpired(frame, it->second.m_seen)) {
            releaseMesh(it->second.m_mesh);
            it = cache.m_patches.erase(it);
        } else ++it;
    }
    std::erase_if(cache.m_retired, [&](auto& retired) {
        if (!FiberTerrainExpired(frame, retired.m_frame)) return false;
        releaseMesh(retired.m_mesh);
        return true;
    });
}

bool FiberTerrainFinite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

/// @note world 球を逆 TRS で local 楕円体へ移す。各軸の投影半径は radius / abs(scale) で回転に依存しない。
/// @see Docs/design/fiber-rendering.md 地形の近傍常駐。
bool FiberTerrainWindow(const TerrainComponent& terrain, const Transform& transform,
    const math::Vector3& camera, float farDistance, int patch, std::array<int, 4>& window)
{
    const auto scale = transform.worldScale;
    const float rotationLength = transform.worldRotation.Length();
    if (!FiberTerrainFinite(camera) || !FiberTerrainFinite(transform.worldPosition) || !FiberTerrainFinite(scale)
        || scale.x == 0 || scale.y == 0 || scale.z == 0 || !std::isfinite(rotationLength)
        || std::abs(rotationLength - 1.0f) > 0.001f || !std::isfinite(farDistance) || farDistance <= 0) return false;
    const auto rotated = transform.worldRotation.Inverse() * (camera - transform.worldPosition);
    const double localX = static_cast<double>(rotated.x) / scale.x;
    const double localZ = static_cast<double>(rotated.z) / scale.z;
    if (!std::isfinite(localX) || !std::isfinite(localZ)) return false;
    const auto axis = [&](double center, float worldScale, int cells, int& first, int& last) {
        const double radius = static_cast<double>(farDistance) / std::abs(static_cast<double>(worldScale));
        const double extent = static_cast<double>(cells) * terrain.cellSize;
        if (center + radius < 0 || center - radius > extent) return false;
        const double step = static_cast<double>(patch) * terrain.cellSize;
        const double maximum = static_cast<double>((cells - 1) / patch);
        first = static_cast<int>(std::clamp(std::floor((center - radius) / step), 0.0, maximum));
        last = static_cast<int>(std::clamp(std::floor((center + radius) / step), 0.0, maximum));
        return true;
    };
    return axis(localX, scale.x, terrain.columns - 1, window[0], window[1])
        && axis(localZ, scale.z, terrain.rows - 1, window[2], window[3]);
}

/// @note 法線は隣接高さを読むため、署名だけ 1 頂点広く採る。植生は指定層に寄与する重みだけ比較する。
bool ReadFiberTerrainPatch(const TerrainComponent& terrain, int x, int z, int nx, int nz,
    int layer, const std::array<uint32_t, 7>& metadata, uint64_t& signature, math::Vector3& center)
{
    signature = 1469598103934665603ull;
    const auto add = [&](uint64_t value) { signature = (std::rotl(signature, 29) ^ value) * 1099511628211ull; };
    for (const auto value : metadata) add(value);
    add(static_cast<uint32_t>(x)); add(static_cast<uint32_t>(z));
    float minHeight = std::numeric_limits<float>::max(), maxHeight = -std::numeric_limits<float>::max();
    for (int gz = std::max(0, z - 1); gz <= std::min(terrain.rows - 1, z + nz + 1); ++gz) {
        for (int gx = std::max(0, x - 1); gx <= std::min(terrain.columns - 1, x + nx + 1); ++gx) {
            const float height = terrain.heightData[static_cast<size_t>(gz) * terrain.columns + gx] * terrain.maxHeight;
            if (!std::isfinite(height)) return false;
            add(std::bit_cast<uint32_t>(height));
            if (gx >= x && gx <= x + nx && gz >= z && gz <= z + nz) {
                minHeight = std::min(minHeight, height); maxHeight = std::max(maxHeight, height);
                if (layer >= 0) add(std::bit_cast<uint32_t>(terrain.GetLayerWeightAtGrid(gx, gz, layer)));
            }
        }
    }
    const bool holes = terrain.holeData.size() == terrain.CellCount();
    for (int gz = z; gz < z + nz; ++gz) for (int gx = x; gx < x + nx; ++gx)
        add(holes && terrain.IsHoleCell(gx, gz) ? 1 : 0);
    center = {(static_cast<float>(x) * terrain.cellSize + static_cast<float>(x + nx) * terrain.cellSize) * 0.5f,
        (minHeight + maxHeight) * 0.5f,
        (static_cast<float>(z) * terrain.cellSize + static_cast<float>(z + nz) * terrain.cellSize) * 0.5f};
    return FiberTerrainFinite(center);
}

void GenerateFiberTerrainPatch(renderer::Mesh& mesh, const TerrainComponent& terrain,
    int x, int z, int nx, int nz, int layer, float threshold, renderer::ResourceManager& resources)
{
    const bool holes = terrain.holeData.size() == terrain.CellCount();
    const auto weightAt = [&](int gx, int gz) { return layer < 0 ? 1.0f : terrain.GetLayerWeightAtGrid(gx, gz, layer); };
    for (int iz = 0; iz < nz; ++iz) for (int ix = 0; ix < nx; ++ix) {
        const int cx = x + ix, cz = z + iz;
        if (holes && terrain.IsHoleCell(cx, cz)) continue;
        if (layer >= 0 && std::max({weightAt(cx, cz), weightAt(cx + 1, cz), weightAt(cx, cz + 1), weightAt(cx + 1, cz + 1)}) < threshold) continue;
        const uint32_t a = static_cast<uint32_t>(iz * (nx + 1) + ix), b = a + static_cast<uint32_t>(nx + 1);
        mesh.cpuIndices.insert(mesh.cpuIndices.end(), {a, b, a + 1, a + 1, b, b + 1});
    }
    if (mesh.cpuIndices.empty()) return;
    mesh.cpuVertices.reserve(static_cast<size_t>(nx + 1) * (nz + 1));
    for (int iz = 0; iz <= nz; ++iz) for (int ix = 0; ix <= nx; ++ix) {
        renderer::Vertex vertex;
        vertex.position = {static_cast<float>(x + ix) * terrain.cellSize,
            terrain.heightData[static_cast<size_t>(z + iz) * terrain.columns + x + ix] * terrain.maxHeight,
            static_cast<float>(z + iz) * terrain.cellSize};
        vertex.normal = terrain.ComputeNormal(x + ix, z + iz);
        vertex.tangent = {1, 0, 0};
        vertex.uv = {vertex.position.x, vertex.position.z};
        vertex.color.w = weightAt(x + ix, z + iz);
        if (!FiberTerrainFinite(vertex.position) || !FiberTerrainFinite(vertex.normal)) {
            mesh = {};
            return;
        }
        mesh.cpuVertices.push_back(vertex);
    }
    mesh.ComputeBounds();
    mesh.vertexCount = static_cast<uint32_t>(mesh.cpuVertices.size());
    mesh.indexCount = static_cast<uint32_t>(mesh.cpuIndices.size());
    mesh.vertexBuffer = resources.CreateVertexBuffer(mesh.cpuVertices.data(), mesh.cpuVertices.size() * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
    mesh.indexBuffer = resources.CreateIndexBuffer(mesh.cpuIndices.data(), mesh.indexCount);
}

/// @brief 閲覧カメラの距離 LOD に入る地形パッチだけを生成し、別ビューの要求を同じ常駐キャッシュへ追加する。
/// @note 穴・保存層の判定と頂点順序・seed は全域生成時と同じ。錐台で省かず、画面外の投影影も保つ。
/// @see Docs/design/fiber-rendering.md 地形の近傍常駐。
void UpdateFiberTerrain(FiberTerrainCache& cache, const TerrainComponent& terrain, int patchCells,
    int terrainLayer, float layerThreshold, const Transform& transform, const math::Vector3& camera,
    bool distanceLod, float nearDistance, float farDistance, renderer::ResourceManager& resources)
{
    const uint64_t frame = resources.FrameStamp();
    cache.m_seen = frame;
    cache.m_visible.clear();
    if (terrain.columns < 2 || terrain.rows < 2 || terrain.columns > 4097 || terrain.rows > 4097
        || terrain.heightData.size() != static_cast<size_t>(terrain.columns) * terrain.rows
        || !std::isfinite(terrain.cellSize) || terrain.cellSize <= 0 || !std::isfinite(terrain.maxHeight)) return;
    const int patch = std::clamp(patchCells, 1, 32);
    const int layer = terrainLayer < 0 ? -1 : terrainLayer;
    const float threshold = std::isfinite(layerThreshold) ? std::clamp(layerThreshold, 0.0f, 1.0f) : 0.0f;
    const std::array<uint32_t, 7> metadata{static_cast<uint32_t>(terrain.columns), static_cast<uint32_t>(terrain.rows),
        static_cast<uint32_t>(patch), static_cast<uint32_t>(layer), std::bit_cast<uint32_t>(terrain.cellSize),
        std::bit_cast<uint32_t>(terrain.maxHeight), std::bit_cast<uint32_t>(threshold)};
    if (metadata != cache.m_metadata) { cache.m_metadata = metadata; ++cache.m_metadataRevision; }
    std::array<int, 4> window{0, (terrain.columns - 2) / patch, 0, (terrain.rows - 2) / patch};
    if (distanceLod) {
        if (!std::isfinite(nearDistance) || !std::isfinite(farDistance)) return;
        farDistance = std::max(farDistance, std::max(nearDistance, 0.0f) + 0.1f);
        if (!FiberTerrainWindow(terrain, transform, camera, farDistance, patch, window)) return;
    }
    const auto world = transform.GetWorldMatrix();
    for (int pz = window[2]; pz <= window[3]; ++pz) for (int px = window[0]; px <= window[1]; ++px) {
        const int x = px * patch, z = pz * patch;
        const int nx = std::min(patch, terrain.columns - 1 - x), nz = std::min(patch, terrain.rows - 1 - z);
        auto& entry = cache.m_patches[{x, z}];
        entry.m_seen = frame;
        const bool revisionsChanged = entry.m_metadataRevision != cache.m_metadataRevision
            || entry.m_surfaceRevision != terrain.surfaceRevision
            || (layer >= 0 && entry.m_vegetationRevision != terrain.vegetationRevision);
        if (revisionsChanged || (entry.m_checkedFrame != frame && (terrain.heightDirty || (layer >= 0 && terrain.splatDirty)))) {
            uint64_t signature = 0;
            math::Vector3 center{};
            const bool valid = ReadFiberTerrainPatch(terrain, x, z, nx, nz, layer, metadata, signature, center);
            if (signature != entry.m_signature || valid != entry.m_valid) {
                if (entry.m_mesh.vertexBuffer.IsValid() || entry.m_mesh.indexBuffer.IsValid())
                    cache.m_retired.push_back({std::move(entry.m_mesh), frame});
                entry.m_mesh = {};
                entry.m_generated = false;
            }
            entry.m_signature = signature; entry.m_center = center; entry.m_valid = valid;
            entry.m_checkedFrame = frame; entry.m_metadataRevision = cache.m_metadataRevision;
            entry.m_surfaceRevision = terrain.surfaceRevision; entry.m_vegetationRevision = terrain.vegetationRevision;
        }
        if (!entry.m_valid) continue;
        if (distanceLod) {
            const auto center = world * math::Vector4{entry.m_center.x, entry.m_center.y, entry.m_center.z, 1};
            const math::Vector3 worldCenter{center.x, center.y, center.z};
            if (!FiberTerrainFinite(worldCenter) || (worldCenter - camera).Length() >= farDistance) continue;
        }
        if (!entry.m_generated) {
            GenerateFiberTerrainPatch(entry.m_mesh, terrain, x, z, nx, nz, layer, threshold, resources);
            entry.m_generated = true;
        }
        if (entry.m_mesh.vertexBuffer.IsValid() && entry.m_mesh.indexBuffer.IsValid()) cache.m_visible.push_back(&entry.m_mesh);
    }
}
} /// @note namespace
} /// @note namespace fbzz::scene
