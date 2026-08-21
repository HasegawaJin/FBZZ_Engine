// FBZZ Engine
// RenderPasses/Geometry/WaterRenderPass.cpp | fbzz::scene
// WaterComponent → GPU 水面メッシュ・泡マスク・波紋テクスチャ生成と描画 (IRenderPass 実装)
//
// WHY: 水面は透明描画、Terrain 高さ参照、動的 CPU テクスチャ更新をまとめて扱う。
//      Component に GPU リソースを持たせず System 側の static cache に閉じることで、
//      Scene データは保存しやすい純粋なパラメータのまま保つ。
#include "Engine/Scene/Systems/RenderPasses/Geometry/WaterRenderPass.hpp"
#include "GeometryPasses.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/ParticleEmitter.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include "Engine/Scene/Components/WaterComponent.hpp"
#include "Engine/Renderer/IShader.hpp"
#include "Engine/Renderer/Material.hpp"
#include "Engine/Renderer/Camera.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/RenderSettings.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Engine/Renderer/SamplerMode.hpp"
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Math/Frustum.hpp>
#include <Math/Matrix4.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector4.hpp>
#include <Physics/Layer.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::scene {

namespace {

// WaterVertex — HLSL の WaterVSInput と一致する頂点レイアウト。
struct WaterVertex {
    math::Vector3 position;
    math::Vector2 uv;
};

// WaterChunk — チャンク 1 個分の GPU リソースと AABB。
struct WaterChunk {
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer;
    renderer::ResourceHandle<renderer::BufferTag> indexBuffer;
    uint32_t indexCount = 0;
    math::Vector3 aabbMin;
    math::Vector3 aabbMax;
};

struct WaterMesh {
    std::vector<WaterChunk> chunks;
    math::Vector3 aabbMin;
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
    float waveWidth = 0.038f;
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
struct WaterCB {
    math::Matrix4 worldMatrix;
    math::Matrix4 wvpMatrix;
    math::Vector4 shallowColorDepth;
    math::Vector4 deepColorDepth;
    math::Vector4 surfaceParams;
    math::Vector4 normalMap1Params;
    math::Vector4 normalMap2Params;
    math::Vector4 foamParams;
    math::Vector4 refractionFlowParams;
    math::Vector4 waveDir[4];
    math::Vector4 waveParams[4];
};

// WaterEffectParams — MaterialConstants cbuffer (b2) の C++ ミラー。
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

void BuildWaterMesh(const WaterComponent& water, WaterMesh& mesh, renderer::ResourceManager& resources)
{
    assert(water.resolutionX > 0);
    assert(water.resolutionZ > 0);

    for (auto& chunk : mesh.chunks) {
        if (chunk.vertexBuffer.IsValid()) resources.Release(chunk.vertexBuffer);
        if (chunk.indexBuffer.IsValid()) resources.Release(chunk.indexBuffer);
    }
    mesh.chunks.clear();

    const float dx = water.extentX / static_cast<float>(water.resolutionX);
    const float dz = water.extentZ / static_cast<float>(water.resolutionZ);
    const float ox = -water.extentX * 0.5f;
    const float oz = -water.extentZ * 0.5f;

    float maxAmp = 0.0f;
    for (const auto& w : water.waves) maxAmp += w.amplitude;
    const float yMargin = maxAmp + 0.5f;

    const uint32_t numChunks = (std::max)(water.chunkCount, 1u);
    const uint32_t cellsPerChunkX = (water.resolutionX + numChunks - 1) / numChunks;
    const uint32_t cellsPerChunkZ = (water.resolutionZ + numChunks - 1) / numChunks;

    for (uint32_t cz = 0; cz < numChunks; ++cz) {
        for (uint32_t cx = 0; cx < numChunks; ++cx) {
            const uint32_t ixStart = cx * cellsPerChunkX;
            const uint32_t izStart = cz * cellsPerChunkZ;
            const uint32_t ixEnd = (std::min)(ixStart + cellsPerChunkX, water.resolutionX);
            const uint32_t izEnd = (std::min)(izStart + cellsPerChunkZ, water.resolutionZ);

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
    float foamThreshold,
    float foamFade,
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
            const float foam = ComputeFoamWeight(heightDiff, foamThreshold, foamFade);

            const size_t p = (static_cast<size_t>(iz) * width + ix) * 4u;
            pixels[p + 0] = static_cast<uint8_t>(math::Clamp01(foam) * 255.0f);
            pixels[p + 1] = pixels[p + 0];
            pixels[p + 2] = pixels[p + 0];
            pixels[p + 3] = 255u;
        }
    }

    return resources.CreateTexture(pixels.data(), width, height);
}

inline float WGetF(const asset::MaterialAsset* m, const char* name, float def)
{
    if (!m) return def;
    auto it = m->params.find(name);
    if (it != m->params.end() && !it->second.empty()) return it->second[0];
    return def;
}
inline math::Vector2 WGetF2(const asset::MaterialAsset* m, const char* name, math::Vector2 def)
{
    if (!m) return def;
    auto it = m->params.find(name);
    if (it != m->params.end() && it->second.size() >= 2)
        return { it->second[0], it->second[1] };
    return def;
}
inline math::Vector3 WGetF3(const asset::MaterialAsset* m, const char* name, math::Vector3 def)
{
    if (!m) return def;
    auto it = m->params.find(name);
    if (it != m->params.end() && it->second.size() >= 3)
        return { it->second[0], it->second[1], it->second[2] };
    return def;
}
inline std::string WGetTex(const asset::MaterialAsset* m, const char* name)
{
    if (!m) return {};
    auto it = m->textures.find(name);
    if (it != m->textures.end()) return it->second;
    return {};
}

renderer::ResourceHandle<renderer::TextureTag> GetProceduralRiverFlowMap(renderer::ResourceManager& resources)
{
    // WHAT: 外部 flowMap が未設定でも River プリセットが動くよう、川方向の簡易 FlowMap を生成する。
    // WHY: アーティスト製フローマップが揃う前の段階でも Phase D の水流シェーダー挙動を確認できる。
    static renderer::ResourceHandle<renderer::TextureTag> s_flowMap;
    static uint64_t s_resetVersion = 0;
    if (s_flowMap.IsValid() && s_resetVersion == resources.GetResetVersion())
        return s_flowMap;

    constexpr uint32_t size = 128;
    std::vector<uint8_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size) * 4u, 255u);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
            const float meander = std::sin(v * math::TWO_PI * 3.0f + std::sin(u * math::TWO_PI * 2.0f) * 0.7f);
            const math::Vector2 flow = math::Vector2(0.92f, meander * 0.26f).Normalized();
            const size_t p = (static_cast<size_t>(y) * size + x) * 4u;
            pixels[p + 0] = static_cast<uint8_t>(math::Clamp01(flow.x * 0.5f + 0.5f) * 255.0f);
            pixels[p + 1] = static_cast<uint8_t>(math::Clamp01(flow.y * 0.5f + 0.5f) * 255.0f);
            pixels[p + 2] = 0u;
            pixels[p + 3] = 255u;
        }
    }

    s_flowMap = resources.CreateTexture(pixels.data(), size, size);
    s_resetVersion = resources.GetResetVersion();
    return s_flowMap;
}

WaterTextures BuildTextureSet(
    const asset::MaterialAsset* mat,
    const WaterComponent& water,
    const Transform& waterTransform,
    Scene& scene,
    renderer::ResourceManager& resources,
    float foamThreshold,
    float foamFade,
    renderer::ResourceHandle<renderer::TextureTag> flatNormal,
    renderer::ResourceHandle<renderer::TextureTag> white,
    renderer::ResourceHandle<renderer::TextureTag> black,
    renderer::ResourceHandle<renderer::TextureTag> neutralFlow)
{
    const std::string normalMap1Path = WGetTex(mat, "normalMap1");
    const std::string normalMap2Path = WGetTex(mat, "normalMap2");
    const std::string foamTexPath    = WGetTex(mat, "foamTex");
    const std::string envCubemapPath = WGetTex(mat, "envCubemap");
    const std::string flowMapPath    = WGetTex(mat, "flowMap");
    const bool enableFlow = WGetF(mat, "enableFlowMap", 0.0f) > 0.5f;

    WaterTextures textures;
    textures.normalMap1 = normalMap1Path.empty() ? flatNormal : resources.LoadTexture(normalMap1Path);
    textures.normalMap2 = normalMap2Path.empty() ? flatNormal : resources.LoadTexture(normalMap2Path);
    textures.foamTex    = foamTexPath.empty()    ? white      : resources.LoadTexture(foamTexPath);
    textures.foamMask   = BuildFoamMask(scene, water, waterTransform, foamThreshold, foamFade, resources);
    textures.envTex     = envCubemapPath.empty() ? black      : resources.LoadTexture(envCubemapPath);
    textures.flowMap    = !enableFlow
        ? neutralFlow
        : (flowMapPath.empty() ? GetProceduralRiverFlowMap(resources) : resources.LoadTexture(flowMapPath));
    return textures;
}

void UpdateRippleState(WaterRippleState& state, float dt, renderer::ResourceManager& resources)
{
    for (WaterRipple& ripple : state.ripples) {
        ripple.radius += ripple.speed * dt;
        ripple.amplitude *= std::exp(-ripple.decayRate * dt);
    }
    std::erase_if(state.ripples, [](const WaterRipple& ripple) {
        return ripple.amplitude < 0.001f || ripple.radius > 2.8f;
    });

    // WHY: 波紋がなく既存テクスチャも有効な場合は CPU 更新・GPU アップロードをスキップする。
    if (state.ripples.empty() && state.gpuTex.IsValid() && !state.dirty)
        return;

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

                const float w = (std::max)(ripple.waveWidth, 0.001f);
                const math::Vector2 dir = d.Normalized();

                auto addRing = [&](float r, float scale) {
                    const float ring = dist - r;
                    const float g    = std::exp(-(ring * ring) / (w * w));
                    const float ph   = std::sin(ring / w * math::PI);
                    offset += dir * (g * ph * ripple.amplitude * scale);
                };

                addRing(ripple.radius,               1.00f);
                addRing(ripple.radius - w * 3.0f,    0.42f);
                addRing(ripple.radius - w * 6.0f,    0.16f);
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

WaterCB BuildWaterCB(const WaterComponent& water, const asset::MaterialAsset* mat,
                     const Transform& transform, const renderer::Camera& camera, float time)
{
    WaterCB cb{};
    const math::Matrix4 world = transform.GetWorldMatrix();
    cb.worldMatrix = world;
    cb.wvpMatrix = camera.GetViewProjection() * world;

    const auto shallowColor = WGetF3(mat, "shallowColor", { 0.20f, 0.60f, 0.70f });
    const auto deepColor    = WGetF3(mat, "deepColor",    { 0.00f, 0.10f, 0.30f });
    cb.shallowColorDepth = { shallowColor.x, shallowColor.y, shallowColor.z,
                              WGetF(mat, "shallowDepth", 0.5f) };
    cb.deepColorDepth    = { deepColor.x, deepColor.y, deepColor.z,
                              WGetF(mat, "deepDepth", 5.0f) };
    cb.surfaceParams = {
        WGetF(mat, "opacity",       0.85f),
        WGetF(mat, "reflectivity",  0.5f),
        WGetF(mat, "fresnelBias",   0.02f),
        WGetF(mat, "fresnelPower",  5.0f)
    };
    const auto scroll1 = WGetF2(mat, "normalMap1Scroll", { 0.02f,  0.01f });
    const auto scroll2 = WGetF2(mat, "normalMap2Scroll", { -0.01f, 0.02f });
    cb.normalMap1Params = { scroll1.x, scroll1.y, WGetF(mat, "normalMap1Tiling", 4.0f), WGetF(mat, "normalStrength", 1.0f) };
    cb.normalMap2Params = { scroll2.x, scroll2.y, WGetF(mat, "normalMap2Tiling", 6.0f), time };
    cb.foamParams = {
        WGetF(mat, "foamThreshold",     0.3f),
        WGetF(mat, "foamFade",          0.5f),
        WGetF(mat, "foamStrength",      1.0f),
        WGetF(mat, "foamTiling",        8.0f)
    };
    cb.refractionFlowParams = {
        WGetF(mat, "refractionStrength", 0.03f),
        WGetF(mat, "flowSpeed",          0.3f),
        WGetF(mat, "flowTiling",         1.0f),
        WGetF(mat, "enableFlowMap",      0.0f)
    };

    for (int i = 0; i < 4; ++i) {
        const GerstnerWave& wave = water.waves[static_cast<size_t>(i)];
        assert(wave.steepness <= 1.0f);

        const bool validWave =
            water.enableGerstnerWaves && wave.amplitude > 0.0001f && wave.wavelength > 0.0001f;
        const math::Vector2 dir = wave.direction.Normalized();
        const float k = validWave ? math::TWO_PI / wave.wavelength : 0.0f;
        const float omega = validWave ? std::sqrt(9.8f * k) : 0.0f;
        const float steepness = (std::min)(wave.steepness, 1.0f);
        cb.waveDir[i] = { dir.x, dir.y, steepness, validWave ? 1.0f : 0.0f };
        cb.waveParams[i] = { validWave ? wave.amplitude : 0.0f, wave.wavelength, omega, k };
    }

    return cb;
}

WaterEffectParams BuildWaterEffectParams(const asset::MaterialAsset* mat)
{
    WaterEffectParams params{};
    params.rimGlowStrength    = WGetF(mat, "rimGlowStrength",    params.rimGlowStrength);
    params.minShallowAlpha    = WGetF(mat, "minShallowAlpha",    params.minShallowAlpha);
    params.specularStrength   = WGetF(mat, "specularStrength",   params.specularStrength);
    params.specularExponent   = WGetF(mat, "specularExponent",   params.specularExponent);
    const math::Vector3 skyTint = WGetF3(mat, "skyReflectTint", {
        params.skyReflectTint[0], params.skyReflectTint[1], params.skyReflectTint[2]
    });
    params.skyReflectTint[0] = skyTint.x;
    params.skyReflectTint[1] = skyTint.y;
    params.skyReflectTint[2] = skyTint.z;
    params.envMapBlend = WGetF(mat, "envMapBlend", params.envMapBlend);
    const math::Vector3 rippleColor = WGetF3(mat, "rippleRingColor", {
        params.rippleRingColor[0], params.rippleRingColor[1], params.rippleRingColor[2]
    });
    params.rippleRingColor[0] = rippleColor.x;
    params.rippleRingColor[1] = rippleColor.y;
    params.rippleRingColor[2] = rippleColor.z;
    params.rippleRingStrength = WGetF(mat, "rippleRingStrength", params.rippleRingStrength);
    return params;
}

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
    ripple.speed     = (std::max)(speed, 0.0f);
    ripple.decayRate = (std::max)(decayRate, 0.0f);
    ripple.waveWidth = (std::max)(waveWidth, 0.005f);

    WaterRippleState& state = s_rippleStates[waterEntity.index];
    state.ripples.push_back(ripple);
    state.dirty = true;
}

void QueueWaterSplash(const math::Vector3& worldPos, float intensity)
{
    s_pendingSplashes.push_back({ worldPos, math::Clamp01(intensity) });
}

void WaterSelectionMaskSystem(RenderPassContext& ctx)
{
    if (!ctx.selectionOutlineEnabled) return;
    auto& h = ctx.handles;
    if (!h.selectionMaskShader.IsValid() || !h.selectionMaskPSO.IsValid()) return;

    for (auto [water, transform] : ctx.scene.View<WaterComponent, Transform>()) {
        EntityID eid = FindEntityForWater(ctx.scene, water);
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

        auto it = s_meshCache.find(eid.index);
        if (it == s_meshCache.end()) continue;

    const math::Matrix4 world = transform.GetWorldMatrix();
        PerObjectCB objData{};
        objData.world             = world;
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(world);
        ctx.resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));

        for (const WaterChunk& chunk : it->second.chunks) {
            renderer::DrawCall dc;
            dc.vertexBuffer       = chunk.vertexBuffer;
            dc.indexBuffer        = chunk.indexBuffer;
            dc.indexCount         = chunk.indexCount;
            dc.shader             = h.selectionMaskShader;
            dc.pipelineState      = h.selectionMaskPSO;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            ctx.renderer.Submit(dc, ctx.resources);
        }
    }
}

// ─── IRenderPass ──────────────────────────────────────────────────────────────

std::string_view WaterRenderPass::Name() const { return "WaterForward"; }

std::vector<renderer::RenderGraph::ResourceAccess> WaterRenderPass::DeclareAccesses(
    const RenderPassContext&) const
{
    return {
        { "HDR",       renderer::RenderGraph::ResourceUsage::ReadWrite },
        { "ShadowMap", renderer::RenderGraph::ResourceUsage::Read }
    };
}

void WaterRenderPass::Execute(RenderPassContext& ctx)
{
    Scene& scene = ctx.scene;
    renderer::IRenderer& renderer = ctx.renderer;
    renderer::ResourceManager& resources = ctx.resources;
    const renderer::Camera& camera = ctx.camera;
    const renderer::RenderSettings* settings = &ctx.settings;
    const auto lightCB = ctx.handles.lightCB;
    const auto shadowDepthTexture = resources.GetDepthTexture(ctx.handles.shadowMapRT);
    const auto shadowCB = ctx.handles.shadowCB;
    const float elapsedTime = Time::time;

    // WHY: static ローカルは初回のみ初期化される。ResourceManager::Reset() で世代が変わった
    //      場合だけ再生成し、旧ハンドル（失効済み）へのアクセスを防ぐ。
    static uint64_t s_resetVersion = resources.GetResetVersion();
    static auto waterShader = resources.LoadShader("Assets/Shaders/Water/Water.hlsl");
    static auto waterPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    static auto waterWireframePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::WIREFRAME,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    static auto cameraCBH = resources.CreateConstantBuffer(288);
    static auto waterCBH  = resources.CreateConstantBuffer(sizeof(WaterCB));
    static auto defaultEffectCBH = [&] {
        WaterEffectParams defaults{};
        auto h = resources.CreateConstantBuffer(sizeof(WaterEffectParams));
        resources.Update(h, &defaults, sizeof(WaterEffectParams));
        return h;
    }();
    static auto waterEffectCBH = resources.CreateConstantBuffer(sizeof(WaterEffectParams));
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

    // Scene color copy for water refraction (formerly a separate WaterSceneColorCopy pass).
    // WHY: Water shader reads HDR as scene color for refraction; we must copy it before
    //      binding hdrRT as the output RT, since DX11 prohibits simultaneous read/write.
    static auto copyColorShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
    static auto depthCopyShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
    static renderer::ResourceHandle<renderer::RenderTargetTag> s_sceneColorRT;
    static renderer::ResourceHandle<renderer::RenderTargetTag> s_sceneDepthRT;
    static uint32_t s_sceneColorW = 0, s_sceneColorH = 0;

    if (s_resetVersion != resources.GetResetVersion()) {
        s_resetVersion    = resources.GetResetVersion();
        waterShader       = resources.LoadShader("Assets/Shaders/Water/Water.hlsl");
        waterPSO          = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        waterWireframePSO = resources.CreatePipelineState({ renderer::RasterizerMode::WIREFRAME,    renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        cameraCBH         = resources.CreateConstantBuffer(288);
        waterCBH          = resources.CreateConstantBuffer(sizeof(WaterCB));
        defaultEffectCBH  = [&] { WaterEffectParams d{}; auto h = resources.CreateConstantBuffer(sizeof(WaterEffectParams)); resources.Update(h, &d, sizeof(WaterEffectParams)); return h; }();
        waterEffectCBH    = resources.CreateConstantBuffer(sizeof(WaterEffectParams));
        flatNormalTex     = [&] { const uint8_t n[4] = { 128, 128, 255, 255 }; return resources.CreateTexture(n, 1, 1); }();
        whiteTex          = [&] { const uint8_t w[4] = { 255, 255, 255, 255 }; return resources.CreateTexture(w, 1, 1); }();
        blackTex          = [&] { const uint8_t b[4] = {   0,   0,   0, 255 }; return resources.CreateTexture(b, 1, 1); }();
        neutralFlowTex    = [&] { const uint8_t f[4] = { 128, 128,   0, 255 }; return resources.CreateTexture(f, 1, 1); }();
        neutralRippleTex  = [&] { const uint8_t r[4] = { 128, 128, 255, 255 }; return resources.CreateTexture(r, 1, 1); }();
        copyColorShader   = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
        depthCopyShader   = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
        // WHY: IsValid() は id != 0 のみ確認し Reset 後の失効を検出しない。
        //      W/H をゼロにして次の解像度チェックで強制的に RT を再生成させる。
        s_sceneColorW = 0;
        s_sceneColorH = 0;
    }
    // 無効になったエンティティのキャッシュを解放する
    {
        std::unordered_set<uint32_t> validIndices;
        for (EntityID eid : scene.GetEntities<WaterComponent>())
            validIndices.insert(eid.index);

        std::erase_if(s_meshCache, [&validIndices, &resources](auto& kv) {
            if (validIndices.count(kv.first)) return false;
            for (auto& chunk : kv.second.chunks) {
                resources.Release(chunk.vertexBuffer);
                resources.Release(chunk.indexBuffer);
            }
            return true;
        });
        std::erase_if(s_texCache, [&validIndices](auto& kv) {
            return !validIndices.count(kv.first);
        });
        std::erase_if(s_rippleStates, [&validIndices, &resources](auto& kv) {
            if (validIndices.count(kv.first)) return false;
            if (kv.second.gpuTex.IsValid()) resources.Release(kv.second.gpuTex);
            return true;
        });
    }

    const math::Frustum frustum = math::Frustum::FromViewProjection(camera.GetViewProjection());
    bool hasVisibleWater = false;
    for (auto [water, transform] : scene.View<WaterComponent, Transform>()) {
        if (!water.enabled) continue;
        EntityID eid = FindEntityForWater(scene, water);
        if (!scene.IsValid(eid)) continue;

        const auto* go = scene.GetGameObject(eid);
        if (!go || !go->activeInHierarchy()) continue;
        if (!fbzz::Layer::Contains(ctx.cullingMask, go->layer)) continue;

        float maxAmp = 0.0f;
        for (const auto& wave : water.waves) maxAmp += wave.amplitude;
        const float yMargin = maxAmp + 0.5f;
        const math::Vector3 waterMin = { -water.extentX * 0.5f, -yMargin, -water.extentZ * 0.5f };
        const math::Vector3 waterMax = {  water.extentX * 0.5f,  yMargin,  water.extentZ * 0.5f };
        if (AabbVisible(frustum, transform.position, waterMin, waterMax)) {
            hasVisibleWater = true;
            break;
        }
    }
    if (!hasVisibleWater) {
        s_pendingSplashes.clear();
        return;
    }

    if (ctx.width != s_sceneColorW || ctx.height != s_sceneColorH
        || !s_sceneColorRT.IsValid() || !s_sceneDepthRT.IsValid()) {
        if (s_sceneColorRT.IsValid()) resources.Release(s_sceneColorRT);
        if (s_sceneDepthRT.IsValid()) resources.Release(s_sceneDepthRT);
        s_sceneColorRT = resources.CreateRenderTarget(ctx.width, ctx.height, 1);
        s_sceneDepthRT = resources.CreateRenderTarget(ctx.width, ctx.height, 0);
        s_sceneColorW = ctx.width;
        s_sceneColorH = ctx.height;
    }
    renderer.SetRenderTarget(s_sceneColorRT, resources);
    renderer.SetSampler(0, renderer::SamplerMode::CLAMP_LINEAR);
    if (copyColorShader.IsValid()) {
        renderer::DrawCall copyDC;
        copyDC.shader = copyColorShader;
        copyDC.pipelineState = ctx.handles.postprocPSO;
        copyDC.vertexCount = 3;
        copyDC.textures[5] = resources.GetColorTexture(ctx.handles.hdrRT, 0);
        renderer.Submit(copyDC, resources);
    }
    const auto sceneColor = resources.GetColorTexture(s_sceneColorRT, 0);

    // WHAT: HDR の depth を Water 専用の深度 RT へコピーし、PS ではその SRV を読む。
    // WHY: hdrRT を RTV/DSV として Water 描画に使いながら同じ depth を SRV(t5) で読むと
    //      DX11 の read/write 競合で SRV が解除され、背景判定・水深・泡が破綻する。
    renderer.SetRenderTarget(s_sceneDepthRT, resources);
    renderer.ClearDepth();
    if (depthCopyShader.IsValid()) {
        renderer::DrawCall depthDC;
        depthDC.shader = depthCopyShader;
        depthDC.pipelineState = ctx.handles.defaultPSO;
        depthDC.vertexCount = 3;
        depthDC.textures[7] = resources.GetDepthTexture(ctx.handles.hdrRT);
        renderer.Submit(depthDC, resources);
    }
    const auto sceneDepth = resources.GetDepthTexture(s_sceneDepthRT);

    renderer.SetRenderTarget(ctx.handles.hdrRT, resources);

    // スプラッシュ GO 生成（前フレームのキューを消費）
    for (const SplashEvent& ev : s_pendingSplashes) {
        auto& go = scene.CreateGameObject("__WaterSplash");
        go.transform.position = ev.worldPos;

        ParticleEmitter emitter;
        emitter.enabled      = true;
        emitter.emitRate     = 0.0f;
        emitter.lifetime     = 1.2f;
        emitter.sizeStart    = 0.10f + ev.intensity * 0.20f;
        emitter.sizeEnd      = 0.0f;
        emitter.colorStart   = { 0.75f, 0.93f, 1.0f, 0.9f };
        emitter.colorEnd     = { 0.55f, 0.80f, 1.0f, 0.0f };
        emitter.maxParticles = 24;

        const int count = static_cast<int>(6.0f + ev.intensity * 14.0f);
        emitter.particles.reserve(static_cast<size_t>(count));
        std::mt19937 rng{ std::random_device{}() };
        std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
        for (int i = 0; i < count; ++i) {
            Particle p;
            p.position = ev.worldPos;
            const float angle =
                (static_cast<float>(i) / static_cast<float>(count)) * math::TWO_PI
                + (dist01(rng) - 0.5f) * 0.8f;
            const float hSpeed = dist01(rng) * ev.intensity * 2.0f + 0.2f;
            const float vSpeed = dist01(rng) * ev.intensity * 5.0f + 1.5f;
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
    renderer.SetSampler(3, renderer::SamplerMode::BORDER_ZERO);

    {
        struct CameraCB {
            math::Matrix4 view;
            math::Matrix4 projection;
            math::Matrix4 viewProjection;
            math::Matrix4 invViewProjection;
            math::Vector3 cameraPos;
            float nearZ;
            float farZ;
            float waterSsrEnabled;
            float _pad[2];
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
        camData.waterSsrEnabled = (ctx.isDeferred && ctx.settings.ssr.enabled) ? 1.0f : 0.0f;
        resources.Update(cameraCBH, &camData, sizeof(camData));
    }

    static float s_lastElapsedTime = elapsedTime;
    const float dt = math::Clamp(elapsedTime - s_lastElapsedTime, 0.0f, 0.1f);
    s_lastElapsedTime = elapsedTime;

    for (auto [water, transform] : scene.View<WaterComponent, Transform>()) {
        if (!water.enabled) continue;
        water.resolutionX = std::clamp(water.resolutionX, 1u, 512u);
        water.resolutionZ = std::clamp(water.resolutionZ, 1u, 512u);
        water.chunkCount  = std::clamp(water.chunkCount,  1u, 64u);
        water.extentX     = std::clamp(water.extentX, 0.1f, 10000.0f);
        water.extentZ     = std::clamp(water.extentZ, 0.1f, 10000.0f);

        EntityID eid = FindEntityForWater(scene, water);
        if (!scene.IsValid(eid)) continue;
        {
            const auto* go = scene.GetGameObject(eid);
            if (!go || !go->activeInHierarchy()) continue;
            if (!fbzz::Layer::Contains(ctx.cullingMask, go->layer)) continue;
        }

        float maxAmp = 0.0f;
        for (const auto& wave : water.waves) maxAmp += wave.amplitude;
        const float yMargin = maxAmp + 0.5f;
        const math::Vector3 waterMin = { -water.extentX * 0.5f, -yMargin, -water.extentZ * 0.5f };
        const math::Vector3 waterMax = {  water.extentX * 0.5f,  yMargin,  water.extentZ * 0.5f };
        if (!AabbVisible(frustum, transform.position, waterMin, waterMax))
            continue;

        const asset::MaterialAsset* mat = nullptr;
        if (!water.materialPath.empty()) {
            const auto handle = asset::AssetManager::LoadMaterial(water.materialPath);
            mat = asset::AssetManager::GetMaterial(handle);
        }

        const float foamThreshold = WGetF(mat, "foamThreshold", 0.3f);
        const float foamFade      = WGetF(mat, "foamFade",      0.5f);

        if (water.meshDirty || !s_meshCache.contains(eid.index)) {
            BuildWaterMesh(water, s_meshCache[eid.index], resources);
            water.meshDirty = false;
            water.foamDirty = true;
        }

        if (water.texDirty || water.foamDirty || !s_texCache.contains(eid.index)) {
            s_texCache[eid.index] = BuildTextureSet(
                mat, water, transform, scene, resources,
                foamThreshold, foamFade,
                flatNormalTex, whiteTex, blackTex, neutralFlowTex);
            water.texDirty = false;
            water.foamDirty = false;
        }

        WaterRippleState& rippleState = s_rippleStates[eid.index];
        if (rippleState.dirty || !rippleState.gpuTex.IsValid() || !rippleState.ripples.empty())
            UpdateRippleState(rippleState, dt, resources);

        const WaterMesh& mesh = s_meshCache.at(eid.index);
        if (!AabbVisible(frustum, transform.position, mesh.aabbMin, mesh.aabbMax))
            continue;

        const WaterCB cb = BuildWaterCB(water, mat, transform, camera, elapsedTime);
        resources.Update(waterCBH, &cb, sizeof(cb));

        auto effectCBH = defaultEffectCBH;
        if (waterEffectCBH.IsValid()) {
            const WaterEffectParams effectParams = BuildWaterEffectParams(mat);
            resources.Update(waterEffectCBH, &effectParams, sizeof(effectParams));
            effectCBH = waterEffectCBH;
        }

        const WaterTextures& textures = s_texCache.at(eid.index);
        // WHY: outputRT を RTV/DSV としてバインドしたまま、その depth を SRV(t5) として読むことは DX11 で禁止。
        //      Water パス開始時にコピーした depth を読むことで、背景・水深判定を安定させる。
        const renderer::ResourceHandle<renderer::TextureTag> depthTex = sceneDepth;
        const renderer::ResourceHandle<renderer::TextureTag> colorTex =
            sceneColor.IsValid() ? sceneColor : blackTex;
        const renderer::ResourceHandle<renderer::TextureTag> rippleTex =
            rippleState.gpuTex.IsValid() ? rippleState.gpuTex : neutralRippleTex;

        const auto activeShader = (mat && !mat->shaderPath.empty())
            ? resources.LoadShader(mat->shaderPath)
            : waterShader;

        for (const WaterChunk& chunk : mesh.chunks) {
            // 水面チャンクも地形と同様、独立にカリングされる描画候補として数える。
            ++ctx.statsTotalObjects;
            // 地形チャンクと同様、カメラの Frustum Culling を切っている間は落とさない。
            if (ctx.frustumCullingEnabled &&
                !AabbVisible(frustum, transform.position, chunk.aabbMin, chunk.aabbMax)) {
                ++ctx.statsFrustumCulled;
                continue;
            }

            renderer::DrawCall call;
            call.vertexBuffer = chunk.vertexBuffer;
            call.indexBuffer  = chunk.indexBuffer;
            call.shader       = activeShader;
            call.pipelineState = (settings && settings->IsWireframe()) ? waterWireframePSO : waterPSO;
            call.indexCount   = chunk.indexCount;
            call.layer        = renderer::RenderLayer::TRANSPARENT_LAYER;
            call.topology     = renderer::PrimitiveTopology::TRIANGLE_LIST;
            call.constantBuffers[0] = cameraCBH;
            call.constantBuffers[1] = waterCBH;
            call.constantBuffers[2] = effectCBH;
            call.constantBuffers[3] = lightCB;
            call.constantBuffers[4] = shadowCB;
            call.constantBuffers[8] = ctx.handles.advancedGraphicsCB;
            BindClusterLighting(call, ctx);
            call.textures[0] = textures.normalMap1;
            call.textures[1] = textures.normalMap2;
            call.textures[2] = textures.foamTex;
            call.textures[3] = textures.foamMask;
            call.textures[4] = textures.envTex;
            call.textures[5] = depthTex;
            call.textures[6] = colorTex;
            call.textures[7] = textures.flowMap;
            call.textures[8] = rippleTex;
            call.textures[9] = shadowDepthTexture;
            SubmitCounted(ctx, call);
        }
    }
}

} // namespace fbzz::scene
