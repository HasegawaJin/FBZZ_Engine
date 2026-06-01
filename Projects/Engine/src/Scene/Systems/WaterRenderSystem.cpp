// FBZZ Engine
// WaterRenderSystem.cpp | fbzz::scene
// WaterComponent → GPU 水面メッシュ・泡マスク・波紋テクスチャ生成と描画
//
// WHY: 水面は透明描画、Terrain 高さ参照、動的 CPU テクスチャ更新をまとめて扱う。
//      Component に GPU リソースを持たせず System 側の static cache に閉じることで、
//      Scene データは保存しやすい純粋なパラメータのまま保つ。
#include "Engine/Scene/Systems/WaterRenderSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/LightComponent.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/ParticleEmitter.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include "Engine/Scene/Components/WaterComponent.hpp"
#include "Engine/Renderer/IShader.hpp"
#include "Engine/Renderer/Material.hpp"
#include "Engine/Renderer/Camera.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/LightSystem.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Engine/Renderer/SamplerMode.hpp"
#include <Math/Frustum.hpp>
#include <Math/Matrix4.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {

namespace {

// WaterVertex — HLSL の WaterVSInput と一致する頂点レイアウト。
// WHAT: POSITION(float3) + TEXCOORD0(float2) の 20 bytes。法線は GPU 側で解析的に作る。
struct WaterVertex {
    math::Vector3 position;
    math::Vector2 uv;
};

// WaterChunk — チャンク 1 個分の GPU リソースと AABB。
// WHY: 水面全体を 1 DrawCall で描くと広大な水域でカリングできないため、
//      chunkCount×chunkCount のサブメッシュに分割して個別に視錐台テストする。
struct WaterChunk {
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer;
    renderer::ResourceHandle<renderer::BufferTag> indexBuffer;
    uint32_t indexCount = 0;
    math::Vector3 aabbMin; // ローカル空間 AABB（変換前）
    math::Vector3 aabbMax;
};

struct WaterMesh {
    std::vector<WaterChunk> chunks;
    math::Vector3 aabbMin; // 全体 AABB（早期リジェクト用）
    math::Vector3 aabbMax;
};

struct WaterTextures {
    renderer::ResourceHandle<renderer::TextureTag> normalMap1;
    renderer::ResourceHandle<renderer::TextureTag> normalMap2;
    renderer::ResourceHandle<renderer::TextureTag> foamTex;
    renderer::ResourceHandle<renderer::TextureTag> foamMask;
    renderer::ResourceHandle<renderer::TextureTag> envTex;
    renderer::ResourceHandle<renderer::TextureTag> flowMap;
};

struct WaterRipple {
    math::Vector2 positionUV;
    float amplitude = 0.0f;
    float radius = 0.0f;
    float speed = 0.25f;
    float decayRate = 1.5f;
    float waveWidth = 0.038f;  // 0.025 では 128px に対して細すぎて潰れる
};

struct WaterRippleState {
    std::vector<WaterRipple> ripples;
    std::vector<uint8_t> pixels;
    uint32_t width = 128;
    uint32_t height = 128;
    renderer::ResourceHandle<renderer::TextureTag> gpuTex;
    bool dirty = true;
};

// WaterCB — Assets/Shaders/Water/Water.hlsl の WaterCB と完全に一致させる。
// WHY: HLSL の cbuffer は 16 byte 境界でパックされるため、C++ 側も Vector4 単位で揃える。
struct WaterCB {
    math::Matrix4 worldMatrix;
    math::Matrix4 wvpMatrix;
    math::Vector4 shallowColorDepth;      // xyz=shallowColor, w=shallowDepth
    math::Vector4 deepColorDepth;         // xyz=deepColor, w=deepDepth
    math::Vector4 surfaceParams;          // x=opacity, y=reflectivity, z=fresnelBias, w=fresnelPower
    math::Vector4 normalMap1Params;       // xy=scroll, z=tiling, w=normalStrength
    math::Vector4 normalMap2Params;       // xy=scroll, z=tiling, w=time
    math::Vector4 foamParams;             // x=threshold, y=fade, z=strength, w=tiling
    math::Vector4 refractionFlowParams;   // x=refraction, y=flowSpeed, z=flowTiling, w=enableFlow
    math::Vector4 waveDir[4];             // xy=direction, z=steepness, w=enable
    math::Vector4 waveParams[4];          // x=amplitude, y=wavelength, z=omega, w=k
};

// WaterEffectParams — MaterialConstants cbuffer (b2) の C++ ミラー。
// MaterialComponent が存在しないときのフォールバックデフォルト値もここに保持する。
// WHY: HLSL の MaterialConstants と同じレイアウト(48 bytes = 3×float4)にし、
//      Upload でそのまま GPU へコピーできるようにする。
struct WaterEffectParams {
    float rimGlowStrength    = 0.40f;
    float minShallowAlpha    = 0.65f;
    float specularStrength   = 0.75f;
    float specularExponent   = 80.0f;
    float skyReflectTint[3]  = { 0.45f, 0.82f, 1.0f };
    float envMapBlend        = 0.35f;
    float rippleRingColor[3] = { 0.88f, 0.97f, 1.0f };
    float rippleRingStrength = 0.72f;
};
static_assert(sizeof(WaterEffectParams) == 48, "WaterEffectParams layout mismatch with MaterialConstants");

static_assert(sizeof(WaterVertex) == 20, "WaterVertex size mismatch");
static_assert(sizeof(WaterCB) == 368, "WaterCB size mismatch");

static std::unordered_map<uint32_t, WaterMesh> s_meshCache;
static std::unordered_map<uint32_t, WaterTextures> s_texCache;
static std::unordered_map<uint32_t, WaterRippleState> s_rippleStates;

// ── スプラッシュ ───────────────────────────────────────────────────────────
// SplashEvent — QueueWaterSplash で積まれ、WaterRenderSystem が消費する。
struct SplashEvent {
    math::Vector3 worldPos;
    float         intensity;
};

static std::vector<SplashEvent> s_pendingSplashes;
static std::vector<EntityID>    s_splashGos;

float SmoothStep(float edge0, float edge1, float x)
{
    const float denom = edge1 - edge0;
    if (std::abs(denom) <= math::EPSILON)
        return x < edge0 ? 0.0f : 1.0f;
    const float t = math::Clamp01((x - edge0) / denom);
    return t * t * (3.0f - 2.0f * t);
}

float ComputeFoamWeight(float heightDiff, float threshold, float fade)
{
    // WHAT: 地形が水面に近いほど 1、深いほど 0。smoothstep の端を逆にして岸辺を強調する。
    return SmoothStep(threshold + fade, threshold - fade, heightDiff);
}

EntityID FindEntityForWater(Scene& scene, WaterComponent& water)
{
    for (EntityID candidate : scene.GetEntities<WaterComponent>()) {
        if (scene.GetComponent<WaterComponent>(candidate) == &water)
            return candidate;
    }
    return {};
}

// WHAT: 水面を chunkCount×chunkCount のサブメッシュに分割して GPU にアップロードする。
// WHY: サブメッシュごとに AABB を持たせ、視錐台テストで非表示チャンクをスキップする。
//      全体 AABB は早期リジェクト用に mesh.aabbMin/Max に保存する。
void BuildWaterMesh(const WaterComponent& water, WaterMesh& mesh, renderer::ResourceManager& resources)
{
    assert(water.resolutionX > 0);
    assert(water.resolutionZ > 0);

    // 既存チャンクを解放する
    for (auto& chunk : mesh.chunks) {
        if (chunk.vertexBuffer.IsValid()) resources.Release(chunk.vertexBuffer);
        if (chunk.indexBuffer.IsValid()) resources.Release(chunk.indexBuffer);
    }
    mesh.chunks.clear();

    const float dx = water.extentX / static_cast<float>(water.resolutionX);
    const float dz = water.extentZ / static_cast<float>(water.resolutionZ);
    const float ox = -water.extentX * 0.5f;
    const float oz = -water.extentZ * 0.5f;

    // Gerstner 波の最大変位量（AABB Y 方向のマージンに使う）
    float maxAmp = 0.0f;
    for (const auto& w : water.waves) maxAmp += w.amplitude;
    const float yMargin = maxAmp + 0.5f;

    const uint32_t numChunks = std::max(water.chunkCount, 1u);

    // チャンクごとのセル数（端数は最終チャンクに収める）
    const uint32_t cellsPerChunkX = (water.resolutionX + numChunks - 1) / numChunks;
    const uint32_t cellsPerChunkZ = (water.resolutionZ + numChunks - 1) / numChunks;

    for (uint32_t cz = 0; cz < numChunks; ++cz) {
        for (uint32_t cx = 0; cx < numChunks; ++cx) {
            const uint32_t ixStart = cx * cellsPerChunkX;
            const uint32_t izStart = cz * cellsPerChunkZ;
            const uint32_t ixEnd = std::min(ixStart + cellsPerChunkX, water.resolutionX);
            const uint32_t izEnd = std::min(izStart + cellsPerChunkZ, water.resolutionZ);

            const uint32_t vertCols = ixEnd - ixStart + 1;
            const uint32_t vertRows = izEnd - izStart + 1;

            std::vector<WaterVertex> verts;
            verts.reserve(static_cast<size_t>(vertCols) * static_cast<size_t>(vertRows));

            for (uint32_t iz = izStart; iz <= izEnd; ++iz) {
                for (uint32_t ix = ixStart; ix <= ixEnd; ++ix) {
                    WaterVertex v;
                    v.position = { ox + static_cast<float>(ix) * dx, 0.0f, oz + static_cast<float>(iz) * dz };
                    v.uv = {
                        static_cast<float>(ix) / static_cast<float>(water.resolutionX),
                        static_cast<float>(iz) / static_cast<float>(water.resolutionZ)
                    };
                    verts.push_back(v);
                }
            }

            std::vector<uint32_t> indices;
            indices.reserve(static_cast<size_t>(vertCols - 1) * static_cast<size_t>(vertRows - 1) * 6u);
            for (uint32_t iz = 0; iz < vertRows - 1; ++iz) {
                for (uint32_t ix = 0; ix < vertCols - 1; ++ix) {
                    const uint32_t i00 = iz * vertCols + ix;
                    const uint32_t i10 = i00 + 1;
                    const uint32_t i01 = i00 + vertCols;
                    const uint32_t i11 = i01 + 1;
                    indices.insert(indices.end(), { i00, i01, i10, i10, i01, i11 });
                }
            }

            WaterChunk chunk;
            chunk.vertexBuffer = resources.CreateVertexBuffer(
                verts.data(), verts.size() * sizeof(WaterVertex), static_cast<uint32_t>(sizeof(WaterVertex)));
            chunk.indexBuffer = resources.CreateIndexBuffer(
                indices.data(), static_cast<uint32_t>(indices.size()));
            chunk.indexCount = static_cast<uint32_t>(indices.size());
            // ローカル空間 AABB（Y は Gerstner 波の最大変位分をマージンとして加える）
            chunk.aabbMin = { ox + static_cast<float>(ixStart) * dx, -yMargin, oz + static_cast<float>(izStart) * dz };
            chunk.aabbMax = { ox + static_cast<float>(ixEnd)   * dx,  yMargin, oz + static_cast<float>(izEnd)   * dz };
            mesh.chunks.push_back(std::move(chunk));
        }
    }

    mesh.aabbMin = { ox, -yMargin, oz };
    mesh.aabbMax = { -ox, yMargin, -oz };
}

renderer::ResourceHandle<renderer::TextureTag> BuildFoamMask(
    Scene& scene,
    const WaterComponent& water,
    const Transform& waterTransform,
    renderer::ResourceManager& resources)
{
    const uint32_t width = water.resolutionX + 1;
    const uint32_t height = water.resolutionZ + 1;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0u);

    TerrainComponent* terrain = nullptr;
    Transform* terrainTransform = nullptr;
    for (auto [tc, tf] : scene.View<TerrainComponent, Transform>()) {
        if (!tc.enabled || tc.heightData.empty()) continue;
        terrain = &tc;
        terrainTransform = &tf;
        break;
    }

    if (!terrain || !terrainTransform)
        return resources.CreateTexture(pixels.data(), width, height);

    const float dx = water.extentX / static_cast<float>(water.resolutionX);
    const float dz = water.extentZ / static_cast<float>(water.resolutionZ);
    const float ox = -water.extentX * 0.5f;
    const float oz = -water.extentZ * 0.5f;

    for (uint32_t iz = 0; iz < height; ++iz) {
        for (uint32_t ix = 0; ix < width; ++ix) {
            const float worldX = waterTransform.position.x + ox + static_cast<float>(ix) * dx;
            const float worldZ = waterTransform.position.z + oz + static_cast<float>(iz) * dz;
            const float localX = worldX - terrainTransform->position.x;
            const float localZ = worldZ - terrainTransform->position.z;
            const float terrainY = terrain->GetHeightAt(localX, localZ) + terrainTransform->position.y;
            const float heightDiff = waterTransform.position.y - terrainY;
            const float foam = ComputeFoamWeight(heightDiff, water.foamThreshold, water.foamFade);

            const size_t p = (static_cast<size_t>(iz) * width + ix) * 4u;
            pixels[p + 0] = static_cast<uint8_t>(math::Clamp01(foam) * 255.0f);
            pixels[p + 1] = pixels[p + 0];
            pixels[p + 2] = pixels[p + 0];
            pixels[p + 3] = 255u;
        }
    }

    return resources.CreateTexture(pixels.data(), width, height);
}

WaterTextures BuildTextureSet(
    const WaterComponent& water,
    const Transform& waterTransform,
    Scene& scene,
    renderer::ResourceManager& resources,
    renderer::ResourceHandle<renderer::TextureTag> flatNormal,
    renderer::ResourceHandle<renderer::TextureTag> white,
    renderer::ResourceHandle<renderer::TextureTag> black,
    renderer::ResourceHandle<renderer::TextureTag> neutralFlow)
{
    WaterTextures textures;
    textures.normalMap1 = water.normalMap1Path.empty() ? flatNormal : resources.LoadTexture(water.normalMap1Path);
    textures.normalMap2 = water.normalMap2Path.empty() ? flatNormal : resources.LoadTexture(water.normalMap2Path);
    textures.foamTex = water.foamTexPath.empty() ? white : resources.LoadTexture(water.foamTexPath);
    textures.foamMask = BuildFoamMask(scene, water, waterTransform, resources);
    textures.envTex = water.envCubemapPath.empty() ? black : resources.LoadTexture(water.envCubemapPath);
    textures.flowMap = (!water.enableFlowMap || water.flowMapPath.empty())
        ? neutralFlow
        : resources.LoadTexture(water.flowMapPath);
    return textures;
}

void UpdateRippleState(
    WaterRippleState& state,
    float dt,
    renderer::ResourceManager& resources)
{
    for (WaterRipple& ripple : state.ripples) {
        ripple.radius += ripple.speed * dt;
        ripple.amplitude *= std::exp(-ripple.decayRate * dt);
    }
    std::erase_if(state.ripples, [](const WaterRipple& ripple) {
        return ripple.amplitude < 0.001f || ripple.radius > 2.8f;
    });

    state.pixels.assign(static_cast<size_t>(state.width) * static_cast<size_t>(state.height) * 4u, 128u);
    for (uint32_t y = 0; y < state.height; ++y) {
        for (uint32_t x = 0; x < state.width; ++x) {
            const math::Vector2 uv = {
                (static_cast<float>(x) + 0.5f) / static_cast<float>(state.width),
                (static_cast<float>(y) + 0.5f) / static_cast<float>(state.height)
            };

            math::Vector2 offset = math::Vector2::ZERO;
            for (const WaterRipple& ripple : state.ripples) {
                const math::Vector2 d = uv - ripple.positionUV;
                const float dist = d.Length();
                if (dist <= math::EPSILON) continue;

                const float w = std::max(ripple.waveWidth, 0.001f);
                const math::Vector2 dir = d.Normalized();

                // WHY: 実際の水面は主リングに続いて複数の同心円が生まれる。
                //      ガウス包絡 × 正弦でリング形状を作り、副次リングを追加する。
                auto addRing = [&](float r, float scale) {
                    const float ring = dist - r;
                    const float g    = std::exp(-(ring * ring) / (w * w));
                    const float ph   = std::sin(ring / w * math::PI);
                    offset += dir * (g * ph * ripple.amplitude * scale);
                };

                addRing(ripple.radius,               1.00f);  // 主リング
                addRing(ripple.radius - w * 3.0f,    0.42f);  // 第2リング（主より内側）
                addRing(ripple.radius - w * 6.0f,    0.16f);  // 第3リング（さらに内側）
            }

            const size_t p = (static_cast<size_t>(y) * state.width + x) * 4u;
            state.pixels[p + 0] = static_cast<uint8_t>(math::Clamp01(0.5f + offset.x * 0.5f) * 255.0f);
            state.pixels[p + 1] = static_cast<uint8_t>(math::Clamp01(0.5f + offset.y * 0.5f) * 255.0f);
            state.pixels[p + 2] = 255u;
            state.pixels[p + 3] = 255u;
        }
    }

    if (state.gpuTex.IsValid())
        resources.Release(state.gpuTex);
    state.gpuTex = resources.CreateTexture(state.pixels.data(), state.width, state.height);
    state.dirty = false;
}

WaterCB BuildWaterCB(const WaterComponent& water, const Transform& transform, const renderer::Camera& camera, float time)
{
    WaterCB cb{};
    const math::Matrix4 world = transform.GetWorldMatrix();
    cb.worldMatrix = world;
    cb.wvpMatrix = camera.GetViewProjection() * world;
    cb.shallowColorDepth = { water.shallowColor.x, water.shallowColor.y, water.shallowColor.z, water.shallowDepth };
    cb.deepColorDepth = { water.deepColor.x, water.deepColor.y, water.deepColor.z, water.deepDepth };
    cb.surfaceParams = { water.opacity, water.reflectivity, water.fresnelBias, water.fresnelPower };
    cb.normalMap1Params = {
        water.normalMap1Scroll.x, water.normalMap1Scroll.y, water.normalMap1Tiling, water.normalStrength
    };
    cb.normalMap2Params = {
        water.normalMap2Scroll.x, water.normalMap2Scroll.y, water.normalMap2Tiling, time
    };
    cb.foamParams = { water.foamThreshold, water.foamFade, water.foamStrength, water.foamTiling };
    cb.refractionFlowParams = {
        water.refractionStrength, water.flowSpeed, water.flowTiling, water.enableFlowMap ? 1.0f : 0.0f
    };

    for (int i = 0; i < 4; ++i) {
        const GerstnerWave& wave = water.waves[static_cast<size_t>(i)];
        assert(wave.steepness <= 1.0f);

        const bool validWave =
            water.enableGerstnerWaves && wave.amplitude > 0.0001f && wave.wavelength > 0.0001f;
        const math::Vector2 dir = wave.direction.Normalized();
        const float k = validWave ? math::TWO_PI / wave.wavelength : 0.0f;
        const float omega = validWave ? std::sqrt(9.8f * k) : 0.0f;
        cb.waveDir[i] = { dir.x, dir.y, wave.steepness, validWave ? 1.0f : 0.0f };
        cb.waveParams[i] = { validWave ? wave.amplitude : 0.0f, wave.wavelength, omega, k };
    }

    return cb;
}

// WHAT: ローカル空間 AABB をワールド空間に変換し、視錐台との交差判定をする。
bool AabbVisible(const math::Frustum& frustum, const math::Vector3& tfPos,
                 const math::Vector3& localMin, const math::Vector3& localMax)
{
    const math::Vector3 center = {
        tfPos.x + (localMin.x + localMax.x) * 0.5f,
        tfPos.y + (localMin.y + localMax.y) * 0.5f,
        tfPos.z + (localMin.z + localMax.z) * 0.5f,
    };
    const math::Vector3 extents = {
        (localMax.x - localMin.x) * 0.5f,
        (localMax.y - localMin.y) * 0.5f,
        (localMax.z - localMin.z) * 0.5f,
    };
    return frustum.IntersectsAABB(center, extents);
}

} // namespace

void AddWaterRipple(
    EntityID waterEntity,
    math::Vector2 positionUV,
    float amplitude,
    float speed,
    float decayRate,
    float waveWidth)
{
    WaterRipple ripple;
    ripple.positionUV = {
        math::Clamp01(positionUV.x),
        math::Clamp01(positionUV.y)
    };
    ripple.amplitude = math::Clamp01(amplitude);
    ripple.speed     = std::max(speed, 0.0f);
    ripple.decayRate = std::max(decayRate, 0.0f);
    ripple.waveWidth = std::max(waveWidth, 0.005f);

    WaterRippleState& state = s_rippleStates[waterEntity.index];
    state.ripples.push_back(ripple);
    state.dirty = true;
}

void QueueWaterSplash(const math::Vector3& worldPos, float intensity)
{
    s_pendingSplashes.push_back({ worldPos, math::Clamp01(intensity) });
}

void WaterRenderSystem(
    Scene& scene,
    renderer::IRenderer& renderer,
    renderer::ResourceManager& resources,
    const renderer::Camera& camera,
    renderer::ResourceHandle<renderer::RenderTargetTag> outputRT,
    renderer::ResourceHandle<renderer::TextureTag> sceneColor,
    float elapsedTime,
    const renderer::RenderSettings* /*settings*/)
{
    static auto waterShader = resources.LoadShader("assets/shaders/Water/Water.hlsl");
    static auto waterPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    static auto cameraCBH = resources.CreateConstantBuffer(288);
    static auto waterCBH  = resources.CreateConstantBuffer(sizeof(WaterCB));
    static auto lightCBH  = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    // MaterialComponent 未設定時のデフォルトエフェクトパラメータ CB
    static auto defaultEffectCBH = [&] {
        WaterEffectParams defaults{};
        auto h = resources.CreateConstantBuffer(sizeof(WaterEffectParams));
        resources.Update(h, &defaults, sizeof(WaterEffectParams));
        return h;
    }();

    static auto flatNormalTex = [&] {
        const uint8_t n[4] = { 128, 128, 255, 255 };
        return resources.CreateTexture(n, 1, 1);
    }();
    static auto whiteTex = [&] {
        const uint8_t w[4] = { 255, 255, 255, 255 };
        return resources.CreateTexture(w, 1, 1);
    }();
    static auto blackTex = [&] {
        const uint8_t b[4] = { 0, 0, 0, 255 };
        return resources.CreateTexture(b, 1, 1);
    }();
    static auto neutralFlowTex = [&] {
        const uint8_t f[4] = { 128, 128, 0, 255 };
        return resources.CreateTexture(f, 1, 1);
    }();
    static auto neutralRippleTex = [&] {
        const uint8_t r[4] = { 128, 128, 255, 255 };
        return resources.CreateTexture(r, 1, 1);
    }();

    if (outputRT.IsValid())
        renderer.SetRenderTarget(outputRT, resources);

    // ── スプラッシュ GO 生成（前フレームのキューを消費） ───────────────────────
    // WHY: PhysicsSystem/IKSystem は Scene 参照を持たないためキュー経由で委譲する。
    //      ParticleEmitter はシーンの既存 ParticlePass がそのまま描画する。
    for (const SplashEvent& ev : s_pendingSplashes) {
        auto& go = scene.CreateGameObject("__WaterSplash");
        go.transform.localPosition = ev.worldPos;

        ParticleEmitter emitter;
        emitter.enabled      = true;
        emitter.emitRate     = 0.0f;   // 連続排出なし（バースト済み粒子のみ）
        emitter.lifetime     = 1.2f;
        emitter.sizeStart    = 0.10f + ev.intensity * 0.20f;
        emitter.sizeEnd      = 0.0f;
        emitter.colorStart   = { 0.75f, 0.93f, 1.0f, 0.9f };
        emitter.colorEnd     = { 0.55f, 0.80f, 1.0f, 0.0f };
        emitter.maxParticles = 24;

        const int count = static_cast<int>(6.0f + ev.intensity * 14.0f);
        emitter.particles.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            Particle p;
            p.position = ev.worldPos;
            const float angle =
                (static_cast<float>(i) / static_cast<float>(count)) * math::TWO_PI
                + (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX) - 0.5f) * 0.8f;
            const float hSpeed = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)
                                 * ev.intensity * 2.0f + 0.2f;
            const float vSpeed = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)
                                 * ev.intensity * 5.0f + 1.5f;
            p.velocity = { std::cos(angle) * hSpeed, vSpeed, std::sin(angle) * hSpeed };
            p.color    = emitter.colorStart;
            p.size     = emitter.sizeStart;
            p.age      = 0.0f;
            emitter.particles.push_back(std::move(p));
        }

        const EntityID id = go.GetID();
        go.AddComponent<ParticleEmitter>(std::move(emitter));
        s_splashGos.push_back(id);
    }
    s_pendingSplashes.clear();

    // 全パーティクルが寿命切れになった splash GO を遅延破棄する
    auto splashIt = s_splashGos.begin();
    while (splashIt != s_splashGos.end()) {
        auto* em = scene.GetComponent<ParticleEmitter>(*splashIt);
        if (!em || em->particles.empty()) {
            scene.DestroyGameObject(*splashIt);
            splashIt = s_splashGos.erase(splashIt);
        } else {
            ++splashIt;
        }
    }

    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::CLAMP_LINEAR);
    renderer.SetSampler(2, renderer::SamplerMode::CLAMP_LINEAR);

    {
        struct CameraCB {
            math::Matrix4 view;
            math::Matrix4 projection;
            math::Matrix4 viewProjection;
            math::Matrix4 invViewProjection;
            math::Vector3 cameraPos;
            float nearZ;
            float farZ;
            float _pad[3];
        };
        static_assert(sizeof(CameraCB) == 288, "CameraCB size mismatch");

        const math::Matrix4 vp = camera.GetViewProjection();
        CameraCB camData{};
        camData.view = camera.GetViewMatrix();
        camData.projection = camera.GetProjectionMatrix();
        camData.viewProjection = vp;
        camData.invViewProjection = math::Matrix4::Inverse(vp);
        camData.cameraPos = camera.m_position;
        camData.nearZ = camera.m_near;
        camData.farZ = camera.m_far;
        resources.Update(cameraCBH, &camData, sizeof(camData));
    }

    {
        constexpr float kDegToRad = math::PI / 180.0f;
        renderer::LightConstantsCB lightData{};
        lightData.lightDir = { 0.0f, -1.0f, 0.5f };
        lightData.lightColor = { 1.0f, 1.0f, 1.0f };
        lightData.lightIntensity = 1.0f;

        for (auto [tf, lc] : scene.View<Transform, LightComponent>()) {
            if (!lc.enabled) continue;
            if (lc.type == LightComponent::Type::Directional) {
                lightData.lightDir = tf.Forward().Normalized();
                lightData.lightColor = lc.color;
                lightData.lightIntensity = lc.intensity;
            } else if (lc.type == LightComponent::Type::Point && lightData.pointLightCount < 8) {
                auto& pl = lightData.pointLights[lightData.pointLightCount++];
                pl.position = tf.position; pl.range = lc.range;
                pl.color = lc.color; pl.intensity = lc.intensity;
            } else if (lc.type == LightComponent::Type::Spot && lightData.spotLightCount < 4) {
                auto& sl = lightData.spotLights[lightData.spotLightCount++];
                sl.position = tf.position;
                sl.direction = tf.Forward().Normalized();
                sl.range = lc.range;
                sl.innerCos = std::cos(lc.innerCone * kDegToRad);
                sl.outerCos = std::cos(lc.outerCone * kDegToRad);
                sl.color = lc.color;
                sl.intensity = lc.intensity;
            }
        }
        resources.Update(lightCBH, &lightData, sizeof(lightData));
    }

    const math::Frustum frustum = math::Frustum::FromViewProjection(camera.GetViewProjection());
    static float s_lastElapsedTime = elapsedTime;
    const float dt = math::Clamp(elapsedTime - s_lastElapsedTime, 0.0f, 0.1f);
    s_lastElapsedTime = elapsedTime;

    for (auto [water, transform] : scene.View<WaterComponent, Transform>()) {
        if (!water.enabled) continue;

        EntityID eid = FindEntityForWater(scene, water);
        if (!scene.IsValid(eid)) continue;

        if (water.meshDirty || !s_meshCache.contains(eid.index)) {
            // BuildWaterMesh 内で既存チャンクの GPU リソースを解放してから再構築する
            BuildWaterMesh(water, s_meshCache[eid.index], resources);
            water.meshDirty = false;
            water.foamDirty = true;
        }

        if (water.texDirty || water.foamDirty || !s_texCache.contains(eid.index)) {
            s_texCache[eid.index] = BuildTextureSet(
                water, transform, scene, resources, flatNormalTex, whiteTex, blackTex, neutralFlowTex);
            water.texDirty = false;
            water.foamDirty = false;
        }

        WaterRippleState& rippleState = s_rippleStates[eid.index];
        if (rippleState.dirty || !rippleState.gpuTex.IsValid() || !rippleState.ripples.empty())
            UpdateRippleState(rippleState, dt, resources);

        const WaterMesh& mesh = s_meshCache.at(eid.index);
        // 全体 AABB で早期リジェクト: 水面全体が視野外なら全チャンクをスキップ
        if (!AabbVisible(frustum, transform.position, mesh.aabbMin, mesh.aabbMax))
            continue;

        const WaterCB cb = BuildWaterCB(water, transform, camera, elapsedTime);
        resources.Update(waterCBH, &cb, sizeof(cb));

        // ── ユーザー定義エフェクトパラメータ (b2 = MaterialConstants) ────────────
        // MaterialComponent がなければ WaterEffectParams のデフォルト値 CB を使う。
        auto effectCBH = defaultEffectCBH;
        if (auto* go = scene.GetGameObject(eid)) {
            if (auto* mc = go->GetComponent<MaterialComponent>();
                mc && mc->enabled && !mc->shaderPath.empty())
            {
                if (!mc->material)
                    mc->material = std::make_shared<renderer::Material>();
                auto& mat = *mc->material;
                mat.shader = resources.LoadShader(mc->shaderPath);
                if (const auto* sh = resources.Get(mat.shader)) {
                    const auto& desc = sh->GetDescriptor();
                    if (mc->paramData.size() != desc.cbufferSize)
                        mc->InitFromDescriptor(desc);
                    if (!mat.paramsBuffer.IsValid())
                        mat.Init(resources, desc.cbufferSize);
                    mat.paramData = mc->paramData;
                    mat.Upload(resources, desc);
                    effectCBH = mat.paramsBuffer;
                }
            }
        }

        const WaterTextures& textures = s_texCache.at(eid.index);
        const renderer::ResourceHandle<renderer::TextureTag> depthTex =
            outputRT.IsValid() ? resources.GetDepthTexture(outputRT) : renderer::ResourceHandle<renderer::TextureTag>{};
        const renderer::ResourceHandle<renderer::TextureTag> colorTex =
            sceneColor.IsValid() ? sceneColor : blackTex;
        const renderer::ResourceHandle<renderer::TextureTag> rippleTex =
            rippleState.gpuTex.IsValid() ? rippleState.gpuTex : neutralRippleTex;

        // チャンク単位でフラスタムカリング → DrawCall 発行
        // WHY: 全体 AABB で弾けなかった場合でも、視野外のチャンクは個別に除外できる。
        for (const WaterChunk& chunk : mesh.chunks) {
            if (!AabbVisible(frustum, transform.position, chunk.aabbMin, chunk.aabbMax))
                continue;

            renderer::DrawCall call;
            call.vertexBuffer = chunk.vertexBuffer;
            call.indexBuffer  = chunk.indexBuffer;
            call.shader       = waterShader;
            call.pipelineState = waterPSO;
            call.indexCount   = chunk.indexCount;
            call.layer        = renderer::RenderLayer::TRANSPARENT;
            call.topology     = renderer::PrimitiveTopology::TRIANGLE_LIST;
            call.constantBuffers[0] = cameraCBH;
            call.constantBuffers[1] = waterCBH;
            call.constantBuffers[2] = effectCBH;   // MaterialConstants (ユーザー定義)
            call.constantBuffers[3] = lightCBH;
            call.textures[0] = textures.normalMap1;
            call.textures[1] = textures.normalMap2;
            call.textures[2] = textures.foamTex;
            call.textures[3] = textures.foamMask;
            call.textures[4] = textures.envTex;
            call.textures[5] = depthTex;
            call.textures[6] = colorTex;
            call.textures[7] = textures.flowMap;
            call.textures[8] = rippleTex;
            renderer.Submit(call, resources);
        }
    }
}

} // namespace fbzz::scene
