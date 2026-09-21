/// @file    RenderTerrainExtractor.cpp
/// @brief   TerrainComponent → GPU チャンクメッシュ生成・描画 (IRenderPass 実装)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// @note 枠 (TerrainSurface.hlsli と同期): t0 = 層番号マップ、t1 = 重みマップ、t13 = 影。層テクスチャと層パラメーターは
/// @note StructuredBuffer (TerrainLayerGpu) と bindless 添字で渡す。
/// @note シングルスレッド前提。GPU 資源は ResourceHandle で所有し、static map でエンティティごとにキャッシュする。
/// @note heightDirty (穴を含む) でチャンクを、splatDirty で番号 / 重みマップを作り直す。
/// @see Docs/design/terrain-layers.md
#include "Engine/Scene/Systems/RenderPasses/Geometry/TerrainRenderPass.hpp"
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include <Engine/Scene/Systems/RenderTerrainExtractor.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include "Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/IStructuredBuffer.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include "Engine/Scene/Components/TerrainGridComponent.hpp"
#include "Engine/Scene/Entity.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/Camera.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/RenderSettings.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include <Math/Frustum.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Physics/Layer.hpp>
#include <array>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::scene {

/// @note GPU 頂点レイアウト。HLSL の TerrainVSInput と完全に一致させること。
/// @note POSITION  : float3  offset  0  (12 bytes)
/// @note NORMAL    : float3  offset 12  (12 bytes)
/// @note TANGENT   : float3  offset 24  (12 bytes)
/// @note TEXCOORD0 : float2  offset 36  ( 8 bytes)
/// @note stride = 44 bytes
struct TerrainVertex {
    math::Vector3 position;
    math::Vector3 normal;
    math::Vector3 tangent;
    math::Vector2 uv;
};

static constexpr int kLODCount            = 3;
static constexpr int kLODSteps[kLODCount] = { 1, 2, 4 };

using TerrainChunk = renderer::RenderTerrainPatch;

struct TerrainChunkKey {
    EntityID entityId;
    uint32_t chunkX = 0;
    uint32_t chunkZ = 0;
    bool operator==(const TerrainChunkKey& o) const = default;
};

} /// @note namespace fbzz::scene

template<>
struct std::hash<fbzz::scene::TerrainChunkKey> {
    size_t operator()(const fbzz::scene::TerrainChunkKey& k) const noexcept
    {
        size_t h = std::hash<uint32_t>{}(k.entityId.index);
        h ^= std::hash<uint32_t>{}(k.entityId.generation) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.chunkX)              + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.chunkZ)              + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

namespace fbzz::scene {

/// @brief 地形 1 つぶんの GPU 資源。層テクスチャは LoadTexture のパスキャッシュが所有するので持たない。
struct TerrainGpuState {
    renderer::ResourceHandle<renderer::TextureTag> splatIndices;   ///< @note 所有
    renderer::ResourceHandle<renderer::TextureTag> splatWeights;   ///< @note 所有
    renderer::ResourceHandle<renderer::StructuredBufferTag> layerBuffer; ///< @note 所有
    uint32_t layerCapacity = 0;
    /// @note 直前に転送したバイト列。同じなら Update を省く。
    std::vector<TerrainLayerGpu> uploadedLayers;
};

static std::unordered_map<TerrainChunkKey, TerrainChunk> g_chunkCache;
static std::unordered_map<uint32_t, TerrainGpuState>     g_gpuCache;

struct TerrainNeighbors {
    const TerrainComponent* north = nullptr;
    const TerrainComponent* south = nullptr;
    const TerrainComponent* west  = nullptr;
    const TerrainComponent* east  = nullptr;
    const TerrainComponent* northWest = nullptr;
    const TerrainComponent* northEast = nullptr;
    const TerrainComponent* southWest = nullptr;
    const TerrainComponent* southEast = nullptr;
};

static bool TryAddTerrainHeightSample(
    const TerrainComponent* terrain,
    int                     x,
    int                     z,
    float&                  sum,
    int&                    count)
{
    if (!terrain || terrain->heightData.empty())
        return false;
    if (x < 0 || x >= terrain->columns || z < 0 || z >= terrain->rows)
        return false;

    const size_t idx = static_cast<size_t>(z) * static_cast<size_t>(terrain->columns)
                     + static_cast<size_t>(x);
    sum += terrain->heightData[idx] * terrain->maxHeight;
    ++count;
    return true;
}

static float SampleStitchedTerrainHeight(
    const TerrainComponent& terrain,
    const TerrainNeighbors& neighbors,
    int                     x,
    int                     z)
{
    /// @note TerrainGrid の境界では複数 Terrain が同じワールド頂点を持つ。描画用メッシュでは
    /// @note その共有頂点を平均し、保存データを破壊せずに隙間を隠す。角は最大 4 Terrain が
    /// @note 交差するため、斜め隣接も平均に含める。
    float sum = 0.0f;
    int count = 0;

    TryAddTerrainHeightSample(&terrain, x, z, sum, count);

    if (x <= 0) {
        TryAddTerrainHeightSample(neighbors.west, neighbors.west ? neighbors.west->columns - 1 + x : x, z, sum, count);
    }
    if (x >= terrain.columns - 1) {
        TryAddTerrainHeightSample(neighbors.east, x - (terrain.columns - 1), z, sum, count);
    }
    if (z <= 0) {
        TryAddTerrainHeightSample(neighbors.north, x, neighbors.north ? neighbors.north->rows - 1 + z : z, sum, count);
    }
    if (z >= terrain.rows - 1) {
        TryAddTerrainHeightSample(neighbors.south, x, z - (terrain.rows - 1), sum, count);
    }

    if (x <= 0 && z <= 0) {
        TryAddTerrainHeightSample(
            neighbors.northWest,
            neighbors.northWest ? neighbors.northWest->columns - 1 + x : x,
            neighbors.northWest ? neighbors.northWest->rows - 1 + z : z,
            sum,
            count);
    }
    if (x >= terrain.columns - 1 && z <= 0) {
        TryAddTerrainHeightSample(
            neighbors.northEast,
            x - (terrain.columns - 1),
            neighbors.northEast ? neighbors.northEast->rows - 1 + z : z,
            sum,
            count);
    }
    if (x <= 0 && z >= terrain.rows - 1) {
        TryAddTerrainHeightSample(
            neighbors.southWest,
            neighbors.southWest ? neighbors.southWest->columns - 1 + x : x,
            z - (terrain.rows - 1),
            sum,
            count);
    }
    if (x >= terrain.columns - 1 && z >= terrain.rows - 1) {
        TryAddTerrainHeightSample(
            neighbors.southEast,
            x - (terrain.columns - 1),
            z - (terrain.rows - 1),
            sum,
            count);
    }

    if (count > 0)
        return sum / static_cast<float>(count);

    const int clampedX = std::clamp(x, 0, terrain.columns - 1);
    const int clampedZ = std::clamp(z, 0, terrain.rows - 1);
    const size_t idx = static_cast<size_t>(clampedZ) * static_cast<size_t>(terrain.columns)
                     + static_cast<size_t>(clampedX);
    return terrain.heightData[idx] * terrain.maxHeight;
}

static math::Vector3 ComputeStitchedNormal(
    const TerrainComponent& terrain,
    const TerrainNeighbors& neighbors,
    int                     x,
    int                     z)
{
    /// @note 補正済み高さで中心差分を取り、頂点位置だけでなく陰影も隣接 Terrain と連続させる。
    /// @note `TerrainComponent::ComputeNormal()` は単体 Terrain 用に端を clamp するため、
    /// @note TerrainGrid では境界法線が隣の傾斜を見ず、継ぎ目が暗線として残ってしまう。
    const float dhdx = (SampleStitchedTerrainHeight(terrain, neighbors, x + 1, z)
                      - SampleStitchedTerrainHeight(terrain, neighbors, x - 1, z))
                     / (2.0f * terrain.cellSize);
    const float dhdz = (SampleStitchedTerrainHeight(terrain, neighbors, x, z + 1)
                      - SampleStitchedTerrainHeight(terrain, neighbors, x, z - 1))
                     / (2.0f * terrain.cellSize);
    return math::Vector3{ -dhdx, 1.0f, -dhdz }.Normalized();
}

static TerrainNeighbors ResolveTerrainNeighbors(
    Scene&                       scene,
    const TerrainGridComponent*  terrainGrid,
    EntityID                     eid)
{
    TerrainNeighbors neighbors;
    if (!terrainGrid)
        return neighbors;

    int gx = 0;
    int gz = 0;
    if (!terrainGrid->TryGetGridPos(eid, gx, gz))
        return neighbors;

    auto resolveNeighbor = [&](int ngx, int ngz) -> const TerrainComponent* {
        const EntityID neid = terrainGrid->GetCell(ngx, ngz);
        if (!scene.IsValid(neid))
            return nullptr;
        return scene.GetComponent<TerrainComponent>(neid);
    };

    neighbors.north     = resolveNeighbor(gx,     gz - 1);
    neighbors.south     = resolveNeighbor(gx,     gz + 1);
    neighbors.west      = resolveNeighbor(gx - 1, gz);
    neighbors.east      = resolveNeighbor(gx + 1, gz);
    neighbors.northWest = resolveNeighbor(gx - 1, gz - 1);
    neighbors.northEast = resolveNeighbor(gx + 1, gz - 1);
    neighbors.southWest = resolveNeighbor(gx - 1, gz + 1);
    neighbors.southEast = resolveNeighbor(gx + 1, gz + 1);
    return neighbors;
}

/// @note チャンクが保持する GPU 頂点・インデックスバッファを明示解放する。ResourceHandle は RAII
/// @note ではなく明示解放が必要 (このファイルの texCache 解放や末尾の GC 経路と同じ規約)。キャッシュ
/// @note からチャンクを破棄する前に必ず呼ばないと、スカルプト中は heightDirty が毎フレーム立って
/// @note チャンクを作り直すため、編集するほど GPU バッファがリークし続けてフレームが重くなる。
static void ReleaseChunkBuffers(renderer::ResourceManager& resources, TerrainChunk& chunk)
{
    resources.Release(chunk.vertexBuffer);
    for (auto& ib : chunk.indexBufferLOD)
        resources.Release(ib);
}

static void EraseTerrainChunkCache(renderer::ResourceManager& resources, EntityID eid)
{
    std::erase_if(g_chunkCache, [&](auto& kv) {
        if (kv.first.entityId != eid)
            return false;
        ReleaseChunkBuffers(resources, kv.second);
        return true;
    });
}

static void EraseTerrainChunkCacheForComponent(Scene& scene, renderer::ResourceManager& resources, const TerrainComponent* terrain)
{
    if (!terrain)
        return;

    for (EntityID candidate : scene.GetEntities<TerrainComponent>()) {
        if (scene.GetComponent<TerrainComponent>(candidate) == terrain) {
            EraseTerrainChunkCache(resources, candidate);
            return;
        }
    }
}

static void EraseNeighborTerrainChunkCaches(Scene& scene, renderer::ResourceManager& resources, const TerrainNeighbors& neighbors)
{
    /// @note 境界頂点は隣接 Terrain の高さを参照して構築されるため、自身の高さが変わったときは
    /// @note 隣接側の GPU キャッシュも無効化する。heightDirty を相互に立てると隣接同士で毎フレーム
    /// @note 再 dirty 化されるため、ここではキャッシュだけを直接破棄する (GPU バッファは解放する)。
    EraseTerrainChunkCacheForComponent(scene, resources, neighbors.north);
    EraseTerrainChunkCacheForComponent(scene, resources, neighbors.south);
    EraseTerrainChunkCacheForComponent(scene, resources, neighbors.west);
    EraseTerrainChunkCacheForComponent(scene, resources, neighbors.east);
    EraseTerrainChunkCacheForComponent(scene, resources, neighbors.northWest);
    EraseTerrainChunkCacheForComponent(scene, resources, neighbors.northEast);
    EraseTerrainChunkCacheForComponent(scene, resources, neighbors.southWest);
    EraseTerrainChunkCacheForComponent(scene, resources, neighbors.southEast);
}

static void BuildChunk(
    const TerrainComponent&      terrain,
    int                          cx,
    int                          cz,
    const TerrainNeighbors&      neighbors,
    std::vector<TerrainVertex>&  outVerts,
    math::Vector3&               outAabbMin,
    math::Vector3&               outAabbMax,
    int&                         outX0,
    int&                         outZ0,
    int&                         outX1,
    int&                         outZ1)
{
    outX0 = cx * terrain.chunkSize;
    outZ0 = cz * terrain.chunkSize;
    outX1 = std::min(outX0 + terrain.chunkSize, terrain.columns - 1);
    outZ1 = std::min(outZ0 + terrain.chunkSize, terrain.rows    - 1);

    outAabbMin = {  1e30f,  1e30f,  1e30f };
    outAabbMax = { -1e30f, -1e30f, -1e30f };

    for (int z = outZ0; z <= outZ1; ++z) {
        for (int x = outX0; x <= outX1; ++x) {
            const float h = SampleStitchedTerrainHeight(terrain, neighbors, x, z);

            TerrainVertex v;
            v.position = {
                static_cast<float>(x) * terrain.cellSize,
                h,
                static_cast<float>(z) * terrain.cellSize
            };
            v.normal = ComputeStitchedNormal(terrain, neighbors, x, z);

            {
                const float hL = SampleStitchedTerrainHeight(terrain, neighbors, x - 1, z);
                const float hR = SampleStitchedTerrainHeight(terrain, neighbors, x + 1, z);
                const float span = 2.0f * terrain.cellSize;
                const float dhDx = (hR - hL) / span;
                v.tangent = math::Vector3{ terrain.cellSize, dhDx * terrain.cellSize, 0.0f }.Normalized();
            }

            v.uv = {
                static_cast<float>(x) / static_cast<float>(terrain.columns - 1),
                static_cast<float>(z) / static_cast<float>(terrain.rows    - 1)
            };
            outVerts.push_back(v);

            outAabbMin.x = std::min(outAabbMin.x, v.position.x);
            outAabbMin.y = std::min(outAabbMin.y, v.position.y);
            outAabbMin.z = std::min(outAabbMin.z, v.position.z);
            outAabbMax.x = std::max(outAabbMax.x, v.position.x);
            outAabbMax.y = std::max(outAabbMax.y, v.position.y);
            outAabbMax.z = std::max(outAabbMax.z, v.position.z);
        }
    }
}

/// @brief チャンクの LOD インデックスを作る。穴のセルは三角形を作らない。
/// @note 粗い LOD のブロックに穴が 1 つでもあれば、そのブロックは LOD0 の三角形で埋める。
/// @note 粗い三角形で穴を丸ごと塞ぐか広げるかの二択にすると、遠景で穴の形が変わって見える。
static void BuildChunkLODIndices(
    const TerrainComponent& terrain,
    int                    x0,
    int                    z0,
    int                    x1,
    int                    z1,
    int                    step,
    std::vector<uint32_t>& outIndices)
{
    const int w = x1 - x0 + 1;
    const bool hasHoles = terrain.holeData.size() == terrain.CellCount();
    auto pushQuad = [&](int x, int z, int xNext, int zNext) {
        const uint32_t i00 = static_cast<uint32_t>(z     * w + x);
        const uint32_t i10 = static_cast<uint32_t>(z     * w + xNext);
        const uint32_t i01 = static_cast<uint32_t>(zNext * w + x);
        const uint32_t i11 = static_cast<uint32_t>(zNext * w + xNext);
        outIndices.push_back(i00); outIndices.push_back(i01); outIndices.push_back(i10);
        outIndices.push_back(i10); outIndices.push_back(i01); outIndices.push_back(i11);
    };
    for (int z = 0; z < (z1 - z0); z += step) {
        for (int x = 0; x < (x1 - x0); x += step) {
            const int zNext = std::min(z + step, z1 - z0);
            const int xNext = std::min(x + step, x1 - x0);
            bool blockHasHole = false;
            if (hasHoles) {
                for (int cz = z; cz < zNext && !blockHasHole; ++cz)
                    for (int cx = x; cx < xNext && !blockHasHole; ++cx)
                        blockHasHole = terrain.IsHoleCell(x0 + cx, z0 + cz);
            }
            if (!blockHasHole) {
                pushQuad(x, z, xNext, zNext);
                continue;
            }
            for (int cz = z; cz < zNext; ++cz)
                for (int cx = x; cx < xNext; ++cx)
                    if (!terrain.IsHoleCell(x0 + cx, z0 + cz))
                        pushQuad(cx, cz, cx + 1, cz + 1);
        }
    }
}

static TerrainChunk& EnsureTerrainChunk(
    const TerrainComponent& terrain,
    EntityID eid,
    int cx,
    int cz,
    const TerrainNeighbors& neighbors,
    renderer::ResourceManager& resources)
{
    const TerrainChunkKey key{ eid, static_cast<uint32_t>(cx), static_cast<uint32_t>(cz) };
    if (!g_chunkCache.contains(key)) {
        std::vector<TerrainVertex> verts;
        math::Vector3 aabbMin, aabbMax;
        int x0, z0, x1, z1;
        BuildChunk(terrain, cx, cz, neighbors, verts, aabbMin, aabbMax, x0, z0, x1, z1);

        TerrainChunk chunk;
        chunk.aabbMin = aabbMin;
        chunk.aabbMax = aabbMax;
        chunk.vertexBuffer = resources.CreateVertexBuffer(
            verts.data(),
            verts.size() * sizeof(TerrainVertex),
            static_cast<uint32_t>(sizeof(TerrainVertex)));

        for (int lod = 0; lod < kLODCount; ++lod) {
            std::vector<uint32_t> indices;
            BuildChunkLODIndices(terrain, x0, z0, x1, z1, kLODSteps[lod], indices);
            chunk.indexCountLOD[lod] = static_cast<uint32_t>(indices.size());
            /// @note 全セルが穴のチャンクはインデックス 0 本。空バッファは作らず、描画側で飛ばす。
            if (chunk.indexCountLOD[lod] > 0)
                chunk.indexBufferLOD[lod] = resources.CreateIndexBuffer(indices.data(), chunk.indexCountLOD[lod]);
        }
        g_chunkCache[key] = std::move(chunk);
    }
    return g_chunkCache.at(key);
}

static float TGetF(const asset::MaterialAsset* m, const std::string& name, float def)
{
    if (!m) return def;
    auto it = m->params.find(name);
    if (it != m->params.end() && !it->second.empty()) return it->second[0];
    return def;
}
static std::string TGetTex(const asset::MaterialAsset* m, const std::string& name)
{
    if (!m) return {};
    auto it = m->textures.find(name);
    if (it != m->textures.end()) return it->second;
    return {};
}

TerrainLayerGpu ReadTerrainLayerParams(const asset::MaterialAsset* material, std::string_view keyPrefix)
{
    const std::string prefix(keyPrefix);
    const auto get = [&](const char* key, float def) { return TGetF(material, prefix + key, def); };
    TerrainLayerGpu layer;
    layer.tilingX            = get("tilingX", 8.0f);
    layer.tilingZ            = get("tilingZ", 8.0f);
    layer.normalStrength     = get("normalStrength", 1.0f);
    layer.roughness          = get("roughness", 0.8f);
    layer.ambientOcclusion   = get("ambientOcclusion", 1.0f);
    layer.hasAoRoughness     = TGetTex(material, prefix + "ao_roughness").empty() ? 0.0f : 1.0f;
    layer.hasHeight          = TGetTex(material, prefix + "height").empty() ? 0.0f : 1.0f;
    layer.heightBlend        = get("heightBlend", 0.0f);
    layer.autoMinHeight      = get("autoMinHeight", -10000.0f);
    layer.autoMaxHeight      = get("autoMaxHeight", 10000.0f);
    layer.autoHeightFade     = get("autoHeightFade", 1.0f);
    layer.autoBlendEnabled   = get("autoBlendEnabled", 0.0f);
    layer.autoMinSlope       = get("autoMinSlope", 0.0f);
    layer.autoMaxSlope       = get("autoMaxSlope", 1.0f);
    layer.autoSlopeFade      = get("autoSlopeFade", 0.1f);
    layer.autoBlendStrength  = get("autoBlendStrength", 1.0f);
    layer.triplanar          = get("triplanar", 0.0f);
    layer.triplanarSharpness = get("triplanarSharpness", 4.0f);
    layer.macroScale         = get("macroScale", 0.1f);
    layer.macroStrength      = get("macroStrength", 0.0f);
    return layer;
}

bool TerrainFallbackTextures::IsValid() const
{
    return white.IsValid() && flatNormal.IsValid() && black.IsValid() && gray.IsValid();
}

TerrainFallbackTextures CreateTerrainFallbackTextures(renderer::ResourceManager& resources)
{
    TerrainFallbackTextures t;
    const uint8_t white[4]  = { 255, 255, 255, 255 };
    const uint8_t normal[4] = { 128, 128, 255, 255 };
    const uint8_t black[4]  = {   0,   0,   0, 255 };
    const uint8_t gray[4]   = { 128, 128, 128, 255 };
    t.white      = resources.CreateTexture(white, 1, 1);
    t.flatNormal = resources.CreateTexture(normal, 1, 1);
    t.black      = resources.CreateTexture(black, 1, 1);
    t.gray       = resources.CreateTexture(gray, 1, 1);
    return t;
}

void ReleaseTerrainFallbackTextures(renderer::ResourceManager& resources, TerrainFallbackTextures& textures)
{
    resources.Release(textures.white);
    resources.Release(textures.flatNormal);
    resources.Release(textures.black);
    resources.Release(textures.gray);
    textures = {};
}

/// @return テクスチャの bindless 添字。未ロード・発行失敗なら代替の添字。
static uint32_t BindlessIndexOr(renderer::ResourceManager& resources,
                                renderer::ResourceHandle<renderer::TextureTag> texture,
                                renderer::ResourceHandle<renderer::TextureTag> fallback)
{
    if (const renderer::ITexture* t = resources.Get(texture)) {
        const uint32_t index = t->GetBindlessIndex();
        if (index != renderer::INVALID_BINDLESS_INDEX) return index;
    }
    if (const renderer::ITexture* f = resources.Get(fallback))
        return f->GetBindlessIndex();
    return renderer::INVALID_BINDLESS_INDEX;
}

void FillTerrainLayerTextureIndices(renderer::ResourceManager& resources,
                                    const asset::MaterialAsset* material,
                                    std::string_view keyPrefix,
                                    const TerrainFallbackTextures& fallback,
                                    TerrainLayerGpu& layer)
{
    const std::string prefix(keyPrefix);
    const auto load = [&](const char* key) {
        const std::string path = TGetTex(material, prefix + key);
        return path.empty() ? renderer::ResourceHandle<renderer::TextureTag>{} : resources.LoadTexture(path);
    };
    layer.diffuseIndex     = BindlessIndexOr(resources, load("diffuse"), fallback.white);
    layer.normalIndex      = BindlessIndexOr(resources, load("normal"), fallback.flatNormal);
    layer.aoRoughnessIndex = BindlessIndexOr(resources, load("ao_roughness"), fallback.black);
    layer.heightIndex      = BindlessIndexOr(resources, load("height"), fallback.gray);
}

/// @brief 番号 / 重みマップを作り直す。どちらも頂点 1 つ = RGBA8 1 画素なので配列をそのまま渡せる。
static void RebuildSplatTextures(TerrainGpuState& state, const TerrainComponent& terrain,
                                 renderer::ResourceManager& resources)
{
    resources.Release(state.splatIndices);
    resources.Release(state.splatWeights);
    state.splatIndices = {};
    state.splatWeights = {};
    if (terrain.HasValidSplat()) {
        state.splatIndices = resources.CreateTexture(terrain.splatIndices.data(),
            static_cast<uint32_t>(terrain.columns), static_cast<uint32_t>(terrain.rows));
        state.splatWeights = resources.CreateTexture(terrain.splatWeights.data(),
            static_cast<uint32_t>(terrain.columns), static_cast<uint32_t>(terrain.rows));
        return;
    }
    /// @note スプラット未初期化の地形は «全頂点が層 0» の 1x1 で描く。
    const uint8_t indices[4] = { 0, 0, 0, 0 };
    const uint8_t weights[4] = { 255, 0, 0, 0 };
    state.splatIndices = resources.CreateTexture(indices, 1, 1);
    state.splatWeights = resources.CreateTexture(weights, 1, 1);
}

static void ReleaseTerrainGpuState(TerrainGpuState& state, renderer::ResourceManager& resources)
{
    resources.Release(state.splatIndices);
    resources.Release(state.splatWeights);
    resources.Release(state.layerBuffer);
    state = {};
}

/// @brief 層配列を StructuredBuffer へ置く。層数が変わったら作り直し、内容が同じなら転送しない。
static void UploadLayerBuffer(TerrainGpuState& state, const std::vector<TerrainLayerGpu>& layers,
                              renderer::ResourceManager& resources)
{
    const uint32_t count = static_cast<uint32_t>(layers.size());
    const size_t bytes = layers.size() * sizeof(TerrainLayerGpu);
    if (!state.layerBuffer.IsValid() || state.layerCapacity != count) {
        resources.Release(state.layerBuffer);
        state.layerBuffer = resources.CreateStructuredBuffer(layers.data(), count,
                                                             static_cast<uint32_t>(sizeof(TerrainLayerGpu)));
        state.layerCapacity = count;
        state.uploadedLayers = layers;
        return;
    }
    if (state.uploadedLayers.size() == layers.size()
        && std::memcmp(state.uploadedLayers.data(), layers.data(), bytes) == 0)
        return;
    resources.Update(state.layerBuffer, layers.data(), bytes);
    state.uploadedLayers = layers;
}

void ExtractRenderTerrains(RenderPassContext& ctx, renderer::RenderScene& output) {
    auto& scene = ctx.scene;
    auto& resources = ctx.resources;
    static renderer::ResourceManager* owner = nullptr;
    static uint64_t version = UINT64_MAX;
    static TerrainFallbackTextures s_fallback;
    if (owner != &resources || version != resources.GetResetVersion()) {
        owner = &resources; version = resources.GetResetVersion();
        g_chunkCache.clear(); g_gpuCache.clear();
        s_fallback = CreateTerrainFallbackTextures(resources);
    }
    {
        std::unordered_set<uint32_t> validIndices;
        for (EntityID eid : scene.GetEntities<TerrainComponent>())
            validIndices.insert(eid.index);

        std::erase_if(g_chunkCache, [&validIndices, &resources](auto& kv) {
            if (validIndices.count(kv.first.entityId.index)) return false;
            resources.Release(kv.second.vertexBuffer);
            for (auto& ib : kv.second.indexBufferLOD)
                resources.Release(ib);
            return true;
        });
        std::erase_if(g_gpuCache, [&validIndices, &resources](auto& kv) {
            if (validIndices.count(kv.first)) return false;
            ReleaseTerrainGpuState(kv.second, resources);
            return true;
        });
    }
    TerrainGridComponent* terrainGrid = nullptr;
    /// @note 親ごと無効化された Grid は使わない (有効な最初の 1 つ)。
    for (const EntityID gridId : scene.GetEntities<TerrainGridComponent>()) {
        const GameObject* gridObject = scene.GetGameObject(gridId);
        if (!gridObject || !gridObject->activeInHierarchy()) continue;
        terrainGrid = scene.GetComponent<TerrainGridComponent>(gridId);
        break;
    }

    /// @note 天候はシーンに 1 つ。TerrainGrid では地形が何十個も回るのでループの外で引く。
    const ActiveWeather weather = FindActiveWeather(scene);

    for (auto [terrain, transform] : scene.View<TerrainComponent, Transform>()) {
        if (!terrain.enabled || terrain.heightData.empty()) continue;

        assert(terrain.heightData.size() == static_cast<size_t>(terrain.columns)
                                          * static_cast<size_t>(terrain.rows));
        assert(terrain.chunkSize > 0);

        EntityID eid{};
        for (EntityID candidate : scene.GetEntities<TerrainComponent>()) {
            if (scene.GetComponent<TerrainComponent>(candidate) == &terrain) {
                eid = candidate;
                break;
            }
        }
        if (!scene.IsValid(eid)) continue;
        {
            const auto* go = scene.GetGameObject(eid);
            if (!go || !go->activeInHierarchy()) continue;
        }

        const TerrainNeighbors neighbors = ResolveTerrainNeighbors(scene, terrainGrid, eid);

        if (terrain.heightDirty) {
            EraseTerrainChunkCache(resources, eid);
            EraseNeighborTerrainChunkCaches(scene, resources, neighbors);
            terrain.heightDirty = false;
        }

        TerrainGpuState& gpu = g_gpuCache[eid.index];
        if (terrain.splatDirty || !gpu.splatIndices.IsValid() || !gpu.splatWeights.IsValid()) {
            RebuildSplatTextures(gpu, terrain, resources);
            terrain.splatDirty = false;
        }

        /// @note 層パラメーターは毎フレーム .mat から読み直す。Material インスペクタで .mat を直接編集しても
        /// @note 地形側の dirty は立たないため。抽出は map 参照だけで安く、内容が同じなら転送もしない。
        /// @note bindless 添字も毎フレーム引く。差し替え・ホットリロードで変わるため (Material::Upload と同じ方針)。
        std::vector<TerrainLayerGpu> layers;
        layers.reserve(static_cast<size_t>((std::max)(terrain.LayerCount(), 1)));
        bool anyAutoBlend = false;
        for (const std::string& path : terrain.layerMaterials) {
            const asset::MaterialAsset* material = nullptr;
            if (!path.empty())
                material = asset::AssetManager::Get<asset::MaterialAsset>(
                    asset::AssetManager::Load<asset::MaterialAsset>(path));
            TerrainLayerGpu layer = ReadTerrainLayerParams(material);
            FillTerrainLayerTextureIndices(resources, material, {}, s_fallback, layer);
            anyAutoBlend |= layer.autoBlendEnabled > 0.0f && layer.autoBlendStrength > 0.0f;
            layers.push_back(layer);
        }
        if (layers.empty()) {
            /// @note 層 0 枚の地形は白い既定層 1 枚で描く。シェーダーは layerCount >= 1 を前提にする。
            TerrainLayerGpu layer;
            FillTerrainLayerTextureIndices(resources, nullptr, {}, s_fallback, layer);
            layers.push_back(layer);
        }
        UploadLayerBuffer(gpu, layers, resources);
        terrain.materialParamDirty = false;

        const renderer::IStructuredBuffer* layerBuffer = resources.Get(gpu.layerBuffer);
        const uint32_t layerBufferIndex = layerBuffer ? layerBuffer->GetBindlessIndex() : renderer::INVALID_BINDLESS_INDEX;
        /// @note 添字が無いと ResourceDescriptorHeap の範囲外を読む。描かない方が原因を切り分けやすい。
        if (layerBufferIndex == renderer::INVALID_BINDLESS_INDEX) continue;

        const int chunkCountX = (terrain.columns - 1 + terrain.chunkSize - 1) / terrain.chunkSize;
        const int chunkCountZ = (terrain.rows    - 1 + terrain.chunkSize - 1) / terrain.chunkSize;

        const math::Matrix4 world = transform.GetWorldMatrix();
        TerrainObjectCB terrainCBData{};
        terrainCBData.worldMatrix = world;
        terrainCBData.wvpMatrix   = world;
        terrainCBData.weather = { weather.wetness, weather.darkening, weather.puddleAmount, 0.0f };
        terrainCBData.terrainParams = {
            static_cast<float>(terrain.columns - 1) * terrain.cellSize,
            static_cast<float>(terrain.rows - 1) * terrain.cellSize,
            terrain.heightBlendDepth,
            anyAutoBlend ? 1.0f : 0.0f };
        terrainCBData.layerBufferIndex = layerBufferIndex;
        terrainCBData.layerCount       = static_cast<uint32_t>(layers.size());
        terrainCBData.splatColumns     = terrain.HasValidSplat() ? static_cast<uint32_t>(terrain.columns) : 1u;
        terrainCBData.splatRows        = terrain.HasValidSplat() ? static_cast<uint32_t>(terrain.rows) : 1u;

        renderer::RenderTerrainInput input;
        input.layer = static_cast<uint32_t>(scene.GetGameObject(eid)->layer);
        input.constants = terrainCBData;
        input.splatIndices = gpu.splatIndices;
        input.splatWeights = gpu.splatWeights;
        input.chunkWorldSize = static_cast<float>(terrain.chunkSize) * terrain.cellSize;
        const auto* fiber = scene.GetComponent<FiberComponent>(eid);
        input.fiberSurface = fiber && fiber->m_enabled && !fiber->m_materialPath.empty();
        for (const auto& selected : ctx.settings.selectedObjects)
            if (selected.index == eid.index && selected.generation == eid.generation) { input.selected = true; break; }
        for (int cz = 0; cz < chunkCountZ; ++cz)
            for (int cx = 0; cx < chunkCountX; ++cx)
                input.patches.push_back(EnsureTerrainChunk(terrain, eid, cx, cz, neighbors, resources));
        output.terrains.push_back(std::move(input));
    }
}
}
