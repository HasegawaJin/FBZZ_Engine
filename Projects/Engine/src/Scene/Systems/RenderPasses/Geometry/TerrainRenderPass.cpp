// FBZZ Engine
// RenderPasses/Geometry/TerrainRenderPass.cpp | fbzz::scene
// TerrainComponent → GPU チャンクメッシュ生成・描画 (IRenderPass 実装)
//
// テクスチャスロット (Terrain.hlsl と同期すること):
//   t0 = スプラットマップ  RGBA8 (R=layer0, G=layer1, B=layer2, A=layer3)
//   t1-t4   = layer0-3 ディフューズ
//   t5-t8   = layer0-3 法線
//   t9-t12  = layer0-3 AO/Roughness (R=AO, G=Roughness)
//   t13     = shadow depth
//
// サンプラースロット (Terrain.hlsl と同期すること):
//   s0 = WRAP_ANISOTROPIC  ディフューズテクスチャ用
//   s1 = BORDER_ZERO       shadow PCF 用比較サンプラー
//   s2 = CLAMP_LINEAR      スプラットマップ用
//
// 設計上の注意:
//   - シングルスレッド前提。static ローカルによる遅延初期化を使う。
//   - GPU バッファは ResourceHandle で所有し、static map でエンティティごとにキャッシュする。
//   - heightDirty: 全チャンクを削除して再構築
//   - splatDirty : スプラットマップ + レイヤーテクスチャを再ロード
#include "Engine/Scene/Systems/RenderPasses/Geometry/TerrainRenderPass.hpp"
#include "GeometryPasses.hpp"
#include "Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include "Engine/Scene/Components/TerrainGridComponent.hpp"
#include "Engine/Scene/Entity.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/Camera.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/RenderSettings.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Engine/Renderer/SamplerMode.hpp"
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
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::scene {

// GPU 頂点レイアウト。HLSL の TerrainVSInput と完全に一致させること。
//   POSITION  : float3  offset  0  (12 bytes)
//   NORMAL    : float3  offset 12  (12 bytes)
//   TANGENT   : float3  offset 24  (12 bytes)
//   TEXCOORD0 : float2  offset 36  ( 8 bytes)
//   stride = 44 bytes
struct TerrainVertex {
    math::Vector3 position;
    math::Vector3 normal;
    math::Vector3 tangent;
    math::Vector2 uv;
};

static constexpr int kLODCount            = 3;
static constexpr int kLODSteps[kLODCount] = { 1, 2, 4 };

struct TerrainChunk {
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer;
    std::array<renderer::ResourceHandle<renderer::BufferTag>, kLODCount> indexBufferLOD;
    std::array<uint32_t, kLODCount> indexCountLOD = {};
    math::Vector3 aabbMin;
    math::Vector3 aabbMax;
};

struct TerrainChunkKey {
    EntityID entityId;
    uint32_t chunkX = 0;
    uint32_t chunkZ = 0;
    bool operator==(const TerrainChunkKey& o) const = default;
};

} // namespace fbzz::scene

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

struct TerrainTextures {
    renderer::ResourceHandle<renderer::TextureTag> splatmap;
    std::array<renderer::ResourceHandle<renderer::TextureTag>, 4> diffuse;
    std::array<renderer::ResourceHandle<renderer::TextureTag>, 4> normal;
    std::array<renderer::ResourceHandle<renderer::TextureTag>, 4> aoRoughness;
};

static std::unordered_map<TerrainChunkKey, TerrainChunk> g_chunkCache;
static std::unordered_map<uint32_t, TerrainTextures>    g_texCache;
// レイヤーマテリアルのテクスチャパス署名。Material インスペクタでテクスチャを差し替えたときだけ
// テクスチャを再構築するための変更検出に使う（毎フレーム GPU 再アップロードを避ける）。
static std::unordered_map<uint32_t, size_t>             g_texSigCache;

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

// CameraConstants (b0)
struct TerrainCameraFrameCB {
    math::Matrix4 view;
    math::Matrix4 projection;
    math::Matrix4 viewProjection;
    math::Matrix4 invViewProjection;
    math::Vector3 cameraPos; float nearZ;
    float         farZ;      float _pad[3];
};
static_assert(sizeof(TerrainCameraFrameCB) == 288, "PerFrameCB size mismatch");

// TerrainCB (b1)
struct TerrainObjectCB {
    math::Matrix4 worldMatrix;
    math::Matrix4 wvpMatrix;
    math::Vector4 layerTiling[4];
    math::Vector4 layerNormalStrength;
    math::Vector4 layerMaterial[4];
    math::Vector4 layerTextureFlags;
    math::Vector4 layerAutoHeight[4];
    math::Vector4 layerAutoSlope[4];
};
static_assert(sizeof(TerrainObjectCB) == 416, "TerrainObjectCB size mismatch");

static std::unordered_map<uint32_t, TerrainObjectCB> g_cbParamCache;

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
    // WHY: TerrainGrid の境界では複数 Terrain が同じワールド頂点を持つ。
    //      描画用メッシュではその共有頂点を平均して、保存データを破壊せずに隙間を隠す。
    //      角は最大 4 Terrain が交差するため、斜め隣接も平均に含める。
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
    // WHAT: 補正済み高さで中心差分を取る。頂点位置だけでなく陰影も隣接 Terrain と連続させる。
    // WHY: TerrainComponent::ComputeNormal() は単体 Terrain 用に端を clamp するため、
    //      TerrainGrid では境界法線が隣の傾斜を見ず、継ぎ目が暗線として残る。
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

// チャンクが保持する GPU 頂点・インデックスバッファを明示解放する。
// WHY: ResourceHandle は RAII ではなく明示解放が必要（このファイルの texCache 解放や
//      末尾の GC 経路と同じ規約）。キャッシュからチャンクを破棄する前に必ず呼ばないと、
//      スカルプト中は heightDirty が毎フレーム立ってチャンクを作り直すため、
//      編集するほど GPU バッファがリークし続けてフレームが重くなる。
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
    // WHY: 境界頂点は隣接 Terrain の高さを参照して構築されるため、
    //      自身の高さが変わったときは隣接側の GPU キャッシュも無効化する。
    //      heightDirty を相互に立てると隣接同士で毎フレーム再 dirty 化されるため、
    //      ここではキャッシュだけを直接破棄する（GPU バッファは解放する）。
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

static void BuildChunkLODIndices(
    int                    x0,
    int                    z0,
    int                    x1,
    int                    z1,
    int                    step,
    std::vector<uint32_t>& outIndices)
{
    const int w = x1 - x0 + 1;
    for (int z = 0; z < (z1 - z0); z += step) {
        for (int x = 0; x < (x1 - x0); x += step) {
            const int zNext = std::min(z + step, z1 - z0);
            const int xNext = std::min(x + step, x1 - x0);
            const uint32_t i00 = static_cast<uint32_t>(z     * w + x);
            const uint32_t i10 = static_cast<uint32_t>(z     * w + xNext);
            const uint32_t i01 = static_cast<uint32_t>(zNext * w + x);
            const uint32_t i11 = static_cast<uint32_t>(zNext * w + xNext);
            outIndices.push_back(i00); outIndices.push_back(i01); outIndices.push_back(i10);
            outIndices.push_back(i10); outIndices.push_back(i01); outIndices.push_back(i11);
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
            BuildChunkLODIndices(x0, z0, x1, z1, kLODSteps[lod], indices);
            chunk.indexCountLOD[lod] = static_cast<uint32_t>(indices.size());
            chunk.indexBufferLOD[lod] = resources.CreateIndexBuffer(indices.data(), chunk.indexCountLOD[lod]);
        }
        g_chunkCache[key] = std::move(chunk);
    }
    return g_chunkCache.at(key);
}

static bool IsChunkVisible(
    const math::Frustum&  frustum,
    const math::Matrix4&  world,
    const math::Vector3&  localMin,
    const math::Vector3&  localMax)
{
    math::Vector3 worldMin = {  1e30f,  1e30f,  1e30f };
    math::Vector3 worldMax = { -1e30f, -1e30f, -1e30f };
    const math::Vector3 corners[8] = {
        { localMin.x, localMin.y, localMin.z }, { localMax.x, localMin.y, localMin.z },
        { localMin.x, localMax.y, localMin.z }, { localMax.x, localMax.y, localMin.z },
        { localMin.x, localMin.y, localMax.z }, { localMax.x, localMin.y, localMax.z },
        { localMin.x, localMax.y, localMax.z }, { localMax.x, localMax.y, localMax.z },
    };
    for (const auto& c : corners) {
        const math::Vector4 wp = world * math::Vector4{ c.x, c.y, c.z, 1.0f };
        worldMin.x = std::min(worldMin.x, wp.x); worldMax.x = std::max(worldMax.x, wp.x);
        worldMin.y = std::min(worldMin.y, wp.y); worldMax.y = std::max(worldMax.y, wp.y);
        worldMin.z = std::min(worldMin.z, wp.z); worldMax.z = std::max(worldMax.z, wp.z);
    }
    const math::Vector3 center  = { (worldMin.x + worldMax.x) * 0.5f,
                                     (worldMin.y + worldMax.y) * 0.5f,
                                     (worldMin.z + worldMax.z) * 0.5f };
    const math::Vector3 extents = { (worldMax.x - worldMin.x) * 0.5f,
                                     (worldMax.y - worldMin.y) * 0.5f,
                                     (worldMax.z - worldMin.z) * 0.5f };
    return frustum.IntersectsAABB(center, extents);
}

static renderer::ResourceHandle<renderer::TextureTag> BuildSplatmapTexture(
    const TerrainComponent&    terrain,
    renderer::ResourceManager& resources,
    renderer::ResourceHandle<renderer::TextureTag> fallback)
{
    if (terrain.splatData.empty()) return fallback;
    if (terrain.splatData.size() !=
        static_cast<size_t>(terrain.columns) * static_cast<size_t>(terrain.rows) * 4u)
        return fallback;
    return resources.CreateTexture(
        terrain.splatData.data(),
        static_cast<uint32_t>(terrain.columns),
        static_cast<uint32_t>(terrain.rows));
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

static TerrainTextures BuildTextureSet(
    const std::array<const asset::MaterialAsset*, 4>& layerMats,
    const TerrainComponent&    terrain,
    renderer::ResourceManager& resources,
    renderer::ResourceHandle<renderer::TextureTag> splatFallback,
    renderer::ResourceHandle<renderer::TextureTag> whiteTex,
    renderer::ResourceHandle<renderer::TextureTag> flatNormalTex,
    renderer::ResourceHandle<renderer::TextureTag> blackTex)
{
    TerrainTextures ts;
    ts.splatmap = BuildSplatmapTexture(terrain, resources, splatFallback);

    for (int i = 0; i < 4; ++i) {
        const std::string diffusePath     = TGetTex(layerMats[i], "diffuse");
        const std::string normalPath      = TGetTex(layerMats[i], "normal");
        const std::string aoRoughnessPath = TGetTex(layerMats[i], "ao_roughness");
        ts.diffuse[i]     = diffusePath.empty()     ? whiteTex      : resources.LoadTexture(diffusePath);
        ts.normal[i]      = normalPath.empty()       ? flatNormalTex : resources.LoadTexture(normalPath);
        ts.aoRoughness[i] = aoRoughnessPath.empty()  ? blackTex      : resources.LoadTexture(aoRoughnessPath);
    }
    return ts;
}

// ─── IRenderPass ──────────────────────────────────────────────────────────

std::string_view TerrainRenderPass::Name() const { return "TerrainForward"; }

std::vector<renderer::RenderGraph::ResourceAccess> TerrainRenderPass::DeclareAccesses(
    const RenderPassContext& ctx) const
{
    using U = renderer::RenderGraph::ResourceUsage;
    // Deferred では GBuffer へ書き込み、DeferredLighting/GTAO/SSAO/SSR/ContactShadows に地形を含める。
    // Forward では従来どおり HDR へ直接ライティング結果を描く。
    if (ctx.isDeferred)
        return { { "GBuffer", U::ReadWrite } };
    return { { "ShadowMap", U::Read }, { "HDR", U::ReadWrite } };
}

void TerrainRenderPass::Execute(RenderPassContext& ctx)
{
    // Deferred: GBuffer(MRT) へ書く。Forward: HDR へ直接描く。
    ctx.renderer.SetRenderTarget(
        ctx.isDeferred ? ctx.handles.gbufferRT : ctx.handles.hdrRT, ctx.resources);

    // エイリアス: TerrainRenderSystem の旧シグネチャ変数名を ctx から引く
    Scene&                     scene               = ctx.scene;
    renderer::IRenderer&       renderer            = ctx.renderer;
    renderer::ResourceManager& resources           = ctx.resources;
    const renderer::Camera&    camera              = ctx.camera;
    const renderer::RenderSettings* settings       = &ctx.settings;
    auto shadowDepthTexture = ctx.resources.GetDepthTexture(ctx.handles.shadowMapRT);
    auto shadowCB           = ctx.handles.shadowCB;
    auto lightCB            = ctx.handles.lightCB;

    // WHY: static ローカルは初回のみ初期化される。ResourceManager::Reset() で世代が変わった
    //      場合だけ再生成し、旧ハンドル（失効済み）へのアクセスを防ぐ。
    static uint64_t s_resetVersion = resources.GetResetVersion();
    static auto terrainShader = resources.LoadShader("Assets/Shaders/Terrain/Terrain.hlsl");
    // Deferred 用: GBuffer(MRT) へ albedo/roughness/normal/metallic を書き出す地形シェーダ。
    static auto terrainGBufferShader = resources.LoadShader("Assets/Shaders/Terrain/TerrainGBuffer.hlsl");
    static auto terrainPSO    = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto terrainWireframePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::WIREFRAME,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto cameraCBH  = resources.CreateConstantBuffer(sizeof(TerrainCameraFrameCB));
    static auto terrainCBH = resources.CreateConstantBuffer(sizeof(TerrainObjectCB));

    static auto s_whiteTex = [&] {
        const uint8_t w[4] = { 255, 255, 255, 255 };
        return resources.CreateTexture(w, 1, 1);
    }();
    static auto s_splatFallback = [&] {
        const uint8_t s[4] = { 255, 0, 0, 0 };
        return resources.CreateTexture(s, 1, 1);
    }();
    static auto s_flatNormalTex = [&] {
        const uint8_t n[4] = { 128, 128, 255, 255 };
        return resources.CreateTexture(n, 1, 1);
    }();
    static auto s_blackTex = [&] {
        const uint8_t b[4] = { 0, 0, 0, 255 };
        return resources.CreateTexture(b, 1, 1);
    }();

    if (s_resetVersion != resources.GetResetVersion()) {
        s_resetVersion      = resources.GetResetVersion();
        terrainShader       = resources.LoadShader("Assets/Shaders/Terrain/Terrain.hlsl");
        terrainGBufferShader = resources.LoadShader("Assets/Shaders/Terrain/TerrainGBuffer.hlsl");
        terrainPSO          = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID,     renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        terrainWireframePSO = resources.CreatePipelineState({ renderer::RasterizerMode::WIREFRAME, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        cameraCBH           = resources.CreateConstantBuffer(sizeof(TerrainCameraFrameCB));
        terrainCBH          = resources.CreateConstantBuffer(sizeof(TerrainObjectCB));
        s_whiteTex      = [&] { const uint8_t w[4] = { 255, 255, 255, 255 }; return resources.CreateTexture(w, 1, 1); }();
        s_splatFallback = [&] { const uint8_t s[4] = { 255,   0,   0,   0 }; return resources.CreateTexture(s, 1, 1); }();
        s_flatNormalTex = [&] { const uint8_t n[4] = { 128, 128, 255, 255 }; return resources.CreateTexture(n, 1, 1); }();
        s_blackTex      = [&] { const uint8_t b[4] = {   0,   0,   0, 255 }; return resources.CreateTexture(b, 1, 1); }();
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
        std::erase_if(g_texCache, [&validIndices, &resources](auto& kv) {
            if (validIndices.count(kv.first)) return false;
            if (kv.second.splatmap.IsValid() && kv.second.splatmap != s_splatFallback)
                resources.Release(kv.second.splatmap);
            return true;
        });
        std::erase_if(g_cbParamCache, [&validIndices](auto& kv) {
            return !validIndices.count(kv.first);
        });
        std::erase_if(g_texSigCache, [&validIndices](auto& kv) {
            return !validIndices.count(kv.first);
        });
    }

    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC_4X);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);
    renderer.SetSampler(2, renderer::SamplerMode::CLAMP_LINEAR);

    {
        const math::Matrix4 vp  = camera.GetViewProjection();
        const math::Matrix4 ivp = math::Matrix4::Inverse(vp);
        TerrainCameraFrameCB camData{};
        camData.view              = camera.GetViewMatrix();
        camData.projection        = camera.GetProjectionMatrix();
        camData.viewProjection    = vp;
        camData.invViewProjection = ivp;
        camData.cameraPos         = camera.m_position;
        camData.nearZ             = camera.m_near;
        camData.farZ              = camera.m_far;
        resources.Update(cameraCBH, &camData, sizeof(camData));
    }

    const math::Frustum frustum = math::Frustum::FromViewProjection(camera.GetViewProjection());

    TerrainGridComponent* terrainGrid = nullptr;
    {
        const auto gridEntities = scene.GetEntities<TerrainGridComponent>();
        if (!gridEntities.empty())
            terrainGrid = scene.GetComponent<TerrainGridComponent>(gridEntities.front());
    }

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

        std::array<const asset::MaterialAsset*, 4> layerMats = {};
        for (int li = 0; li < 4; ++li) {
            if (!terrain.layerMaterials[li].empty()) {
                const auto handle = asset::AssetManager::LoadMaterial(terrain.layerMaterials[li]);
                layerMats[li] = asset::AssetManager::GetMaterial(handle);
            }
        }

        if (terrain.heightDirty) {
            EraseTerrainChunkCache(resources, eid);
            EraseNeighborTerrainChunkCaches(scene, resources, neighbors);
            terrain.heightDirty = false;
        }

        auto RebuildCBParams = [&]() {
            TerrainObjectCB cb{};
            float normalStr[4]            = { 1.0f, 1.0f, 1.0f, 1.0f };
            float materialTextureFlags[4] = {};
            for (int li = 0; li < 4; ++li) {
                const asset::MaterialAsset* m = layerMats[li];
                normalStr[li]             = TGetF(m, "normalStrength", 1.0f);
                const bool hasAoTex       = m && m->textures.count("ao_roughness") &&
                                            !m->textures.at("ao_roughness").empty();
                materialTextureFlags[li]  = hasAoTex ? 1.0f : 0.0f;
                cb.layerTiling[li]        = { TGetF(m, "tilingX", 8.0f),
                                              TGetF(m, "tilingZ", 8.0f), 0.0f, 0.0f };
                cb.layerMaterial[li]      = { TGetF(m, "roughness",        0.8f),
                                              TGetF(m, "ambientOcclusion", 1.0f), 0.0f, 0.0f };
                cb.layerAutoHeight[li]    = { TGetF(m, "autoMinHeight",    -10000.0f),
                                              TGetF(m, "autoMaxHeight",     10000.0f),
                                              TGetF(m, "autoHeightFade",    1.0f),
                                              TGetF(m, "autoBlendEnabled",  0.0f) };
                cb.layerAutoSlope[li]     = { TGetF(m, "autoMinSlope",      0.0f),
                                              TGetF(m, "autoMaxSlope",      1.0f),
                                              TGetF(m, "autoSlopeFade",     0.1f),
                                              TGetF(m, "autoBlendStrength", 1.0f) };
            }
            cb.layerNormalStrength = { normalStr[0], normalStr[1], normalStr[2], normalStr[3] };
            cb.layerTextureFlags   = { materialTextureFlags[0], materialTextureFlags[1],
                                       materialTextureFlags[2], materialTextureFlags[3] };
            g_cbParamCache[eid.index] = cb;
        };

        // マテリアルパラメータ CB は毎フレーム再構築する。
        // WHY: レイヤーマテリアル(.mat)を Material インスペクタで直接編集しても terrain 側の
        //      materialParamDirty は立たないため、従来は roughness/tiling/normalStrength/autoBlend 等の
        //      変更が既存地形へ反映されなかった。パラメータ抽出は安価（map 参照のみ）なので毎フレーム
        //      読み直し、マテリアル編集を即座に地形へ反映する。Deferred 地形は CB 経由でこれらを使う。
        RebuildCBParams();
        terrain.materialParamDirty = false;

        // レイヤーマテリアルのテクスチャパス署名を計算し、変化したときだけテクスチャを再構築する。
        // WHY: GPU 再アップロードは高価なので毎フレームは避けつつ、Material でテクスチャを差し替えたら反映する。
        size_t texSig = 1469598103934665603ull; // FNV-1a offset basis
        for (int li = 0; li < 4; ++li) {
            const asset::MaterialAsset* m = layerMats[li];
            auto mixPath = [&](const char* key) {
                if (m) {
                    const auto it = m->textures.find(key);
                    if (it != m->textures.end())
                        for (unsigned char c : it->second) { texSig ^= c; texSig *= 1099511628211ull; }
                }
                texSig ^= 0x9Eu; texSig *= 1099511628211ull; // レイヤー/キー境界
            };
            mixPath("diffuse");
            mixPath("normal");
            mixPath("ao_roughness");
        }
        const auto sigIt = g_texSigCache.find(eid.index);
        const bool texChanged = (sigIt == g_texSigCache.end()) || sigIt->second != texSig;
        g_texSigCache[eid.index] = texSig;

        const bool needTexRebuild = terrain.splatDirty || !g_texCache.contains(eid.index) || texChanged;
        if (needTexRebuild) {
            auto& cachedTextures = g_texCache[eid.index];
            // WHY: ペイント更新のたびに生成 splatmap を上書きすると旧 GPU Texture が残るため、
            //      パスキャッシュで共有されない所有テクスチャだけを再構築前に解放する。
            if (cachedTextures.splatmap.IsValid() && cachedTextures.splatmap != s_splatFallback)
                resources.Release(cachedTextures.splatmap);
            cachedTextures = BuildTextureSet(layerMats, terrain, resources,
                                             s_splatFallback, s_whiteTex,
                                             s_flatNormalTex, s_blackTex);
            terrain.splatDirty = false;
        }
        const TerrainTextures& textures = g_texCache.at(eid.index);

        const int chunkCountX = (terrain.columns - 1 + terrain.chunkSize - 1) / terrain.chunkSize;
        const int chunkCountZ = (terrain.rows    - 1 + terrain.chunkSize - 1) / terrain.chunkSize;

        const math::Matrix4 world = transform.GetWorldMatrix();
        TerrainObjectCB terrainCBData = g_cbParamCache.count(eid.index)
                                      ? g_cbParamCache.at(eid.index)
                                      : TerrainObjectCB{};
        terrainCBData.worldMatrix = world;
        terrainCBData.wvpMatrix   = camera.GetViewProjection() * world;

        // WHY: TerrainObjectCB は全チャンクで同一内容のため、チャンクごとに Update するのは無駄。
        resources.Update(terrainCBH, &terrainCBData, sizeof(terrainCBData));

        for (int cz = 0; cz < chunkCountZ; ++cz) {
            for (int cx = 0; cx < chunkCountX; ++cx) {
                const TerrainChunk& chunk = EnsureTerrainChunk(terrain, eid, cx, cz, neighbors, resources);

                // 地形チャンクはそれぞれ独立にカリングされる描画候補なので、
                // メッシュと同じ粒度で統計に数える。
                ++ctx.statsTotalObjects;
                if (!IsChunkVisible(frustum, world, chunk.aabbMin, chunk.aabbMax)) {
                    ++ctx.statsFrustumCulled;
                    continue;
                }

                const math::Vector3 localCenter = {
                    (chunk.aabbMin.x + chunk.aabbMax.x) * 0.5f,
                    (chunk.aabbMin.y + chunk.aabbMax.y) * 0.5f,
                    (chunk.aabbMin.z + chunk.aabbMax.z) * 0.5f
                };
                const math::Vector4 wc = world * math::Vector4{ localCenter.x, localCenter.y, localCenter.z, 1.0f };
                const float dx = wc.x - camera.m_position.x;
                const float dy = wc.y - camera.m_position.y;
                const float dz = wc.z - camera.m_position.z;
                const float distSq = dx * dx + dy * dy + dz * dz;

                const float chunkWorldSize = static_cast<float>(terrain.chunkSize) * terrain.cellSize;
                const float d0 = chunkWorldSize * 2.0f;
                const float d1 = chunkWorldSize * 6.0f;
                const int lod = (distSq < d0 * d0) ? 0
                              : (distSq < d1 * d1) ? 1
                              : 2;

                renderer::DrawCall call;
                call.vertexBuffer  = chunk.vertexBuffer;
                call.indexBuffer   = chunk.indexBufferLOD[lod];
                // Deferred: GBuffer 書き込みシェーダ。Forward: 自前ライティングシェーダ。
                call.shader        = ctx.isDeferred ? terrainGBufferShader : terrainShader;
                call.pipelineState = (settings && settings->IsWireframe()) ? terrainWireframePSO : terrainPSO;
                call.indexCount    = chunk.indexCountLOD[lod];
                call.layer         = renderer::RenderLayer::OPAQUE_LAYER;
                call.topology      = renderer::PrimitiveTopology::TRIANGLE_LIST;

                call.constantBuffers[0] = cameraCBH;
                call.constantBuffers[1] = terrainCBH;
                call.constantBuffers[3] = lightCB;
                call.constantBuffers[4] = shadowCB;
                BindClusterLighting(call, ctx);

                call.textures[0]  = textures.splatmap;
                call.textures[1]  = textures.diffuse[0];
                call.textures[2]  = textures.diffuse[1];
                call.textures[3]  = textures.diffuse[2];
                call.textures[4]  = textures.diffuse[3];
                call.textures[5]  = textures.normal[0];
                call.textures[6]  = textures.normal[1];
                call.textures[7]  = textures.normal[2];
                call.textures[8]  = textures.normal[3];
                call.textures[9]  = textures.aoRoughness[0];
                call.textures[10] = textures.aoRoughness[1];
                call.textures[11] = textures.aoRoughness[2];
                call.textures[12] = textures.aoRoughness[3];
                call.textures[13] = shadowDepthTexture;

                SubmitCounted(ctx, call);
            }
        }
    }
}

// ─── SubmitTerrainShadowCasters ───────────────────────────────────────────

void SubmitTerrainShadowCasters(
    RenderPassContext&                            ctx,
    const math::Frustum&                          lightFrustum,
    renderer::ResourceHandle<renderer::ShaderTag> shadowShader,
    renderer::ResourceHandle<renderer::PipelineStateTag> pipelineState,
    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB,
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB)
{
    // WHY: ライト種別・shadow atlas・cascade の選択は ShadowPass 側に閉じ込め、
    //      こちらは「指定されたライト視錐台へ描けるチャンクを提出する」だけにする。
    if (!shadowShader.IsValid() || !pipelineState.IsValid())
        return;

    Scene&                     scene     = ctx.scene;
    renderer::ResourceManager& resources = ctx.resources;

    TerrainGridComponent* terrainGrid = nullptr;
    {
        const auto gridEntities = scene.GetEntities<TerrainGridComponent>();
        if (!gridEntities.empty())
            terrainGrid = scene.GetComponent<TerrainGridComponent>(gridEntities.front());
    }

    struct ShadowObjectCB {
        math::Matrix4 world;
        math::Matrix4 worldInvTranspose;
    };

    // WHY: View<TerrainComponent, Transform> はエンティティ ID を返さないため、以前は
    //      コンポーネントのアドレス一致で ID を逆引きしていた (地形 1 個につき全地形を走査)。
    //      エンティティ側から引けば逆引きは要らず、O(n²) が消える。
    for (EntityID eid : scene.GetEntities<TerrainComponent>()) {
        auto* terrainPtr = scene.GetComponent<TerrainComponent>(eid);
        auto* gameObject = scene.GetGameObject(eid);
        if (!terrainPtr || !gameObject) continue;

        TerrainComponent& terrain   = *terrainPtr;
        Transform&        transform = gameObject->transform;
        if (!terrain.enabled || terrain.heightData.empty())
            continue;

        assert(terrain.heightData.size() == static_cast<size_t>(terrain.columns)
                                          * static_cast<size_t>(terrain.rows));
        assert(terrain.chunkSize > 0);

        {
            const auto* go = scene.GetGameObject(eid);
            if (!go || !go->activeInHierarchy()) continue;
        }

        if (terrain.heightDirty) {
            const TerrainNeighbors dirtyNeighbors = ResolveTerrainNeighbors(scene, terrainGrid, eid);
            EraseTerrainChunkCache(resources, eid);
            EraseNeighborTerrainChunkCaches(scene, resources, dirtyNeighbors);
            terrain.heightDirty = false;
        }

        const int chunkCountX = (terrain.columns - 1 + terrain.chunkSize - 1) / terrain.chunkSize;
        const int chunkCountZ = (terrain.rows    - 1 + terrain.chunkSize - 1) / terrain.chunkSize;
        const math::Matrix4 world = transform.GetWorldMatrix();

        ShadowObjectCB objData{};
        objData.world             = world;
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(world);
        resources.Update(objectCB, &objData, sizeof(objData));

        const TerrainNeighbors neighbors = ResolveTerrainNeighbors(scene, terrainGrid, eid);
        for (int cz = 0; cz < chunkCountZ; ++cz) {
            for (int cx = 0; cx < chunkCountX; ++cx) {
                const TerrainChunk& chunk = EnsureTerrainChunk(terrain, eid, cx, cz, neighbors, resources);
                if (!IsChunkVisible(lightFrustum, world, chunk.aabbMin, chunk.aabbMax))
                    continue;

                // シャドウ用は 1 段粗い LOD (格子ステップ 2 = 三角形数 1/4) を使う。
                // WHY: 影の形はシャドウマップのテクセルと PCF カーネルで既に鈍っており、
                //      地形の最密メッシュを光源視点でもう一度流しても輪郭は変わらない。
                //      落ちるのは頂点処理と極小三角形のラスタライズだけなので、
                //      見た目を保ったまま地形シャドウのコストを大きく削れる。
                // NOTE: 粗い LOD が未生成のチャンクは LOD0 へフォールバックする。
                const int lod = (chunk.indexCountLOD[1] > 0 && chunk.indexBufferLOD[1].IsValid()) ? 1 : 0;

                renderer::DrawCall dc;
                dc.vertexBuffer       = chunk.vertexBuffer;
                dc.indexBuffer        = chunk.indexBufferLOD[lod];
                dc.indexCount         = chunk.indexCountLOD[lod];
                dc.shader             = shadowShader;
                dc.pipelineState      = pipelineState;
                dc.layer              = renderer::RenderLayer::OPAQUE_LAYER;
                dc.topology           = renderer::PrimitiveTopology::TRIANGLE_LIST;
                dc.constantBuffers[0] = frameCB;
                dc.constantBuffers[1] = objectCB;
                SubmitCountedShadow(ctx, dc);
            }
        }
    }
}

// ─── TerrainSelectionMaskSystem ───────────────────────────────────────────

void TerrainSelectionMaskSystem(RenderPassContext& ctx)
{
    if (!ctx.selectionOutlineEnabled) return;
    auto& h = ctx.handles;
    if (!h.selectionMaskShader.IsValid() || !h.selectionMaskPSO.IsValid()) return;

    for (auto [terrain, transform] : ctx.scene.View<TerrainComponent, Transform>()) {
        if (terrain.heightData.empty()) continue;

        EntityID eid{};
        for (EntityID candidate : ctx.scene.GetEntities<TerrainComponent>()) {
            if (ctx.scene.GetComponent<TerrainComponent>(candidate) == &terrain) {
                eid = candidate;
                break;
            }
        }
        if (!ctx.scene.IsValid(eid)) continue;

        const auto* go = ctx.scene.GetGameObject(eid);
        if (!go || !go->activeInHierarchy()) continue;
        if (!fbzz::Layer::Contains(ctx.cullingMask, go->layer)) continue;

        bool selected = false;
        for (const auto& sel : ctx.settings.selectedObjects) {
            if (sel.index == eid.index && sel.generation == eid.generation) {
                selected = true;
                break;
            }
        }
        if (!selected) continue;

        const math::Matrix4 world = transform.GetWorldMatrix();
        PerObjectCB objData{};
        objData.world             = world;
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(world);
        ctx.resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));

        for (auto& [key, chunk] : g_chunkCache) {
            if (key.entityId != eid) continue;

            renderer::DrawCall dc;
            dc.vertexBuffer       = chunk.vertexBuffer;
            dc.indexBuffer        = chunk.indexBufferLOD[0];
            dc.indexCount         = chunk.indexCountLOD[0];
            dc.shader             = h.selectionMaskShader;
            dc.pipelineState      = h.selectionMaskPSO;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            ctx.renderer.Submit(dc, ctx.resources);
        }
    }
}

} // namespace fbzz::scene
