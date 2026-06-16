// FBZZ Engine
// TerrainRenderSystem.cpp | fbzz::scene
// TerrainComponent → GPU チャンクメッシュ生成・描画の実装
//
// 実装:
//   テクスチャスロット (Terrain.hlsl と同期すること):
//   t0 = スプラットマップ  RGBA8 (R=layer0, G=layer1, B=layer2, A=layer3)
//   t1 = layer0 ディフューズ
//   t2 = layer1 ディフューズ
//   t3 = layer2 ディフューズ
//   t4 = layer3 ディフューズ
//   t5 = layer0 法線
//   t6 = layer1 法線
//   t7 = layer2 法線
//   t8 = layer3 法線
//   t9  = layer0 AO/Roughness (R=AO, G=Roughness)
//   t10 = layer1 AO/Roughness
//   t11 = layer2 AO/Roughness
//   t12 = layer3 AO/Roughness
//   t13 = shadow depth
//
// サンプラースロット (Terrain.hlsl と同期すること):
//   s0 = WRAP_ANISOTROPIC  ディフューズテクスチャ用
//   s1 = BORDER_ZERO       shadow PCF 用比較サンプラー
//   s2 = CLAMP_LINEAR      スプラットマップ用（UV が [0,1] を超えないようクランプ）
//
// 設計上の注意:
//   - シングルスレッド前提。static ローカルによる遅延初期化を使う。
//   - GPU バッファは ResourceHandle で所有し、static map でエンティティごとにキャッシュする。
//   - heightDirty: 全チャンクを削除して再構築
//   - splatDirty : スプラットマップ + レイヤーテクスチャを再ロード
#include "Engine/Scene/Systems/TerrainRenderSystem.hpp"
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
#include <array>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::scene {

// =============================================================================
// 内部型定義
// =============================================================================

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

// チャンク LOD 段階数とステップサイズ
// LOD 0 = step 1 (full), LOD 1 = step 2 (1/4 poly), LOD 2 = step 4 (1/16 poly)
static constexpr int kLODCount           = 3;
static constexpr int kLODSteps[kLODCount] = { 1, 2, 4 };

// チャンク 1 枚の GPU リソースと AABB
struct TerrainChunk {
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer;
    std::array<renderer::ResourceHandle<renderer::BufferTag>, kLODCount> indexBufferLOD;
    std::array<uint32_t, kLODCount> indexCountLOD = {};
    math::Vector3 aabbMin;
    math::Vector3 aabbMax;
};

// チャンクキャッシュキー
struct TerrainChunkKey {
    EntityID entityId;
    uint32_t chunkX = 0;
    uint32_t chunkZ = 0;
    bool operator==(const TerrainChunkKey& o) const = default;
};

} // namespace fbzz::scene

// TerrainChunkKey の std::hash 特殊化
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

// エンティティ 1 つのテクスチャセット (Phase 4)
// splatDirty が立つたびに全テクスチャを再ロードする
struct TerrainTextures {
    renderer::ResourceHandle<renderer::TextureTag> splatmap;        // t0
    std::array<renderer::ResourceHandle<renderer::TextureTag>, 4> diffuse; // t1-t4
    std::array<renderer::ResourceHandle<renderer::TextureTag>, 4> normal;  // t5-t8
    std::array<renderer::ResourceHandle<renderer::TextureTag>, 4> aoRoughness; // t9-t12
};

static std::unordered_map<TerrainChunkKey, TerrainChunk> g_chunkCache;
static std::unordered_map<uint32_t, TerrainTextures>    g_texCache;

// グリッド隣接 Terrain 参照。BuildChunk がエッジ頂点の高さを合わせるために使う。
// nullptr = 隣が存在しない or グリッド未登録
struct TerrainNeighbors {
    const TerrainComponent* north = nullptr; // gz - 1 (Z 負方向)
    const TerrainComponent* south = nullptr; // gz + 1 (Z 正方向)
    const TerrainComponent* west  = nullptr; // gx - 1 (X 負方向)
    const TerrainComponent* east  = nullptr; // gx + 1 (X 正方向)
};

// =============================================================================
// 定数バッファ構造体 — HLSL 側と完全に一致させること
// =============================================================================

// CameraConstants (b0) — PerFrameCB と同じレイアウト
struct TerrainCameraFrameCB {
    math::Matrix4 view;
    math::Matrix4 projection;
    math::Matrix4 viewProjection;
    math::Matrix4 invViewProjection;
    math::Vector3 cameraPos; float nearZ;
    float         farZ;      float _pad[3];
};
static_assert(sizeof(TerrainCameraFrameCB) == 288, "PerFrameCB size mismatch");

// TerrainCB (b1) — チャンクごとに更新
struct TerrainObjectCB {
    math::Matrix4 worldMatrix;
    math::Matrix4 wvpMatrix;
    math::Vector4 layerTiling[4];       // xy = tilingX, tilingZ per layer
    math::Vector4 layerNormalStrength;  // xyzw = normalStrength per layer
    math::Vector4 layerMaterial[4];     // x=roughness, y=AO
    math::Vector4 layerTextureFlags;     // xyzw = AO/Roughness texture exists per layer
    math::Vector4 layerAutoHeight[4];    // x=minHeight, y=maxHeight, z=fade, w=enabled
    math::Vector4 layerAutoSlope[4];     // x=minSlope, y=maxSlope, z=fade, w=strength
};
static_assert(sizeof(TerrainObjectCB) == 416, "TerrainObjectCB size mismatch");

// マテリアルパラメータキャッシュ: entity index → TerrainObjectCB の静的部分
// splatDirty が立ったときに一緒に無効化する（マテリアル変更を検知）。
static std::unordered_map<uint32_t, TerrainObjectCB> g_cbParamCache;

// =============================================================================
// チャンクメッシュ生成
// =============================================================================

// 頂点のみ生成。インデックスは BuildChunkLODIndices で別途生成する。
// neighbors: グリッド隣接 Terrain。エッジ頂点の高さを隣接 Terrain と平均して継ぎ目を消す。
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
            const size_t idx = static_cast<size_t>(z) * static_cast<size_t>(terrain.columns)
                             + static_cast<size_t>(x);
            float h = terrain.heightData[idx] * terrain.maxHeight;

            // エッジ頂点: 隣接 Terrain の対応頂点と平均して継ぎ目を解消する。
            // WHY: 両側の Terrain が同じ高さになるため GPU 上でギャップが生じない。
            //      heightData の書き換えは行わず、レンダリング専用の調整とする。
            if (x == 0 && neighbors.west && z < neighbors.west->rows) {
                const float nh = neighbors.west->heightData[
                    static_cast<size_t>(z) * static_cast<size_t>(neighbors.west->columns)
                    + static_cast<size_t>(neighbors.west->columns - 1)] * neighbors.west->maxHeight;
                h = (h + nh) * 0.5f;
            } else if (x == terrain.columns - 1 && neighbors.east && z < neighbors.east->rows) {
                const float nh = neighbors.east->heightData[
                    static_cast<size_t>(z) * static_cast<size_t>(neighbors.east->columns)
                    + 0] * neighbors.east->maxHeight;
                h = (h + nh) * 0.5f;
            }
            if (z == 0 && neighbors.north && x < neighbors.north->columns) {
                const float nh = neighbors.north->heightData[
                    static_cast<size_t>(neighbors.north->rows - 1) * static_cast<size_t>(neighbors.north->columns)
                    + static_cast<size_t>(x)] * neighbors.north->maxHeight;
                h = (h + nh) * 0.5f;
            } else if (z == terrain.rows - 1 && neighbors.south && x < neighbors.south->columns) {
                const float nh = neighbors.south->heightData[
                    static_cast<size_t>(0) * static_cast<size_t>(neighbors.south->columns)
                    + static_cast<size_t>(x)] * neighbors.south->maxHeight;
                h = (h + nh) * 0.5f;
            }

            TerrainVertex v;
            v.position = {
                static_cast<float>(x) * terrain.cellSize,
                h,
                static_cast<float>(z) * terrain.cellSize
            };
            v.normal = terrain.ComputeNormal(x, z);

            // タンジェントは UV の U 方向 (+X) に沿った地形面の接線。
            // 隣接 heightData の中心差分から dh/dx を求め、normalize(cellSize, dh, 0) とする。
            {
                const int txL = std::max(x - 1, 0);
                const int txR = std::min(x + 1, terrain.columns - 1);
                const float hL = terrain.heightData[static_cast<size_t>(z) * terrain.columns + txL] * terrain.maxHeight;
                const float hR = terrain.heightData[static_cast<size_t>(z) * terrain.columns + txR] * terrain.maxHeight;
                const float span = static_cast<float>(txR - txL) * terrain.cellSize;
                const float dhDx = (hR - hL) / span;
                v.tangent = math::Vector3{ terrain.cellSize, dhDx * terrain.cellSize, 0.0f }.Normalized();
            }

            v.uv     = {
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

// step グリッド間隔でチャンクのインデックスを生成する。
// 頂点バッファは全解像度を保持しているため、step > 1 でも頂点を間引かずに
// インデックスだけをスキップすることでポリゴン数を削減できる。
// step=1 → LOD 0 (full), step=2 → LOD 1 (1/4 poly), step=4 → LOD 2 (1/16 poly)
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

// =============================================================================
// フラスタムカリングヘルパー
// =============================================================================

// ローカル AABB をワールド空間に変換して視錐台と交差判定する
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

// =============================================================================
// テクスチャキャッシュ管理 (Phase 4)
// =============================================================================

// スプラットマップテクスチャを構築する。
// splatData が空なら (255,0,0,0) の 1×1 テクスチャ（layer0 100%）を返す。
static renderer::ResourceHandle<renderer::TextureTag> BuildSplatmapTexture(
    const TerrainComponent&    terrain,
    renderer::ResourceManager& resources,
    renderer::ResourceHandle<renderer::TextureTag> fallback)
{
    if (terrain.splatData.empty()) return fallback;

    // splatData のサイズが columns × rows × 4 と一致するか確認
    // WHY: 不整合があると GPU テクスチャが壊れるため、assert でバグを早期検出する
    assert(terrain.splatData.size() ==
           static_cast<size_t>(terrain.columns) * static_cast<size_t>(terrain.rows) * 4u);

    return resources.CreateTexture(
        terrain.splatData.data(),
        static_cast<uint32_t>(terrain.columns),
        static_cast<uint32_t>(terrain.rows));
}

// fzmat params へのアクセスヘルパー（TerrainRenderSystem ローカル版）
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

// エンティティのテクスチャセット（スプラットマップ + 4 レイヤー）を構築する。
// 各レイヤーは独立した MaterialAsset から diffuse/normal/ao_roughness を取得する。
// 未設定レイヤーには白/フラット法線/黒テクスチャをバインドして HLSL 側の分岐を排除する。
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

// =============================================================================
// TerrainRenderSystem — メイン関数
// =============================================================================

void TerrainRenderSystem(
    Scene&                                        scene,
    renderer::IRenderer&                          renderer,
    renderer::ResourceManager&                    resources,
    const renderer::Camera&                       camera,
    renderer::ResourceHandle<renderer::RenderTargetTag> outputRT,
    const renderer::RenderSettings* settings,
    renderer::ResourceHandle<renderer::TextureTag> shadowDepthTexture,
    renderer::ResourceHandle<renderer::ConstantBufferTag> shadowCB,
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightCB)
{
    // =========================================================================
    // 静的リソースの遅延初期化（初回呼び出し時のみ実行）
    // =========================================================================
    static auto terrainShader = resources.LoadShader("assets/shaders/Terrain/Terrain.hlsl");
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

    // フォールバック: 1×1 白テクスチャ（未設定レイヤーのディフューズ代替）
    static auto s_whiteTex = [&] {
        const uint8_t w[4] = { 255, 255, 255, 255 };
        return resources.CreateTexture(w, 1, 1);
    }();

    // フォールバック: 1×1 スプラットマップ (R=255, G=B=A=0 → layer0 100%)
    // splatData が空の地形は layer0 のみ全面に表示する
    static auto s_splatFallback = [&] {
        const uint8_t s[4] = { 255, 0, 0, 0 };
        return resources.CreateTexture(s, 1, 1);
    }();

    // フォールバック: 1×1 フラット法線 (DX/Tangent space: +Z)
    static auto s_flatNormalTex = [&] {
        const uint8_t n[4] = { 128, 128, 255, 255 };
        return resources.CreateTexture(n, 1, 1);
    }();

    // フォールバック: AO/Roughness テクスチャ未設定を表す黒。実値は CB 側の数値を使う。
    static auto s_blackTex = [&] {
        const uint8_t b[4] = { 0, 0, 0, 255 };
        return resources.CreateTexture(b, 1, 1);
    }();

    // チャンク/テクスチャキャッシュは SubmitTerrainShadowCasters と共有する。

    // 無効になったエンティティのキャッシュを解放する。
    // WHY: エンティティ破棄後も static cache にエントリが残るとメモリリークになる。
    //      毎フレームの走査コストは低い（Terrain 数は通常 1〜数個）。
    {
        // 現在有効な TerrainComponent エンティティの index セットを構築する
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
        std::erase_if(g_texCache, [&validIndices](auto& kv) {
            return !validIndices.count(kv.first);
        });
        std::erase_if(g_cbParamCache, [&validIndices](auto& kv) {
            return !validIndices.count(kv.first);
        });
    }

    // =========================================================================
    // 描画先 RT を設定する
    // WHY: RenderSystem はポストプロセス後に最終 RT を差し替える場合がある。
    //      TerrainRenderSystem は RenderSystem の直後に呼ばれるが、
    //      その時点でバインドされている RT が正しいとは限らない。
    //      outputRT が有効なら明示的に SetRenderTarget を呼んで保証する。
    if (outputRT.IsValid())
        renderer.SetRenderTarget(outputRT, resources);

    // =========================================================================
    // サンプラー設定 (Phase 4)
    // 毎フレーム設定する。他の描画パスが s1 を上書きしても Terrain 描画前に復元される。
    // WHY: s0 = WRAP_ANISOTROPIC_4X で地形ディフューズをタイリングする。
    //      x16 の約 1/3 コストで斜め視線の縦縞ノイズを抑えられる。
    //      s1 = BORDER_ZERO でシャドウマップの範囲外を非遮蔽として扱う。
    //      s2 = CLAMP_LINEAR でスプラットマップを UV [0,1] の境界に正確にクランプする。
    // =========================================================================
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC_4X);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);
    renderer.SetSampler(2, renderer::SamplerMode::CLAMP_LINEAR);

    // =========================================================================
    // カメラ定数バッファを更新
    // =========================================================================
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

    // TerrainGridComponent を取得（シーンに 1 つだけ存在する想定）
    // WHY: GetComponents は vector を返すため毎フレームのヒープ確保を避けるよう
    //      GetEntities (span) で先頭エンティティだけを引く。
    TerrainGridComponent* terrainGrid = nullptr;
    {
        const auto gridEntities = scene.GetEntities<TerrainGridComponent>();
        if (!gridEntities.empty())
            terrainGrid = scene.GetComponent<TerrainGridComponent>(gridEntities.front());
    }

    // =========================================================================
    // TerrainComponent の走査と描画
    // =========================================================================
    for (auto [terrain, transform] : scene.View<TerrainComponent, Transform>()) {
        if (!terrain.enabled || terrain.heightData.empty()) continue;

        assert(terrain.heightData.size() == static_cast<size_t>(terrain.columns)
                                          * static_cast<size_t>(terrain.rows));
        assert(terrain.chunkSize > 0);

        // EntityID を逆引き
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

        // グリッド上の位置を取得し隣接 Terrain を解決する
        TerrainNeighbors neighbors;
        if (terrainGrid) {
            int gx = 0, gz = 0;
            if (terrainGrid->TryGetGridPos(eid, gx, gz)) {
                auto resolveNeighbor = [&](int ngx, int ngz) -> const TerrainComponent* {
                    const EntityID neid = terrainGrid->GetCell(ngx, ngz);
                    if (!scene.IsValid(neid)) return nullptr;
                    return scene.GetComponent<TerrainComponent>(neid);
                };
                neighbors.north = resolveNeighbor(gx,     gz - 1);
                neighbors.south = resolveNeighbor(gx,     gz + 1);
                neighbors.west  = resolveNeighbor(gx - 1, gz);
                neighbors.east  = resolveNeighbor(gx + 1, gz);
            }
        }

        // 4 レイヤーの fzmat を解決する。毎フレーム GetMaterial を呼ぶが AssetManager 側でキャッシュされる。
        std::array<const asset::MaterialAsset*, 4> layerMats = {};
        for (int li = 0; li < 4; ++li) {
            if (!terrain.layerMaterials[li].empty()) {
                const auto handle = asset::AssetManager::LoadMaterial(terrain.layerMaterials[li]);
                layerMats[li] = asset::AssetManager::GetMaterial(handle);
            }
        }

        // -- heightDirty: 全チャンクを削除して再構築 ----------------------------
        if (terrain.heightDirty) {
            std::erase_if(g_chunkCache, [&eid](const auto& kv) {
                return kv.first.entityId == eid;
            });
            // 隣接 Terrain のエッジチャンクも再構築が必要なため、neighbors の heightDirty を立てる。
            // WHY: エッジ頂点は隣接データを参照して構築されるため、
            //      自身が変化したときに隣接のキャッシュも無効化しなければ継ぎ目が残る。
            auto dirtyNeighbor = [&](const TerrainComponent* n) {
                if (n) const_cast<TerrainComponent*>(n)->heightDirty = true;
            };
            dirtyNeighbor(neighbors.north);
            dirtyNeighbor(neighbors.south);
            dirtyNeighbor(neighbors.west);
            dirtyNeighbor(neighbors.east);
            terrain.heightDirty = false;
        }

        // CB パラメータ再構築ヘルパー（splatDirty / materialParamDirty 共用）
        auto RebuildCBParams = [&]() {
            TerrainObjectCB cb{};
            float normalStr[4]           = { 1.0f, 1.0f, 1.0f, 1.0f };
            float materialTextureFlags[4] = {};
            for (int li = 0; li < 4; ++li) {
                const asset::MaterialAsset* m = layerMats[li];
                normalStr[li]            = TGetF(m, "normalStrength", 1.0f);
                const bool hasAoTex      = m && m->textures.count("ao_roughness") &&
                                           !m->textures.at("ao_roughness").empty();
                materialTextureFlags[li] = hasAoTex ? 1.0f : 0.0f;
                cb.layerTiling[li]      = { TGetF(m, "tilingX", 8.0f),
                                            TGetF(m, "tilingZ", 8.0f), 0.0f, 0.0f };
                cb.layerMaterial[li]    = { TGetF(m, "roughness",        0.8f),
                                            TGetF(m, "ambientOcclusion", 1.0f), 0.0f, 0.0f };
                cb.layerAutoHeight[li]  = { TGetF(m, "autoMinHeight",    -10000.0f),
                                            TGetF(m, "autoMaxHeight",     10000.0f),
                                            TGetF(m, "autoHeightFade",    1.0f),
                                            TGetF(m, "autoBlendEnabled",  0.0f) };
                cb.layerAutoSlope[li]   = { TGetF(m, "autoMinSlope",      0.0f),
                                            TGetF(m, "autoMaxSlope",      1.0f),
                                            TGetF(m, "autoSlopeFade",     0.1f),
                                            TGetF(m, "autoBlendStrength", 1.0f) };
            }
            cb.layerNormalStrength = { normalStr[0], normalStr[1], normalStr[2], normalStr[3] };
            cb.layerTextureFlags   = { materialTextureFlags[0], materialTextureFlags[1],
                                       materialTextureFlags[2], materialTextureFlags[3] };
            g_cbParamCache[eid.index] = cb;
        };

        // splatDirty: スプラットマップ + レイヤーテクスチャ + CB を全再構築
        const bool needTexRebuild = terrain.splatDirty || !g_texCache.contains(eid.index);
        if (needTexRebuild) {
            g_texCache[eid.index] = BuildTextureSet(layerMats, terrain, resources,
                                                    s_splatFallback, s_whiteTex,
                                                    s_flatNormalTex, s_blackTex);
            RebuildCBParams();
            terrain.splatDirty = false;
        }

        // materialParamDirty: float パラメータのみ変更 → CB だけ更新、テクスチャ再アップロード不要
        if (terrain.materialParamDirty) {
            RebuildCBParams();
            terrain.materialParamDirty = false;
        }
        const TerrainTextures& textures = g_texCache.at(eid.index);

        // -- チャンク数計算 -------------------------------------------------------
        const int chunkCountX = (terrain.columns - 1 + terrain.chunkSize - 1) / terrain.chunkSize;
        const int chunkCountZ = (terrain.rows    - 1 + terrain.chunkSize - 1) / terrain.chunkSize;

        // -- TerrainCB を組み立てる。マテリアル部分はキャッシュ済みをコピーし
        //    worldMatrix / wvpMatrix だけ毎フレーム更新する --------------------
        const math::Matrix4 world = transform.GetWorldMatrix();
        TerrainObjectCB terrainCBData = g_cbParamCache.count(eid.index)
                                      ? g_cbParamCache.at(eid.index)
                                      : TerrainObjectCB{};
        terrainCBData.worldMatrix = world;
        terrainCBData.wvpMatrix   = camera.GetViewProjection() * world;

        // CB を地形ごとに 1 回だけ GPU へ転送する（チャンクループの外）。
        // WHY: TerrainObjectCB は全チャンクで同一内容のため、チャンクごとに Update するのは無駄。
        resources.Update(terrainCBH, &terrainCBData, sizeof(terrainCBData));

        // -- チャンクごとのメッシュ生成・フラスタムカリング・描画 -----------------
        for (int cz = 0; cz < chunkCountZ; ++cz) {
            for (int cx = 0; cx < chunkCountX; ++cx) {
                const TerrainChunk& chunk = EnsureTerrainChunk(terrain, eid, cx, cz, neighbors, resources);

                // チャンク単位のフラスタムカリング
                if (!IsChunkVisible(frustum, world, chunk.aabbMin, chunk.aabbMax))
                    continue;

                // カメラ距離からLODレベルを決定する
                // チャンクローカル中心をワールド変換して距離を算出する
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

                // LOD 境界距離: chunkWorldSize の 2 倍・6 倍を閾値とする
                // chunkSize=32, cellSize=1.0 のとき LOD 0 < 64m, LOD 1 < 192m, LOD 2 >= 192m
                const float chunkWorldSize = static_cast<float>(terrain.chunkSize) * terrain.cellSize;
                const float d0 = chunkWorldSize * 2.0f;
                const float d1 = chunkWorldSize * 6.0f;
                const int lod = (distSq < d0 * d0) ? 0
                              : (distSq < d1 * d1) ? 1
                              : 2;

                const auto activeShader = terrainShader;

                // DrawCall を構築して発行
                renderer::DrawCall call;
                call.vertexBuffer  = chunk.vertexBuffer;
                call.indexBuffer   = chunk.indexBufferLOD[lod];
                call.shader        = activeShader;
                call.pipelineState = (settings && settings->IsWireframe()) ? terrainWireframePSO : terrainPSO;
                call.indexCount    = chunk.indexCountLOD[lod];
                call.layer         = renderer::RenderLayer::OPAQUE_LAYER;
                call.topology      = renderer::PrimitiveTopology::TRIANGLE_LIST;

                // 定数バッファスロット
                //   [0]=CameraConstants(b0)  [1]=TerrainCB(b1)  [3]=LightConstants(b3)
                //   [4]=ShadowConstants(b4)
                call.constantBuffers[0] = cameraCBH;
                call.constantBuffers[1] = terrainCBH;
                call.constantBuffers[3] = lightCB;
                call.constantBuffers[4] = shadowCB;

                // テクスチャスロット
                //   [0]=splatmap(t0)  [1-4]=layer0-3 diffuse(t1-t4)
                //   [5-8]=layer0-3 normal(t5-t8)
                //   [9-12]=layer0-3 AO/Roughness(t9-t12)
                //   [13]=directional shadow depth(t13)
                call.textures[0] = textures.splatmap;
                call.textures[1] = textures.diffuse[0];
                call.textures[2] = textures.diffuse[1];
                call.textures[3] = textures.diffuse[2];
                call.textures[4] = textures.diffuse[3];
                call.textures[5] = textures.normal[0];
                call.textures[6] = textures.normal[1];
                call.textures[7] = textures.normal[2];
                call.textures[8] = textures.normal[3];
                call.textures[9]  = textures.aoRoughness[0];
                call.textures[10] = textures.aoRoughness[1];
                call.textures[11] = textures.aoRoughness[2];
                call.textures[12] = textures.aoRoughness[3];
                call.textures[13] = shadowDepthTexture;

                renderer.Submit(call, resources);
            }
        }
    }
}

void SubmitTerrainShadowCasters(
    Scene&                                        scene,
    renderer::IRenderer&                          renderer,
    renderer::ResourceManager&                    resources,
    const math::Frustum&                          lightFrustum,
    renderer::ResourceHandle<renderer::ShaderTag> shadowShader,
    renderer::ResourceHandle<renderer::PipelineStateTag> pipelineState,
    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB,
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB)
{
    // Terrain は MeshRenderer を持たないため、通常の ShadowPass 走査では拾われない。
    // ここで Terrain のチャンクキャッシュを共有し、ライト視点の深度だけを書き込む。
    // WHY: ライト種別・shadow atlas・cascade の選択は ShadowPass 側に閉じ込め、
    //      Terrain 側は「指定されたライト視錐台へ描けるチャンクを提出する」だけにする。
    if (!shadowShader.IsValid() || !pipelineState.IsValid())
        return;

    struct ShadowObjectCB {
        math::Matrix4 world;
        math::Matrix4 worldInvTranspose;
    };

    for (auto [terrain, transform] : scene.View<TerrainComponent, Transform>()) {
        if (!terrain.enabled || terrain.heightData.empty())
            continue;

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
        if (!scene.IsValid(eid))
            continue;
        {
            const auto* go = scene.GetGameObject(eid);
            if (!go || !go->activeInHierarchy()) continue;
        }

        if (terrain.heightDirty) {
            std::erase_if(g_chunkCache, [&eid](const auto& kv) {
                return kv.first.entityId == eid;
            });
            terrain.heightDirty = false;
        }

        const int chunkCountX = (terrain.columns - 1 + terrain.chunkSize - 1) / terrain.chunkSize;
        const int chunkCountZ = (terrain.rows    - 1 + terrain.chunkSize - 1) / terrain.chunkSize;
        const math::Matrix4 world = transform.GetWorldMatrix();

        ShadowObjectCB objData{};
        objData.world             = world;
        objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(world));
        resources.Update(objectCB, &objData, sizeof(objData));

        for (int cz = 0; cz < chunkCountZ; ++cz) {
            for (int cx = 0; cx < chunkCountX; ++cx) {
                const TerrainChunk& chunk = EnsureTerrainChunk(terrain, eid, cx, cz, {}, resources);
                if (!IsChunkVisible(lightFrustum, world, chunk.aabbMin, chunk.aabbMax))
                    continue;

                renderer::DrawCall dc;
                dc.vertexBuffer       = chunk.vertexBuffer;
                dc.indexBuffer        = chunk.indexBufferLOD[0];
                dc.indexCount         = chunk.indexCountLOD[0];
                dc.shader             = shadowShader;
                dc.pipelineState      = pipelineState;
                dc.layer              = renderer::RenderLayer::OPAQUE_LAYER;
                dc.topology           = renderer::PrimitiveTopology::TRIANGLE_LIST;
                dc.constantBuffers[0] = frameCB;
                dc.constantBuffers[1] = objectCB;
                renderer.Submit(dc, resources);
            }
        }
    }
}

} // namespace fbzz::scene
