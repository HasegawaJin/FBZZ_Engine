/// @file    WaterRenderPass.cpp
/// @brief   WaterComponent → GPU 水面メッシュ・泡マスク・波紋テクスチャ生成と描画 (IRenderPass 実装)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// @note 水面は透明描画・Terrain 高さ参照・動的 CPU テクスチャ更新を扱う。GPU リソースは
///       Component に持たせず System 側の static cache に閉じ、Scene データは保存しやすい
///       純粋なパラメータのまま保つ。
#include "Engine/Scene/Systems/RenderPasses/Geometry/WaterRenderPass.hpp"
#include "GeometryPasses.hpp"
#include "WaterNoiseBake.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Asset/VelocityFieldAtlas.hpp"
#include "Fluid/VectorFieldAsset.hpp"
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

/// @note WaterVertex / WaterCB / WaterEffectParams は `RenderPassContext.hpp` にある。
///       エディタのマテリアルプレビューが本編と同じレイアウトを焼く必要があり、ここに複製すると黙ってずれる。

/// @brief チャンク 1 個分の GPU リソースと、波を乗せる前のローカル AABB。
/// @note 波の振幅は meshDirty を立てずに変わるため AABB へ焼き込まない。焼き込むと振幅を
///       上げた瞬間から AABB だけ古くなり、見えている端のチャンクが消える。
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

/// 頂点が Gerstner 変位で平面から出る量。水平は Q*A、垂直は A の総和が上限。
struct WaveMargin {
    float horizontal = 0.0f;
    float vertical   = 0.0f;
};

WaveMargin ComputeWaveMargin(const WaterComponent& water)
{
    WaveMargin margin;
    if (water.enableGerstnerWaves) {
        /// @note 群の包絡は最大 (1 + waveGrouping) 倍、方向広がりは主 + 伴走の和まで振幅を持ち上げる。
        ///       ここに入れ忘れると、山に当たったチャンクだけが «AABB からはみ出した» 扱いで消える。
        const float peak = (1.0f + math::Clamp01(water.waveGrouping))
                         * WaterComponent::WaveSpreadAmplitudeSum(water.waveSpread);
        for (const GerstnerWave& wave : water.waves) {
            const float amplitude = (std::max)(wave.amplitude, 0.0f) * peak;
            margin.vertical   += amplitude;
            margin.horizontal += amplitude * math::Clamp01(wave.steepness);
        }
    }
    /// @note 流れの場が作る形と着水の輪も «平面から出る量»。入れないと穴や山や輪の上のチャンク
    ///       だけが «AABB からはみ出した» 扱いで消える。盛り上がりも穴と同じ枠で測る。
    float flowDisplacement = 0.0f;
    for (int i = 0; i < (std::min)(water.surfaceFlowCount, kWaterSurfaceFlowCount); ++i) {
        flowDisplacement = (std::max)(
            flowDisplacement, std::abs(water.surfaceFlows[static_cast<size_t>(i)].height));
    }
    /// @note 輪の合成断面の上限は本体 + 後続 2 本 = 1.58 倍。
    margin.vertical += flowDisplacement
                     + (water.ripples.empty() ? 0.0f : kWaterRippleHeightAmplitude * 1.6f);

    /// @note さざ波・法線ゆらぎのぶんだけ余裕を持たせる。
    margin.horizontal += 0.5f;
    margin.vertical   += 0.5f;
    return margin;
}

/// @brief 水面が使うテクスチャはエンジンが生成する泡マスクだけ。
/// @note 法線・泡・環境反射は手続き / 空連動 IBL から取るため、オーサリング済みテクスチャ資産は不要。
struct WaterTextures {
    renderer::ResourceHandle<renderer::TextureTag> foamMask;
};

/// 焼いた 1 枚と «前のフレームに輪があったか»。
/// @note 輪そのものは WaterComponent::ripples が正本。ここに置くと SceneView と GameView で
///       寿命が 2 回進み、CPU 側の GetSurfaceHeightAt からも読めない。
struct WaterRippleState {
    std::vector<uint8_t> pixels;
    uint32_t width = kWaterRippleTextureSize;
    uint32_t height = kWaterRippleTextureSize;
    renderer::ResourceHandle<renderer::TextureTag> gpuTex;
    bool dirty = true;
    /// 最後に焼いたとき輪があったか。落としきった次のフレームに 1 回だけ焼き直すため。
    bool hadRipples = false;
};

static std::unordered_map<uint32_t, WaterMesh> s_meshCache;
static std::unordered_map<uint32_t, WaterTextures> s_texCache;
static std::unordered_map<uint32_t, WaterRippleState> s_rippleStates;

struct SplashEvent {
    math::Vector3 worldPos;
    float         intensity;
};

static std::vector<SplashEvent> s_pendingSplashes;

/// しぶき GameObject の名前。UpdateWaterSplashes が破棄対象を見分ける印を兼ねる。
constexpr const char* kSplashObjectName = "__WaterSplash";
/// 1 フレームに積める上限。WaterSystem が回らない構成でキューが伸び続けないようにする。
constexpr size_t kMaxPendingSplashes = 64;

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

    /// @note 切り上げでなく境界を按分して分ける。ceil(res/chunks) を全チャンクに掛けると
    ///       最後のほうのチャンクは開始セルが res を追い越し、差が符号なしで折り返して
    ///       4G 要素の reserve になり、解像度を上げた瞬間に確保失敗で落ちていた。
    const uint32_t numChunks =
        (std::min)((std::max)(water.chunkCount, 1u), (std::min)(water.resolutionX, water.resolutionZ));
    auto splitAt = [](uint32_t cells, uint32_t index, uint32_t count) {
        return static_cast<uint32_t>((static_cast<uint64_t>(cells) * index) / count);
    };

    for (uint32_t cz = 0; cz < numChunks; ++cz) {
        for (uint32_t cx = 0; cx < numChunks; ++cx) {
            const uint32_t ixStart = splitAt(water.resolutionX, cx,     numChunks);
            const uint32_t ixEnd   = splitAt(water.resolutionX, cx + 1, numChunks);
            const uint32_t izStart = splitAt(water.resolutionZ, cz,     numChunks);
            const uint32_t izEnd   = splitAt(water.resolutionZ, cz + 1, numChunks);
            if (ixEnd <= ixStart || izEnd <= izStart) continue;

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
            chunk.aabbMin = { ox + static_cast<float>(ixStart) * dx, 0.0f, oz + static_cast<float>(izStart) * dz };
            chunk.aabbMax = { ox + static_cast<float>(ixEnd)   * dx, 0.0f, oz + static_cast<float>(izEnd)   * dz };
            mesh.chunks.push_back(std::move(chunk));
        }
    }

    mesh.aabbMin = { ox, 0.0f, oz };
    mesh.aabbMax = { -ox, 0.0f, -oz };
}

renderer::ResourceHandle<renderer::TextureTag> BuildFoamMask(
    Scene& scene,
    const WaterComponent& water,
    const Transform& waterTransform,
    float foamThreshold,
    float foamFade,
    renderer::ResourceManager& resources)
{
    /// @note 岸沿いのマスクは帯が出れば十分でメッシュ解像度に追随させる理由がなく、上限を置く。
    ///       無ければ解像度に比例した VRAM を毎回焼き直し、スライダー操作中だけで数百 MB を使う。
    constexpr uint32_t kMaxFoamMaskSide = 257u;
    const uint32_t width  = (std::min)(water.resolutionX + 1u, kMaxFoamMaskSide);
    const uint32_t height = (std::min)(water.resolutionZ + 1u, kMaxFoamMaskSide);
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

    /// @note ワールド行列を通すのは、水面が子オブジェクトだったり拡大されていると
    ///       ローカル position 基準では泡の帯だけが実際の水際からずれるため。
    const math::Matrix4 waterWorld = waterTransform.GetWorldMatrix();
    const float ox = -water.extentX * 0.5f;
    const float oz = -water.extentZ * 0.5f;
    /// @note テクセル «中心» を標本点にする。シェーダー側は uv → (u*width - 0.5) で引くため、
    ///       角合わせで焼くと泡の帯が半テクセルぶん岸からずれる。
    const float du = water.extentX / static_cast<float>(width);
    const float dv = water.extentZ / static_cast<float>(height);

    for (uint32_t iz = 0; iz < height; ++iz) {
        for (uint32_t ix = 0; ix < width; ++ix) {
            const math::Vector4 surface = waterWorld * math::Vector4{
                ox + (static_cast<float>(ix) + 0.5f) * du, 0.0f,
                oz + (static_cast<float>(iz) + 0.5f) * dv, 1.0f
            };
            const float localX = surface.x - terrainTransform->worldPosition.x;
            const float localZ = surface.z - terrainTransform->worldPosition.z;
            const float terrainY = terrain->GetHeightAt(localX, localZ) + terrainTransform->worldPosition.y;
            const float heightDiff = surface.y - terrainY;
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

WaterTextures BuildTextureSet(
    const WaterComponent& water,
    const Transform& waterTransform,
    Scene& scene,
    renderer::ResourceManager& resources,
    float foamThreshold,
    float foamFade)
{
    WaterTextures textures;
    textures.foamMask = BuildFoamMask(scene, water, waterTransform, foamThreshold, foamFade, resources);
    return textures;
}

/// 波紋テクスチャを焼く。RG = 法線 xy、**B = 頂点へ乗せる高さ** [m] (kWaterRippleHeightScale 正規化)。
///
/// @note 高さにだけ輪ごとの meshFade を掛ける。細い輪は頂点で刻めないので «傾きだけ» の帯へ落ち、
///       法線は今までどおり全部書く (water-waves.md の帯の原則)。
/// @note テクセルをワールド XZ へ直して距離を測る。UV 距離で測ると、非正方形の水面で輪が
///       楕円になり CPU の GetSurfaceHeightAt と食い違う。
/// @warning 断面は WaterComponent::WaterRippleWave が正本。ここに写しを作らないこと。
void UpdateRippleState(WaterRippleState& state, const WaterComponent& water,
                       const Transform& transform, renderer::ResourceManager& resources)
{
    const float sizeX = (std::max)(water.extentX * std::abs(transform.worldScale.x), 0.1f);
    const float sizeZ = (std::max)(water.extentZ * std::abs(transform.worldScale.z), 0.1f);
    const float invHeightScale = 1.0f / kWaterRippleHeightScale;

    state.pixels.assign(static_cast<size_t>(state.width) * static_cast<size_t>(state.height) * 4u, 128u);
    for (uint32_t y = 0; y < state.height; ++y) {
        for (uint32_t x = 0; x < state.width; ++x) {
            const math::Vector2 uv = {
                (static_cast<float>(x) + 0.5f) / static_cast<float>(state.width),
                (static_cast<float>(y) + 0.5f) / static_cast<float>(state.height)
            };
            /// @note EmitWaterRipple のワールド → UV と逆の写像。ずれると輪が半テクセル泳ぐ。
            const math::Vector2 world = {
                transform.worldPosition.x + (uv.x - 0.5f) * sizeX,
                transform.worldPosition.z + (uv.y - 0.5f) * sizeZ
            };

            math::Vector2 offset = math::Vector2::ZERO;
            float height = 0.0f;
            for (const WaterRipple& ripple : water.ripples) {
                const math::Vector2 d = world - ripple.center;
                const float dist = d.Length();
                if (dist <= math::EPSILON) continue;

                const float wave = WaterComponent::WaterRippleWave(ripple, dist) * ripple.amplitude;
                offset += d.Normalized() * wave;
                height += wave * ripple.meshFade * kWaterRippleHeightAmplitude;
            }

            const size_t p = (static_cast<size_t>(y) * state.width + x) * 4u;
            state.pixels[p + 0] = static_cast<uint8_t>(math::Clamp01(0.5f + offset.x * 0.5f) * 255.0f);
            state.pixels[p + 1] = static_cast<uint8_t>(math::Clamp01(0.5f + offset.y * 0.5f) * 255.0f);
            state.pixels[p + 2] =
                static_cast<uint8_t>(math::Clamp01(0.5f + height * invHeightScale * 0.5f) * 255.0f);
            state.pixels[p + 3] = 255u;
        }
    }

    if (state.gpuTex.IsValid())
        resources.Release(state.gpuTex);
    state.gpuTex = resources.CreateTexture(state.pixels.data(), state.width, state.height);
    state.dirty = false;
    state.hadRipples = !water.ripples.empty();
}

/// viewProjection は TAA ジッター込みで渡す。カメラから組み直すとジッターが落ちる。
/// @param resources 焼いた速度場を常駐させる (VelocityFieldAtlas) のに要る。
WaterCB BuildWaterCB(const WaterComponent& water, const asset::MaterialAsset* mat,
                     const Transform& transform, const math::Matrix4& viewProjection, float time,
                     float skyReflection, const WaterDetailNoise& detailNoise,
                     renderer::ResourceManager& resources)
{
    WaterCB cb{};
    const math::Matrix4 world = transform.GetWorldMatrix();
    cb.worldMatrix = world;
    cb.wvpMatrix = viewProjection * world;

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
    /// @note 水面の «実寸» はローカル extent ではなくワールド実寸。拡大された水面でも、縁のフェードと
    ///       波のエイリアシング判定が、画面に出ているとおりの大きさで効くようにする。
    const float worldExtentX = (std::max)(water.extentX * std::abs(transform.worldScale.x), 0.0001f);
    const float worldExtentZ = (std::max)(water.extentZ * std::abs(transform.worldScale.z), 0.0001f);
    /// @note z はさざ波タイルの勾配復号係数。ベイクした値をそのまま渡し、シェーダー側に定数を
    ///       写さない (写すとタイルを焼き直してもシェーダーが古い係数で復号し続ける)。
    cb.normalParams = { worldExtentX, worldExtentZ, detailNoise.derivativeScale,
                        WGetF(mat, "normalStrength", 1.0f) };
    /// @note 頂点グリッド 1 セルの実寸。«刻めない波» の判断を CPU (浮力) と揃えるため、
    ///       描画側で計算し直さず WaterSystem が解決した値をそのまま渡す。z はタイルのセル数の逆数。
    cb.timeParams   = { water.cellSize.x, water.cellSize.y, detailNoise.invTileCells, time };
    cb.foamParams = {
        WGetF(mat, "foamThreshold",     0.3f),
        WGetF(mat, "foamFade",          0.5f),
        WGetF(mat, "foamStrength",      1.0f),
        /// @note 泡のムラはワールド座標で評価するため、旧 foamTiling(UV 倍率) とは単位が違う。
        WGetF(mat, "foamNoiseScale",    0.5f)
    };
    /// @note 外周フェードの幅はメートルで持ち、軸ごとに extent で割って UV へ直す。UV 比で
    ///       持つと大きい水面ほど帯が広くなり、縁から何 m で消えるかが大きさに依存してしまう。
    const float edgeFadeMeters = (std::max)(WGetF(mat, "edgeFade", 1.5f), 0.0f);
    cb.refractionFlowParams = {
        WGetF(mat, "refractionStrength", 0.03f),
        WGetF(mat, "flowSpeed",          0.3f),
        edgeFadeMeters / worldExtentX,
        edgeFadeMeters / worldExtentZ
    };
    cb.detailParams = {
        WGetF(mat, "detailScale",    0.35f),
        WGetF(mat, "detailSpeed",    0.6f),
        WGetF(mat, "detailStrength", 1.0f),
        math::Clamp01(WGetF(mat, "smoothness", 0.92f))
    };
    const auto sssColor = WGetF3(mat, "sssColor", { 0.12f, 0.50f, 0.46f });
    cb.sssParams = { sssColor.x, sssColor.y, sssColor.z,
                     math::Clamp01(WGetF(mat, "sssStrength", 0.6f)) };
    /// @note 波の山ほど透過光を強くするため、CPU 側と同じ「振幅の合計」を波高の基準として渡す。
    ///       群の包絡と方向広がりのぶんも含める。含めないと、山に当たった波だけ waveState.x が 1 で
    ///       飽和し、透過光と白波が «そこだけ最大» に張り付く。
    const float peak = (1.0f + math::Clamp01(water.waveGrouping))
                     * WaterComponent::WaveSpreadAmplitudeSum(water.waveSpread);
    float waveHeightSum = 0.0f;
    if (water.enableGerstnerWaves)
        for (const GerstnerWave& w : water.waves) waveHeightSum += (std::max)(w.amplitude, 0.0f);
    cb.reflectParams = {
        skyReflection * math::Clamp01(WGetF(mat, "skyReflection", 1.0f)),
        math::Clamp01(water.waveGrouping),
        transform.worldPosition.y,
        (std::max)(waveHeightSum * peak, 0.01f)
    };
    const auto flowDir = WGetF2(mat, "flowDirection", { 1.0f, 0.0f });
    const float flowLen = std::sqrt(flowDir.x * flowDir.x + flowDir.y * flowDir.y);
    /// @note zw はさざ波の «形»。異方比は風向と直交する «うね» の伸び、ワープ幅はうねりの斜面が
    ///       さざ波を運ぶ距離 [m]。どちらも 1.0 / 0.0 にすれば従来の等方・非追従へ戻る。
    const math::Vector2 detailShape = {
        (std::max)(WGetF(mat, "detailAnisotropy", 2.0f), 1.0f),
        (std::max)(WGetF(mat, "detailWarp",       0.5f), 0.0f)
    };
    cb.flowParams = flowLen > 1.0e-4f
        ? math::Vector4{ flowDir.x / flowLen, flowDir.y / flowLen, detailShape.x, detailShape.y }
        : math::Vector4{ 1.0f, 0.0f, detailShape.x, detailShape.y };
    /// @note 流れの場が出す形・流れ・質感。本数は y に入れる (枠を 1 つ増やさずに済む)。
    /// @note 高さも流速も CPU で決める。形の式は WaterComponent が正本で、浮力・水中判定と
    ///       同じ値でなければならない。シェーダーで導き直すと二重化が 1 本増える。
    const int flowCount = (std::min)(water.surfaceFlowCount, kWaterSurfaceFlowCount);
    cb.waveShapeParams = { math::Clamp01(water.waveSpread),
                           static_cast<float>(flowCount), 0.0f, 0.0f };
    for (int i = 0; i < kWaterSurfaceFlowCount; ++i) {
        if (i >= flowCount) continue;
        const WaterSurfaceFlow& f = water.surfaceFlows[static_cast<size_t>(i)];
        cb.surfaceFlowA[i] = { f.center.x, f.center.y, f.radius, f.height };
        cb.surfaceFlowB[i] = { f.speed, f.falloffPower,
                               static_cast<float>(static_cast<int>(f.kind)), f.chop };
        /// @note 常駐できなかった場はタイル -1 で渡す。本数を詰めると別の流れに化けるので、
        ///       枠は残したまま «無効» を送る (ParticleGpuSim と同じ扱い)。
        float tile = -1.0f;
        float maxMagnitude = 1.0f;
        if (f.kind == FlowFieldType::Baked && f.vectorField != nullptr) {
            tile = static_cast<float>(asset::VelocityFieldAtlas::Acquire(*f.vectorField, resources));
            maxMagnitude = f.vectorField->maxMagnitude;
        }
        cb.surfaceFlowC[i] = { f.direction.x, f.direction.y, tile, maxMagnitude };
        cb.surfaceFlowD[i] = { f.inverseRotation.x, f.inverseRotation.y,
                               f.inverseRotation.z, f.inverseRotation.w };
        cb.surfaceFlowE[i] = { f.extents.x, f.extents.y, f.extents.z, f.planeOffsetY };
    }

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
    const math::Vector3 skyTint = WGetF3(mat, "skyReflectTint", {
        params.skyReflectTint[0], params.skyReflectTint[1], params.skyReflectTint[2]
    });
    params.skyReflectTint[0] = skyTint.x;
    params.skyReflectTint[1] = skyTint.y;
    params.skyReflectTint[2] = skyTint.z;
    const math::Vector3 rippleColor = WGetF3(mat, "rippleRingColor", {
        params.rippleRingColor[0], params.rippleRingColor[1], params.rippleRingColor[2]
    });
    params.rippleRingColor[0] = rippleColor.x;
    params.rippleRingColor[1] = rippleColor.y;
    params.rippleRingColor[2] = rippleColor.z;
    params.rippleRingStrength = WGetF(mat, "rippleRingStrength", params.rippleRingStrength);
    return params;
}

/// @brief ローカル AABB をワールド行列で包み直して可視判定する。
/// @note 描画側は `Transform::GetWorldMatrix()` を使い親の回転・スケールが乗るため、
///       カリングをローカル position 基準にすると水面を子にしたり拡大した瞬間に消える。
bool AabbVisible(const math::Frustum& frustum, const math::Matrix4& world,
                 const math::Vector3& localMin, const math::Vector3& localMax)
{
    const math::Vector3 localCenter = {
        (localMin.x + localMax.x) * 0.5f,
        (localMin.y + localMax.y) * 0.5f,
        (localMin.z + localMax.z) * 0.5f,
    };
    const math::Vector3 localExtents = {
        (localMax.x - localMin.x) * 0.5f,
        (localMax.y - localMin.y) * 0.5f,
        (localMax.z - localMin.z) * 0.5f,
    };

    const math::Vector4 center =
        world * math::Vector4{ localCenter.x, localCenter.y, localCenter.z, 1.0f };
    /// @note 回転した箱を軸並行で包み直す = |M| を half-extent に掛ける。
    auto projectRow = [&](int row) {
        return std::abs(world.m[row][0]) * localExtents.x
             + std::abs(world.m[row][1]) * localExtents.y
             + std::abs(world.m[row][2]) * localExtents.z;
    };
    const math::Vector3 extents = { projectRow(0), projectRow(1), projectRow(2) };
    return frustum.IntersectsAABB({ center.x, center.y, center.z }, extents);
}

/// 波のマージンを乗せたローカル AABB。チャンクにも水面全体にも同じ広げ方をする。
void ExpandByWaveMargin(const WaveMargin& margin, math::Vector3& outMin, math::Vector3& outMax)
{
    outMin.x -= margin.horizontal;
    outMin.z -= margin.horizontal;
    outMax.x += margin.horizontal;
    outMax.z += margin.horizontal;
    outMin.y -= margin.vertical;
    outMax.y += margin.vertical;
}

} // namespace

const WaterDetailNoise& GetWaterDetailNoise(renderer::ResourceManager& resources)
{
    static WaterDetailNoise s_noise;
    static uint64_t s_bakedAt = 0xFFFFFFFFFFFFFFFFull;

    /// @note 焼き直しを Reset に紐づける。デバイスロストで実体が消えてもハンドルは残るため、
    ///       世代が変わったときだけ焼き直せば失敗時に毎フレーム焼き続けずに済む。
    const uint64_t resetVersion = resources.GetResetVersion();
    if (s_bakedAt == resetVersion)
        return s_noise;
    s_bakedAt = resetVersion;

    const waternoise::Tile tile = waternoise::BakeDetailTile();
    std::vector<renderer::TextureMipData> mips;
    mips.reserve(tile.mips.size());
    for (size_t level = 0; level < tile.mips.size(); ++level)
        mips.push_back({ tile.mips[level].data(), tile.sizes[level], tile.sizes[level] });

    s_noise.texture = resources.CreateTextureWithMips(mips.data(), static_cast<uint32_t>(mips.size()));
    s_noise.derivativeScale = tile.derivativeScale;
    s_noise.invTileCells = 1.0f / static_cast<float>(waternoise::kTileCells);
    return s_noise;
}

void QueueWaterSplash(const math::Vector3& worldPos, float intensity)
{
    if (s_pendingSplashes.size() >= kMaxPendingSplashes) return;
    s_pendingSplashes.push_back({ worldPos, math::Clamp01(intensity) });
}

void UpdateWaterSplashes(Scene& scene)
{
    /// @note 先に «鳴り終わった» を集めてから消す。消しながら GetEntities を回すと配列が詰め替わる。
    std::vector<EntityID> finished;
    for (EntityID id : scene.GetEntities<ParticleEmitter>()) {
        const GameObject* go = scene.GetGameObject(id);
        if (!go || !go->runtimeGenerated || go->name != kSplashObjectName) continue;
        const auto* emitter = scene.GetComponent<ParticleEmitter>(id);
        if (!emitter || emitter->runtime.particles.empty()) finished.push_back(id);
    }
    for (EntityID id : finished)
        scene.DestroyGameObject(id);

    for (const SplashEvent& ev : s_pendingSplashes) {
        auto& go = scene.CreateGameObject(kSplashObjectName);
        /// @note 数秒で消える演出用。保存に混ざると開くたびに消えない GO が増える。
        go.runtimeGenerated = true;
        go.transform.position = ev.worldPos;

        ParticleEmitter emitter;
        emitter.settings.enabled      = true;
        emitter.settings.emitRate     = 0.0f;
        emitter.settings.lifetime     = 1.2f;
        emitter.settings.sizeStart    = 0.10f + ev.intensity * 0.20f;
        emitter.settings.sizeEnd      = 0.0f;
        emitter.settings.colorStart   = { 0.75f, 0.93f, 1.0f, 0.9f };
        emitter.settings.colorEnd     = { 0.55f, 0.80f, 1.0f, 0.0f };
        emitter.settings.maxParticles = 24;

        const int count = static_cast<int>(6.0f + ev.intensity * 14.0f);
        emitter.runtime.particles.reserve(static_cast<size_t>(count));
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
            p.color    = emitter.settings.colorStart;
            p.size     = emitter.settings.sizeStart;
            p.age      = 0.0f;
            emitter.runtime.particles.push_back(std::move(p));
        }
        go.AddComponent<ParticleEmitter>(std::move(emitter));
    }
    s_pendingSplashes.clear();
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

/// @name IRenderPass

std::string_view WaterRenderPass::Name() const { return "WaterForward"; }

void WaterRenderPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    /// @note 平行光の影 (t9) に加え、BindForwardShadingResources が Spot/Point の影 (t28) と
    ///       Cookie (t31) を束縛する。申告しないと Shadow / LightCookie より先に走ってよいことになる。
    ///
    ///       屈折用のシーンカラー / 深度のコピーはこのパスの中で作って読み切る作業用で、
    ///       他のパスからは見えない。元の HDR は下の ReadWrite で押さえてある。
    ///       SetAutoTarget は呼ばない。屈折用のコピーを作る間に束縛を 3 回切り替えるので、
    ///       描き先は Execute の中で自分で張る。
    builder.ReadWrite("HDR").Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas");
}

void WaterRenderPass::Execute(PassResources&, RenderPassContext& ctx)
{
    Scene& scene = ctx.scene;
    renderer::IRenderer& renderer = ctx.renderer;
    renderer::ResourceManager& resources = ctx.resources;
    const renderer::Camera& camera = ctx.camera;
    const renderer::RenderSettings* settings = &ctx.settings;
    const auto lightCB = ctx.handles.lightCB;
    const auto shadowDepthTexture = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    const auto shadowCB = ctx.handles.shadowCB;
    const float elapsedTime = Time::time;

    /// @note static ローカルは初回のみ初期化される。`ResourceManager::Reset()` で世代が
    ///       変わった場合だけ再生成し、旧ハンドル (失効済み) へのアクセスを防ぐ。
    static uint64_t s_resetVersion = resources.GetResetVersion();
    static auto waterShader = resources.LoadShader("Assets/Shaders/Water/Water.hlsl");
    /// @note 半透明だが深度書き込みありで描く。水面は 1 枚の面で重なるのは自分自身だけなので、
    ///       書き込みを切るとチャンクの submit 順 (-Z→+Z の行優先) がそのままブレンド順に
    ///       なり、+Z 向きカメラでは奥のチャンクが後から手前へ上塗りされ、向こう側の縁や
    ///       うねりの裏面 (SOLID_NOCULL で描かれる) が手前の水面に線となって浮く。深度を
    ///       書けば提出順に関係なく一番手前の水面だけが残る。水中の向こうの不透明物は屈折用
    ///       シーンカラーコピーから引いており、深度を書いても水底は見えたままになる。
    static auto waterPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto waterWireframePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::WIREFRAME,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_ON
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
    static auto blackTex = [&] {
        const uint8_t b[4] = { 0, 0, 0, 255 };
        return resources.CreateTexture(b, 1, 1);
    }();
    /// @note B は «高さ 0» の 128。255 のままだと波紋が 1 枚も無い水面の頂点が
    ///       kWaterRippleHeightScale ぶん持ち上がる。
    static auto neutralRippleTex = [&] {
        const uint8_t r[4] = { 128, 128, 128, 255 };
        return resources.CreateTexture(r, 1, 1);
    }();

    /// @note 水面の屈折用にシーンカラーをコピーする。Water シェーダーが屈折で HDR をシーン
    ///       カラーとして読むため、hdrRT を出力 RT として束ねる前にコピーする必要がある
    ///       (DX11 は同時読み書きを禁止する)。
    static auto copyColorShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
    static auto depthCopyShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");

    if (s_resetVersion != resources.GetResetVersion()) {
        s_resetVersion    = resources.GetResetVersion();
        waterShader       = resources.LoadShader("Assets/Shaders/Water/Water.hlsl");
        waterPSO          = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_ON });
        waterWireframePSO = resources.CreatePipelineState({ renderer::RasterizerMode::WIREFRAME,    renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_ON });
        cameraCBH         = resources.CreateConstantBuffer(288);
        waterCBH          = resources.CreateConstantBuffer(sizeof(WaterCB));
        defaultEffectCBH  = [&] { WaterEffectParams d{}; auto h = resources.CreateConstantBuffer(sizeof(WaterEffectParams)); resources.Update(h, &d, sizeof(WaterEffectParams)); return h; }();
        waterEffectCBH    = resources.CreateConstantBuffer(sizeof(WaterEffectParams));
        blackTex          = [&] { const uint8_t b[4] = {   0,   0,   0, 255 }; return resources.CreateTexture(b, 1, 1); }();
        neutralRippleTex  = [&] { const uint8_t r[4] = { 128, 128, 128, 255 }; return resources.CreateTexture(r, 1, 1); }();
        copyColorShader   = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
        depthCopyShader   = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
        /// @note `Reset()` は実体を全て破棄済みで握っているのは失効したハンドルだけなので、
        ///       返さず空にする。次のループが素直に作り直す。
        s_meshCache.clear();
        s_texCache.clear();
        s_rippleStates.clear();
    }
    /// @note 無効になったエンティティのキャッシュを解放する
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
        std::erase_if(s_texCache, [&validIndices, &resources](auto& kv) {
            if (validIndices.count(kv.first)) return false;
            if (kv.second.foamMask.IsValid()) resources.Release(kv.second.foamMask);
            return true;
        });
        std::erase_if(s_rippleStates, [&validIndices, &resources](auto& kv) {
            if (validIndices.count(kv.first)) return false;
            if (kv.second.gpuTex.IsValid()) resources.Release(kv.second.gpuTex);
            return true;
        });
    }

    /// @note カリング錐台はジッター無しのまま。半ピクセルのために可視判定を揺らす意味がない。
    const math::Frustum frustum = math::Frustum::FromViewProjection(camera.GetViewProjection());

    /// @note 描画用の行列だけ TAA ジッターを乗せる。乗せないと水面だけ AA が効かず、
    ///       b0 経由で描く不透明物とサブピクセルずれた深度になって TAA の再投影が濁る。
    const math::Matrix4 jitteredProj =
        MakeJitteredProjection(camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    const math::Matrix4 jitteredVP = jitteredProj * camera.GetViewMatrix();

    bool hasVisibleWater = false;
    for (auto [water, transform] : scene.View<WaterComponent, Transform>()) {
        if (!water.enabled) continue;
        EntityID eid = FindEntityForWater(scene, water);
        if (!scene.IsValid(eid)) continue;

        const auto* go = scene.GetGameObject(eid);
        if (!go || !go->activeInHierarchy()) continue;
        if (!fbzz::Layer::Contains(ctx.cullingMask, go->layer)) continue;

        const WaveMargin margin = ComputeWaveMargin(water);
        math::Vector3 waterMin = { -water.extentX * 0.5f, 0.0f, -water.extentZ * 0.5f };
        math::Vector3 waterMax = {  water.extentX * 0.5f, 0.0f,  water.extentZ * 0.5f };
        ExpandByWaveMargin(margin, waterMin, waterMax);
        if (AabbVisible(frustum, transform.GetWorldMatrix(), waterMin, waterMax)) {
            hasVisibleWater = true;
            break;
        }
    }
    if (!hasVisibleWater) return;

    /// @note 作業 RT はビューが持つ (理由は RenderPassHandles::waterSceneColorRT を参照)。
    if (!ctx.handles.waterSceneColorRT || !ctx.handles.waterSceneDepthRT) return;
    renderer::SizedRenderTarget& sceneColorRT = *ctx.handles.waterSceneColorRT;
    renderer::SizedRenderTarget& sceneDepthRT = *ctx.handles.waterSceneDepthRT;

    (void)sceneColorRT.Ensure(resources, ctx.width, ctx.height, 1);
    (void)sceneDepthRT.Ensure(resources, ctx.width, ctx.height, 0);
    renderer.SetRenderTarget(sceneColorRT, resources);
    if (copyColorShader.IsValid()) {
        renderer::DrawCall copyDC;
        copyDC.shader = copyColorShader;
        copyDC.pipelineState = ctx.handles.postprocPSO;
        copyDC.vertexCount = 3;
        copyDC.textures[5] = resources.GetColorTexture(ctx.Res().Target("HDR"), 0);
        renderer.Submit(copyDC, resources);
    }
    const auto sceneColor = resources.GetColorTexture(sceneColorRT, 0);

    /// @note HDR の depth を Water 専用の深度 RT へコピーし、PS ではその SRV (t5) を読む。
    ///       hdrRT を RTV/DSV として使いながら同じ depth を SRV(t5) で読むと DX11 の
    ///       read/write 競合で SRV が解除され、背景判定・水深・泡が破綻する。
    renderer.SetRenderTarget(sceneDepthRT, resources);
    renderer.ClearDepth();
    if (depthCopyShader.IsValid()) {
        renderer::DrawCall depthDC;
        depthDC.shader = depthCopyShader;
        depthDC.pipelineState = ctx.handles.defaultPSO;
        depthDC.vertexCount = 3;
        depthDC.textures[7] = resources.GetDepthTexture(ctx.Res().Target("HDR"));
        renderer.Submit(depthDC, resources);
    }
    const auto sceneDepth = resources.GetDepthTexture(sceneDepthRT);

    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);

    /// @note しぶきの GameObject は UpdateWaterSplashes (WaterSystem) が作って消す。
    ///       描画中にシーンを書き換えると、後続パスが握るエミッターがすり替わる。

    {
        struct CameraCB {
            math::Matrix4 view;
            math::Matrix4 projection;
            math::Matrix4 viewProjection;
            math::Matrix4 invViewProjection;
            math::Vector3 cameraPos;
            float nearZ;
            float farZ;
            /// LAYOUT: PerFrameCB / Constants.hlsli の CameraConstants と一致させること
            /// (waterSsrEnabled は共通側で _reserved になっている枠)。
            float waterSsrEnabled;
            float isOrthographic;
            float _pad;
        };
        static_assert(sizeof(CameraCB) == 288, "CameraCB size mismatch");

        CameraCB camData{};
        camData.view = camera.GetViewMatrix();
        camData.projection = jitteredProj;
        camData.viewProjection = jitteredVP;
        camData.invViewProjection = math::Matrix4::Inverse(jitteredVP);
        camData.cameraPos = camera.m_position;
        camData.nearZ = camera.m_near;
        camData.farZ = camera.m_far;
        camData.waterSsrEnabled = (ctx.isDeferred && ctx.settings.ssr.enabled) ? 1.0f : 0.0f;
        camData.isOrthographic  =
            camera.m_projection == renderer::ProjectionMode::Orthographic ? 1.0f : 0.0f;
        resources.Update(cameraCBH, &camData, sizeof(camData));
    }

    /// @note 輪の寿命は WaterSystem が進める。ここで進めると SceneView と GameView で
    ///       2 回進み、ビューを 2 つ開いた瞬間に波紋が倍速で消える。

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

        const math::Matrix4 waterWorld = transform.GetWorldMatrix();
        const WaveMargin margin = ComputeWaveMargin(water);
        {
            math::Vector3 waterMin = { -water.extentX * 0.5f, 0.0f, -water.extentZ * 0.5f };
            math::Vector3 waterMax = {  water.extentX * 0.5f, 0.0f,  water.extentZ * 0.5f };
            ExpandByWaveMargin(margin, waterMin, waterMax);
            if (!AabbVisible(frustum, waterWorld, waterMin, waterMax))
                continue;
        }

        const asset::MaterialAsset* mat = nullptr;
        if (!water.materialPath.empty()) {
            const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(water.materialPath);
            mat = asset::AssetManager::Get<asset::MaterialAsset>(handle);
        }

        const float foamThreshold = WGetF(mat, "foamThreshold", 0.3f);
        const float foamFade      = WGetF(mat, "foamFade",      0.5f);

        if (water.meshDirty || !s_meshCache.contains(eid.index)) {
            BuildWaterMesh(water, s_meshCache[eid.index], resources);
            water.meshDirty = false;
            water.foamDirty = true;
        }

        if (water.texDirty || water.foamDirty || !s_texCache.contains(eid.index)) {
            WaterTextures& cached = s_texCache[eid.index];
            /// @note 焼き直しは解像度スライダー操作中フレーム毎に走るため、上書き前に解放する。
            ///       返さず差し替えると 1 フレームぶんの泡マスクがそのまま漏れ続ける。
            if (cached.foamMask.IsValid()) resources.Release(cached.foamMask);
            cached = BuildTextureSet(
                water, transform, scene, resources, foamThreshold, foamFade);
            water.texDirty = false;
            water.foamDirty = false;
        }

        /// @note 波紋がなく既存テクスチャも有効なら CPU 更新・GPU アップロードを省く。
        ///       hadRipples を見るのは最後の 1 つが消えたフレームに 1 回だけ焼き直すため。
        WaterRippleState& rippleState = s_rippleStates[eid.index];
        if (rippleState.dirty || !rippleState.gpuTex.IsValid()
            || !water.ripples.empty() || rippleState.hadRipples)
            UpdateRippleState(rippleState, water, transform, resources);

        const WaterMesh& mesh = s_meshCache.at(eid.index);
        {
            math::Vector3 meshMin = mesh.aabbMin;
            math::Vector3 meshMax = mesh.aabbMax;
            ExpandByWaveMargin(margin, meshMin, meshMax);
            if (!AabbVisible(frustum, waterWorld, meshMin, meshMax))
                continue;
        }

        /// @note 空反射は空連動 IBL のキューブが焼けているときだけ有効にする。DX12 の未バインド
        ///       スロットは Texture2D の null ディスクリプタなので、TextureCube 宣言のまま
        ///       参照させず、0 を渡してシェーダー側の分岐を閉じる。
        const bool hasSkyCube = ctx.handles.iblPrefilter.IsValid();
        const WaterDetailNoise& detailNoise = GetWaterDetailNoise(resources);
        const WaterCB cb = BuildWaterCB(water, mat, transform, jitteredVP, elapsedTime,
                                        hasSkyCube ? 1.0f : 0.0f, detailNoise, resources);
        resources.Update(waterCBH, &cb, sizeof(cb));

        auto effectCBH = defaultEffectCBH;
        if (waterEffectCBH.IsValid()) {
            const WaterEffectParams effectParams = BuildWaterEffectParams(mat);
            resources.Update(waterEffectCBH, &effectParams, sizeof(effectParams));
            effectCBH = waterEffectCBH;
        }

        const WaterTextures& textures = s_texCache.at(eid.index);
        /// @note outputRT を RTV/DSV としてバインドしたままその depth を SRV(t5) で読むことは
        ///       DX11 で禁止のため、Water パス開始時にコピーした depth を読み背景・水深判定を安定させる。
        const renderer::ResourceHandle<renderer::TextureTag> depthTex = sceneDepth;
        const renderer::ResourceHandle<renderer::TextureTag> colorTex =
            sceneColor.IsValid() ? sceneColor : blackTex;
        const renderer::ResourceHandle<renderer::TextureTag> rippleTex =
            rippleState.gpuTex.IsValid() ? rippleState.gpuTex : neutralRippleTex;

        const auto activeShader = (mat && !mat->shaderPath.empty())
            ? resources.LoadShader(mat->shaderPath)
            : waterShader;

        for (const WaterChunk& chunk : mesh.chunks) {
            /// @note 水面チャンクも地形と同様、独立にカリングされる描画候補として数える。
            ++ctx.statsTotalObjects;
            /// @note 地形チャンクと同様、カメラの Frustum Culling を切っている間は落とさない。
            math::Vector3 chunkMin = chunk.aabbMin;
            math::Vector3 chunkMax = chunk.aabbMax;
            ExpandByWaveMargin(margin, chunkMin, chunkMax);
            if (ctx.frustumCullingEnabled &&
                !AabbVisible(frustum, waterWorld, chunkMin, chunkMax)) {
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
            BindForwardShadingResources(call, ctx);
            call.textures[3]  = textures.foamMask;
            /// @note さざ波タイル (全水面で共有)
            call.textures[4]  = detailNoise.texture;
            call.textures[5]  = depthTex;
            call.textures[6]  = colorTex;
            call.textures[8]  = rippleTex;
            call.textures[9]  = shadowDepthTexture;
            /// @note TEX_IBL_PREFILTER: 空反射
            call.textures[17] = ctx.handles.iblPrefilter;
            /// @note TEX_VELOCITY_FIELD: 焼いた速度場のアトラス。場が 1 枚も無くても **必ず束縛する**
            ///       — DX12 の null ディスクリプタは Texture2D 固定で、Texture3D を宣言した
            ///       スロットを空にすると次元が食い違う。
            call.textures[26] = asset::VelocityFieldAtlas::Texture(resources);
            SubmitCounted(ctx, call);
        }
    }
}

} // namespace fbzz::scene
