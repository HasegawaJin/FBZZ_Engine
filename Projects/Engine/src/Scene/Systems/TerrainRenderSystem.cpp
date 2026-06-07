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
//
// サンプラースロット (Terrain.hlsl と同期すること):
//   s0 = WRAP_ANISOTROPIC  ディフューズテクスチャ用
//   s1 = CLAMP_LINEAR      スプラットマップ用（UV が [0,1] を超えないようクランプ）
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
#include "Engine/Scene/Components/LightComponent.hpp"
#include "Engine/Scene/Entity.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/Camera.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/RenderSettings.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Engine/Renderer/SamplerMode.hpp"
#include "Engine/Renderer/LightSystem.hpp"
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

// エンティティ 1 つのテクスチャセット (Phase 4)
// splatDirty が立つたびに全テクスチャを再ロードする
struct TerrainTextures {
    renderer::ResourceHandle<renderer::TextureTag> splatmap;        // t0
    std::array<renderer::ResourceHandle<renderer::TextureTag>, 4> diffuse; // t1-t4
    std::array<renderer::ResourceHandle<renderer::TextureTag>, 4> normal;  // t5-t8
    std::array<renderer::ResourceHandle<renderer::TextureTag>, 4> aoRoughness; // t9-t12
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

// =============================================================================
// チャンクメッシュ生成
// =============================================================================

// 頂点のみ生成。インデックスは BuildChunkLODIndices で別途生成する。
static void BuildChunk(
    const TerrainComponent&      terrain,
    int                          cx,
    int                          cz,
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
            const float h = terrain.heightData[idx] * terrain.maxHeight;

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

// エンティティのテクスチャセット（スプラットマップ + 4 レイヤーディフューズ）を構築する。
// 未設定レイヤーには whiteTex をバインドして HLSL 側の分岐を排除する。
// WHY: シェーダーは常に 4 レイヤー固定でブレンドする設計（[unroll] ループ効率化）。
//      未設定レイヤーの splat ウェイトは 0 なので白テクスチャを掛けても寄与は 0 になる。
static TerrainTextures BuildTextureSet(
    const asset::MaterialAsset* mat,
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
        char pfx[16]; snprintf(pfx, sizeof(pfx), "layer%d_", i);
        const std::string diffusePath     = TGetTex(mat, std::string(pfx) + "diffuse");
        const std::string normalPath      = TGetTex(mat, std::string(pfx) + "normal");
        const std::string aoRoughnessPath = TGetTex(mat, std::string(pfx) + "ao_roughness");
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
    const renderer::RenderSettings* settings)
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
    static auto lightCBH   = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));

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

    // チャンクキャッシュ: テレインエンティティ × チャンクグリッド → GPU バッファ
    static std::unordered_map<TerrainChunkKey, TerrainChunk> s_chunkCache;

    // テクスチャキャッシュ: entityId.index → TerrainTextures
    // WHY: エンティティ index を使う理由は、generation が異なっても地形エンティティを
    //      削除・再作成するケースはまれで、index で十分識別できるため。
    static std::unordered_map<uint32_t, TerrainTextures> s_texCache;

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
    //      s1 = CLAMP_LINEAR でスプラットマップを UV [0,1] の境界に正確にクランプする。
    // =========================================================================
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC_4X);
    renderer.SetSampler(1, renderer::SamplerMode::CLAMP_LINEAR);

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

    // =========================================================================
    // ライト定数バッファを Scene から収集して更新
    // =========================================================================
    {
        constexpr float kDegToRad = 3.14159265f / 180.0f;
        renderer::LightConstantsCB lightData{};
        lightData.lightDir       = { 0.0f, -1.0f, 0.5f };
        lightData.lightColor     = { 1.0f,  1.0f, 1.0f };
        lightData.lightIntensity = 1.0f;

        for (auto [tf, lc] : scene.View<Transform, LightComponent>()) {
            if (!lc.enabled) continue;
            if (lc.type == LightComponent::Type::Directional) {
                lightData.lightDir       = tf.Forward().Normalized();
                lightData.lightColor     = lc.color;
                lightData.lightIntensity = lc.intensity;
            } else if (lc.type == LightComponent::Type::Point && lightData.pointLightCount < 8) {
                auto& pl    = lightData.pointLights[lightData.pointLightCount++];
                pl.position = tf.position;  pl.range    = lc.range;
                pl.color    = lc.color;     pl.intensity = lc.intensity;
            } else if (lc.type == LightComponent::Type::Spot && lightData.spotLightCount < 4) {
                auto& sl      = lightData.spotLights[lightData.spotLightCount++];
                sl.position   = tf.position;
                sl.direction  = tf.Forward().Normalized();
                sl.range      = lc.range;
                sl.innerCos   = std::cos(lc.innerCone * kDegToRad);
                sl.outerCos   = std::cos(lc.outerCone * kDegToRad);
                sl.color      = lc.color;
                sl.intensity  = lc.intensity;
            }
        }
        lightData.ambientColor = { 0.08f, 0.08f, 0.08f };
        if (settings && settings->IsUnlit()) {
            lightData.ambientColor    = { 1.0f, 1.0f, 1.0f };
            lightData.lightIntensity  = 0.0f;
            lightData.pointLightCount = 0;
            lightData.spotLightCount  = 0;
        }
        resources.Update(lightCBH, &lightData, sizeof(lightData));
    }

    const math::Frustum frustum = math::Frustum::FromViewProjection(camera.GetViewProjection());

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

        // fzmat を解決する。毎フレーム GetMaterial を呼ぶが AssetManager 側でキャッシュされる。
        const asset::MaterialAsset* mat = nullptr;
        if (!terrain.materialPath.empty()) {
            const auto handle = asset::AssetManager::LoadMaterial(terrain.materialPath);
            mat = asset::AssetManager::GetMaterial(handle);
        }

        // -- heightDirty: 全チャンクを削除して再構築 ----------------------------
        if (terrain.heightDirty) {
            std::erase_if(s_chunkCache, [&eid](const auto& kv) {
                return kv.first.entityId == eid;
            });
            terrain.heightDirty = false;
        }

        // -- splatDirty: スプラットマップ + レイヤーテクスチャを再構築 -----------
        if (terrain.splatDirty || !s_texCache.contains(eid.index)) {
            s_texCache[eid.index] = BuildTextureSet(mat, terrain, resources,
                                                    s_splatFallback, s_whiteTex,
                                                    s_flatNormalTex, s_blackTex);
            terrain.splatDirty = false;
        }
        const TerrainTextures& textures = s_texCache.at(eid.index);

        // -- チャンク数計算 -------------------------------------------------------
        const int chunkCountX = (terrain.columns - 1 + terrain.chunkSize - 1) / terrain.chunkSize;
        const int chunkCountZ = (terrain.rows    - 1 + terrain.chunkSize - 1) / terrain.chunkSize;

        // -- TerrainCB のレイヤーデータをセット（全チャンク共通） -----------------
        const math::Matrix4 world = transform.GetWorldMatrix();
        const math::Matrix4 wvp   = camera.GetViewProjection() * world;

        TerrainObjectCB terrainCBData{};
        terrainCBData.worldMatrix = world;
        terrainCBData.wvpMatrix   = wvp;
        {
            float normalStr[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            float materialTextureFlags[4] = {};
            for (int li = 0; li < 4; ++li) {
                char pfx[16]; snprintf(pfx, sizeof(pfx), "layer%d_", li);
                const std::string p = pfx;
                const float tx = TGetF(mat, p + "tilingX", 8.0f);
                const float tz = TGetF(mat, p + "tilingZ", 8.0f);
                normalStr[li]  = TGetF(mat, p + "normalStrength", 1.0f);
                const std::string aoKey = p + "ao_roughness";
                materialTextureFlags[li] = (mat && mat->textures.count(aoKey) &&
                                            !mat->textures.at(aoKey).empty()) ? 1.0f : 0.0f;
                terrainCBData.layerTiling[li] = { tx, tz, 0.0f, 0.0f };
                terrainCBData.layerMaterial[li] = {
                    TGetF(mat, p + "roughness",        0.8f),
                    TGetF(mat, p + "ambientOcclusion", 1.0f),
                    0.0f, 0.0f
                };
                terrainCBData.layerAutoHeight[li] = {
                    TGetF(mat, p + "autoMinHeight",    -10000.0f),
                    TGetF(mat, p + "autoMaxHeight",     10000.0f),
                    TGetF(mat, p + "autoHeightFade",    1.0f),
                    TGetF(mat, p + "autoBlendEnabled",  0.0f)
                };
                terrainCBData.layerAutoSlope[li] = {
                    TGetF(mat, p + "autoMinSlope",      0.0f),
                    TGetF(mat, p + "autoMaxSlope",      1.0f),
                    TGetF(mat, p + "autoSlopeFade",     0.1f),
                    TGetF(mat, p + "autoBlendStrength", 1.0f)
                };
            }
            terrainCBData.layerNormalStrength = {
                normalStr[0], normalStr[1], normalStr[2], normalStr[3]
            };
            terrainCBData.layerTextureFlags = {
                materialTextureFlags[0],
                materialTextureFlags[1],
                materialTextureFlags[2],
                materialTextureFlags[3]
            };
        }

        // -- チャンクごとのメッシュ生成・フラスタムカリング・描画 -----------------
        for (int cz = 0; cz < chunkCountZ; ++cz) {
            for (int cx = 0; cx < chunkCountX; ++cx) {
                const TerrainChunkKey key{ eid,
                    static_cast<uint32_t>(cx), static_cast<uint32_t>(cz) };

                // 未キャッシュなら構築
                if (!s_chunkCache.contains(key)) {
                    std::vector<TerrainVertex> verts;
                    math::Vector3 aabbMin, aabbMax;
                    int x0, z0, x1, z1;
                    BuildChunk(terrain, cx, cz, verts, aabbMin, aabbMax, x0, z0, x1, z1);

                    TerrainChunk chunk;
                    chunk.aabbMin      = aabbMin;
                    chunk.aabbMax      = aabbMax;
                    chunk.vertexBuffer = resources.CreateVertexBuffer(
                        verts.data(),
                        verts.size() * sizeof(TerrainVertex),
                        static_cast<uint32_t>(sizeof(TerrainVertex)));

                    for (int lod = 0; lod < kLODCount; ++lod) {
                        std::vector<uint32_t> indices;
                        BuildChunkLODIndices(x0, z0, x1, z1, kLODSteps[lod], indices);
                        chunk.indexCountLOD[lod] = static_cast<uint32_t>(indices.size());
                        chunk.indexBufferLOD[lod] = resources.CreateIndexBuffer(
                            indices.data(),
                            chunk.indexCountLOD[lod]);
                    }
                    s_chunkCache[key] = std::move(chunk);
                }

                const TerrainChunk& chunk = s_chunkCache.at(key);

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

                resources.Update(terrainCBH, &terrainCBData, sizeof(terrainCBData));

                // fzmat のシェーダーパスを優先し、未設定時は静的フォールバックを使う。
                const auto activeShader = (mat && !mat->shaderPath.empty())
                    ? resources.LoadShader(mat->shaderPath)
                    : terrainShader;

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
                call.constantBuffers[0] = cameraCBH;
                call.constantBuffers[1] = terrainCBH;
                call.constantBuffers[3] = lightCBH;

                // テクスチャスロット
                //   [0]=splatmap(t0)  [1-4]=layer0-3 diffuse(t1-t4)
                //   [5-8]=layer0-3 normal(t5-t8)
                //   [9-12]=layer0-3 AO/Roughness(t9-t12)
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

                renderer.Submit(call, resources);
            }
        }
    }
}

} // namespace fbzz::scene
