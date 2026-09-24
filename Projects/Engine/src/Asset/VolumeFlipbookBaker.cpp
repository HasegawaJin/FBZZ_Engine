/// @file    VolumeFlipbookBaker.cpp
/// @brief   ボリューム → Flipbook / MV アトラスのベイク状態機械の実装。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/VolumeFlipbookBaker.hpp>

#include <Engine/Asset/BakeFingerprint.hpp>
#include <Engine/Asset/FluidRecipeCodec.hpp>
#include <Engine/Asset/FluidRenderMath.hpp>
#include <Fluid/FluidStepping.hpp>

#include "FlipbookImageIO.hpp"

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Util/EngineAssetPath.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Renderer/ColorTemperature.hpp>
#include <Engine/Renderer/ComputeCall.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/Format.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Math/Vector4.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

/// @note 焼きの入力は流体のレシピそのもの。この翻訳単位には fluid という名の局所変数
/// @note       (気体か液体かの判定) があり、fluid:: 修飾と食い違うのでここだけ名前を持ち込む。
using namespace fbzz::fluid;

namespace fbzz::asset {
namespace {

constexpr const char* kFillShaderPath = "Assets/Shaders/Bake/VolumeFlipbook/VolumeFill.cs.hlsl";
constexpr const char* kRaymarchShaderPath = "Assets/Shaders/Bake/VolumeFlipbook/VolumeRaymarch.hlsl";
constexpr std::uint32_t kMaxAtlasDimension = 16384;
constexpr std::size_t kMaxAtlasPixels = 16u * 1024u * 1024u;
constexpr float kDegreesToRadians = 3.14159265358979f / 180.0f;
/// @note タイル外周のこの幅 [px] に α がこれ以上あれば «縁にかかっている» とみなす。
constexpr std::uint32_t kEdgeBandPixels = 2;
constexpr float kEdgeAlphaThreshold = 0.02f;

constexpr std::uint32_t kDisplayRaw = 0;
constexpr std::uint32_t kDisplayColor = 1;
constexpr std::uint32_t kDisplayAlpha = 2;

/// @note ComputeCall::srvBuffers の添字 = レジスタ番号 (VolumeFill.cs.hlsl の gPuffs)。
constexpr std::size_t kPuffBufferSlot = 14;
/// @note VolumeUpload.cs.hlsl の gMediumIn / gVelocityIn (どちらも kComputeStructuredBufferSlots)。
constexpr std::size_t kFluidMediumSlot = 14;
constexpr std::size_t kFluidVelocitySlot = 15;
/// @note CPU の流体は上げすぎると 1 コマに数秒かかる (96³ ≈ 88 万セル)。GPU は 3D テクスチャの VRAM で決まる
/// @note (ソルバーが 13 枚 + ここで 2 枚。RGBA16F の 160³ で合計 ≈ 490 MB)。
constexpr int kMaxFluidResolution = 96;
constexpr int kMaxVolumeResolution = 160;
constexpr int kMaxSupersampling = 3;
/// @note RT に横へ並べるタイルの数: [色 | 速度 | 6-way Positive | 6-way Negative]。
constexpr std::uint32_t kRaymarchTiles = 6;
constexpr const char* kUploadShaderPath = "Assets/Shaders/Bake/VolumeFlipbook/VolumeUpload.cs.hlsl";

/// @note VolumeUpload.cs.hlsl の cbuffer と 1:1。
struct alignas(16) UploadConstants {
    std::uint32_t resolution;
    std::uint32_t pad[3];
};
static_assert(sizeof(math::Vector4) == 16, "VolumeUpload.cs.hlsl は float4 の StructuredBuffer として読む");

/// @note VolumeRaymarch.hlsl の cbuffer と 1:1。
struct alignas(16) RaymarchConstants {
    float camRight[3];   float halfExtent;
    float camUp[3];      std::uint32_t tileSize;
    float camForward[3]; std::uint32_t raySteps;
    float toLight[3];    std::uint32_t shadowSteps;
    float lightColor[3]; float extinction;
    float ambient[3];    float emissionIntensity;
    std::uint32_t displayMode;
    float anisotropy;
    float previewMotionScale;
    std::uint32_t background;
    float exposure;
    float liquidThreshold;
    float liquidSoftness;
    float liquidExtinction;
    float liquidSpecular;
    float liquidGloss;
    float liquidFresnelF0;
    float voxelSize;
    float albedoRamp[kVolumeRampStops][4];
    float emissionRamp[kVolumeRampStops][4];
    std::uint32_t sixWay;
    std::uint32_t octaves;
    std::uint32_t blackbody;
    std::uint32_t frameIndex;
    float skyOcclusion;
    float detailStrength;
    float detailScale;
    float detailPeriod;
    float time;
    float kelvinMin;
    float kelvinMax;
    std::uint32_t distortion;
    float distortionScale;
    std::uint32_t fireEmission;
    float fireEmissionExtinction;
    float distortionPad;
    float blackbodyLutMaxKelvin;
    float blackbodyLutPad[3];
    float blackbodyColorLut[kFluidFireColorLutSamples][4];
};
static_assert(sizeof(RaymarchConstants) == 4448, "VolumeRaymarch.hlsl の cbuffer と一致させること");

/// @note WIC (PNG の書き出し) は呼び出しスレッドで COM が初期化されている必要がある。
struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope()
    {
        if (SUCCEEDED(result)) CoUninitialize();
    }
};

void Store(float (&out)[3], const math::Vector3& v)
{
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

std::uint8_t ToUnorm8(float value)
{
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

/// @note 位置は [0,1] に収め、前の点より手前へ戻らないよう揃える (シェーダーは昇順を前提に区間を探す)。
float RampPosition(const VolumeColorRamp& ramp, int index, float previous)
{
    return (std::max)(previous, std::clamp(ramp.stops[static_cast<std::size_t>(index)].position, 0.0f, 1.0f));
}

void StoreRamp(float (&out)[kVolumeRampStops][4], const VolumeColorRamp& ramp)
{
    float previous = 0.0f;
    for (int i = 0; i < kVolumeRampStops; ++i) {
        const VolumeRampStop& stop = ramp.stops[static_cast<std::size_t>(i)];
        previous = RampPosition(ramp, i, previous);
        out[i][0] = (std::max)(stop.color.x, 0.0f);
        out[i][1] = (std::max)(stop.color.y, 0.0f);
        out[i][2] = (std::max)(stop.color.z, 0.0f);
        out[i][3] = previous;
    }
}

RaymarchConstants BuildRaymarchConstants(const VolumeFlipbookBakeSettings& settings, std::uint32_t tileSize,
                                          std::uint32_t volumeResolution, std::uint32_t displayMode,
                                          std::uint32_t background, float time, std::uint32_t frameIndex,
                                          const VolumeFlipbookCamera* cameraOverride,
                                          const FluidFireColorLut& fireColorLut)
{
    VolumeFlipbookCamera camera = ComputeVolumeFlipbookCamera(settings);
    if (cameraOverride != nullptr) {
        camera.right = cameraOverride->right;
        camera.up = cameraOverride->up;
        camera.forward = cameraOverride->forward;
    }
    RaymarchConstants constants{};
    Store(constants.camRight, camera.right);
    Store(constants.camUp, camera.up);
    Store(constants.camForward, camera.forward);
    Store(constants.toLight, camera.toLight);
    Store(constants.lightColor, settings.lightColor);
    Store(constants.ambient, settings.ambient);
    StoreRamp(constants.albedoRamp, settings.albedoRamp);
    StoreRamp(constants.emissionRamp, settings.emissionRamp);
    constants.liquidThreshold = (std::max)(settings.liquid.threshold, 0.0f);
    /// @note 0 だと smoothstep の両端が一致して割り算が壊れる。
    constants.liquidSoftness = (std::max)(settings.liquid.softness, 0.005f);
    constants.liquidExtinction = (std::max)(settings.liquid.extinction, 0.0f);
    constants.liquidSpecular = (std::max)(settings.liquid.specular, 0.0f);
    constants.liquidGloss = std::clamp(settings.liquid.gloss, 1.0f, 2048.0f);
    constants.liquidFresnelF0 = std::clamp(settings.liquid.fresnelF0, 0.0f, 1.0f);
    constants.voxelSize = 2.0f / static_cast<float>((std::max)(volumeResolution, 1u));
    constants.halfExtent = (std::max)(settings.halfExtent, 0.05f);
    constants.tileSize = tileSize;
    constants.raySteps = static_cast<std::uint32_t>(std::clamp(settings.raySteps, 8, 512));
    constants.shadowSteps = static_cast<std::uint32_t>(std::clamp(settings.shadowSteps, 1, 64));
    constants.extinction = (std::max)(settings.extinction, 0.0f);
    constants.emissionIntensity = (std::max)(settings.emissionIntensity, 0.0f);
    constants.exposure = (std::max)(settings.exposure, 1.0e-3f);
    constants.displayMode = displayMode;
    constants.anisotropy = std::clamp(settings.anisotropy, -0.95f, 0.95f);
    /// @note 1 秒でタイルの 1/4 動く速さを色の振り切りにする。見て分かる程度の目安でよい。
    constants.previewMotionScale = 2.0f;
    constants.background = background;
    constants.sixWay = settings.sixWayLightmaps && !settings.distortion ? 1u : 0u;
    constants.distortion = settings.distortion ? 1u : 0u;
    constants.distortionScale = (std::max)(settings.distortionScale, 0.0f);
    constants.fireEmission = settings.fireEmission ? 1u : 0u;
    constants.fireEmissionExtinction = (std::max)(settings.fireEmissionExtinction, 0.0f);
    constants.octaves = static_cast<std::uint32_t>(std::clamp(settings.scatteringOctaves, 1, 8));
    constants.blackbody = settings.blackbodyEmission ? 1u : 0u;
    constants.frameIndex = frameIndex;
    constants.skyOcclusion = std::clamp(settings.skyOcclusion, 0.0f, 1.0f);
    constants.detailStrength = (std::max)(settings.detailStrength, 0.0f);
    constants.detailScale = (std::max)(settings.detailScale, 0.1f);
    constants.detailPeriod = (std::max)(settings.detailPeriod, 0.05f);
    constants.time = time;
    constants.kelvinMin = std::clamp(settings.blackbodyMinKelvin, 500.0f, 15000.0f);
    constants.kelvinMax = (std::max)(std::clamp(settings.blackbodyMaxKelvin, 500.0f, 15000.0f), constants.kelvinMin);
    constants.blackbodyLutMaxKelvin = fireColorLut.MaxKelvin();
    for (std::size_t i = 0; i < kFluidFireColorLutSamples; ++i) {
        const math::Vector3& chroma = fireColorLut.Samples()[i];
        constants.blackbodyColorLut[i][0] = chroma.x;
        constants.blackbodyColorLut[i][1] = chroma.y;
        constants.blackbodyColorLut[i][2] = chroma.z;
        constants.blackbodyColorLut[i][3] = 0.0f;
    }
    return constants;
}

math::Vector4 Lerp4(const math::Vector4& from, const math::Vector4& to, float t)
{
    return { from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, from.z + (to.z - from.z) * t,
             from.w + (to.w - from.w) * t };
}

/// @note xyz が «w の覆いの中身» の値。覆いの無い側の値 (空の場所の 0) を持ち込まないよう、覆いで重み付けする。
math::Vector4 LerpStraight(const math::Vector4& from, const math::Vector4& to, float t)
{
    const float fromWeight = from.w * (1.0f - t);
    const float toWeight = to.w * t;
    const float coverage = fromWeight + toWeight;
    if (coverage <= 1.0e-6f) return Lerp4(from, to, t);
    const float inverse = 1.0f / coverage;
    return { (from.x * fromWeight + to.x * toWeight) * inverse, (from.y * fromWeight + to.y * toWeight) * inverse,
             (from.z * fromWeight + to.z * toWeight) * inverse, coverage };
}

/// @note GPU のソルバー (気体 / 液体) を frame コマ目まで追いつかせる。1 Tick に進められる分だけ進め、届いたら true。
template <typename Solver>
bool CatchUpGpuSolver(Solver& solver, renderer::IRenderer& renderer, renderer::ResourceManager& resources, int frame)
{
    solver.BeginTick();
    while (solver.SolvedFrame() < frame && solver.StepFrame(renderer, resources)) {}
    return solver.SolvedFrame() >= frame;
}

bool LoadRecipeByPath(const std::string& path, FluidRecipe& recipe, std::string& outError)
{
    const std::string file = path.empty() ? std::string{} : AssetManager::ResolveAssetPath(path);
    std::string loadError;
    if (file.empty() || !LoadFluidRecipe(file, recipe, &loadError)) {
        outError = ".fluid を読み込めません: " + path;
        if (!loadError.empty()) outError += " (" + loadError + ")";
        return false;
    }
    return true;
}

bool LoadBakeRecipe(const VolumeFlipbookBakeSettings& settings, FluidRecipe& recipe, std::string& outError)
{
    return LoadRecipeByPath(settings.fluidRecipePath, recipe, outError);
}

}

math::Vector3 EvaluateVolumeRamp(const VolumeColorRamp& ramp, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    float previous = RampPosition(ramp, 0, 0.0f);
    if (t <= previous) return ramp.stops[0].color;
    for (int i = 1; i < kVolumeRampStops; ++i) {
        const float position = RampPosition(ramp, i, previous);
        if (t <= position) {
            const float f = std::clamp((t - previous) / (std::max)(position - previous, 1.0e-5f), 0.0f, 1.0f);
            return math::Vector3::Lerp(ramp.stops[static_cast<std::size_t>(i - 1)].color,
                                       ramp.stops[static_cast<std::size_t>(i)].color, f);
        }
        previous = position;
    }
    return ramp.stops[kVolumeRampStops - 1].color;
}

VolumeColorRamp EvenVolumeRamp(const math::Vector3& c0, const math::Vector3& c1,
                               const math::Vector3& c2, const math::Vector3& c3)
{
    VolumeColorRamp ramp;
    ramp.stops = { VolumeRampStop{ c0, 0.0f }, VolumeRampStop{ c1, 1.0f / 3.0f },
                   VolumeRampStop{ c2, 2.0f / 3.0f }, VolumeRampStop{ c3, 1.0f } };
    return ramp;
}

VolumeColorRamp UniformVolumeRamp(const math::Vector3& color)
{
    return EvenVolumeRamp(color, color, color, color);
}

VolumeColorRamp DefaultFireRamp()
{
    return EvenVolumeRamp({ 0.0f, 0.0f, 0.0f }, { 0.9f, 0.08f, 0.01f }, { 1.0f, 0.45f, 0.06f }, { 1.0f, 0.9f, 0.6f });
}

VolumeFlipbookCamera ComputeVolumeFlipbookCamera(const VolumeFlipbookBakeSettings& settings)
{
    /// @note DirectX の左手系: yaw 0 で +Z を向き、+X が画面右。
    const float yaw = settings.cameraYawDegrees * kDegreesToRadians;
    const float lightYaw = settings.lightYawDegrees * kDegreesToRadians;
    const float lightPitch = settings.lightPitchDegrees * kDegreesToRadians;

    VolumeFlipbookCamera camera;
    camera.forward = { std::sin(yaw), 0.0f, std::cos(yaw) };
    camera.up = math::Vector3::UP;
    camera.right = math::Vector3::Cross(math::Vector3::UP, camera.forward);
    camera.toLight = math::Vector3{ std::cos(lightPitch) * std::sin(lightYaw), std::sin(lightPitch),
                                    -std::cos(lightPitch) * std::cos(lightYaw) }
                         .NormalizedOr(math::Vector3::UP);
    return camera;
}

bool VolumeBakeLoops(const VolumeFlipbookBakeSettings& settings)
{
    if (settings.sourceKind == VolumeSourceKind::Analytic) return VolumeSourceLoops(settings.source);
    return settings.fluidLoop;
}

int VolumeLoopOverlapFrames(const VolumeFlipbookBakeSettings& settings)
{
    if (settings.sourceKind != VolumeSourceKind::Fluid || !settings.fluidLoop) return 0;
    /// @note Begin と同じ範囲へ丸めてから数える (焼く前の見積もりと実際の焼きで数がずれないように)。
    const int frames = std::clamp(settings.source.frameCount, 2, 256);
    return fluid::FluidLoopOverlapFrames(frames, true, settings.fluidLoopBlendFraction);
}

float VolumeLoopKeepWeight(int index, int overlap)
{
    if (overlap <= 0) return 1.0f;
    return std::clamp(static_cast<float>(index + 1) / static_cast<float>(overlap + 1), 0.0f, 1.0f);
}

math::Vector4 EncodeVolumeDistortion(math::Vector2 screenVelocity, float coverage, float scale)
{
    return EncodeFluidDistortion(screenVelocity, coverage, scale);
}

VolumeFramingReport AnalyzeVolumeFraming(const VolumeFlipbookBakeSettings& settings)
{
    VolumeFramingReport report;
    /// @note 流体の形は解くまで分からない。構図はプレビューで確かめてもらう。
    if (settings.sourceKind == VolumeSourceKind::Fluid) {
        report.frameIssues.assign(static_cast<std::size_t>(std::clamp(settings.source.frameCount, 1, 256)), 0);
        return report;
    }
    const int frames = std::clamp(settings.source.frameCount, 1, 256);
    const float frameDt = (std::max)(settings.source.frameDt, 1.0e-4f);
    const float halfExtent = (std::max)(settings.halfExtent, 0.05f);
    const VolumeFlipbookCamera camera = ComputeVolumeFlipbookCamera(settings);
    const std::vector<VolumePuff> puffs = BuildVolumePuffs(settings.source);
    const float start = ResolveVolumeStartTime(settings.source);

    report.frameIssues.assign(static_cast<std::size_t>(frames), 0);
    for (int frame = 0; frame < frames; ++frame) {
        const float time = start + static_cast<float>(frame) * frameDt;
        std::uint8_t issues = 0;
        const std::uint32_t live = CountLiveVolumePuffs(puffs, time);
        report.maxLivePuffs = (std::max)(report.maxLivePuffs, live);
        if (live > kVolumeFillMaxPuffs) issues |= kFramingTooManyPuffs;
        for (const VolumeBound& bound : CollectVisibleVolumeBounds(puffs, settings.noise, time)) {
            const math::Vector3& c = bound.center;
            const float r = bound.radius;
            if ((std::max)({ std::fabs(c.x), std::fabs(c.y), std::fabs(c.z) }) + r > 1.0f)
                issues |= kFramingCutByVolumeBox;
            const float screenX = std::fabs(math::Vector3::Dot(c, camera.right));
            const float screenY = std::fabs(math::Vector3::Dot(c, camera.up));
            if ((std::max)(screenX, screenY) + r > halfExtent)
                issues |= kFramingCutByTileEdge;
        }
        report.frameIssues[static_cast<std::size_t>(frame)] = issues;
        if (issues & kFramingCutByVolumeBox) ++report.boxCutFrames;
        if (issues & kFramingCutByTileEdge) ++report.tileCutFrames;
        if (issues & kFramingTooManyPuffs) ++report.overflowFrames;
    }
    return report;
}

bool VolumeFlipbookBaker::EnsureGpu(renderer::ResourceManager& resources, std::uint32_t volumeResolution,
                                    std::uint32_t tileSize, std::string& outError)
{
    /// @note デバイスリセット後の古いハンドルは返せない (実体ごと消えている)。捨てて作り直す。
    if (m_resetVersion != resources.GetResetVersion()) {
        m_fillShader = {};
        m_raymarchShader = {};
        m_pipeline = {};
        m_fillConstants = {};
        m_raymarchConstants = {};
        m_puffBuffers = {};
        m_puffRing = 0;
        m_uploadShader = {};
        m_uploadConstants = {};
        m_fluidMediumBuffers = {};
        m_fluidVelocityBuffers = {};
        m_fluidRing = 0;
        m_fluidResolution = 0;
        m_medium = {};
        m_velocity = {};
        m_target = {};
        m_volumeResolution = 0;
        m_tileSize = 0;
        m_previewTile = 0;
        m_resetVersion = resources.GetResetVersion();
    }

    if (!m_fillShader.IsValid())
        m_fillShader = resources.LoadShader(util::FileSystem::PathToUtf8(util::ResolveEngineAssetPath(kFillShaderPath)));
    if (!m_raymarchShader.IsValid())
        m_raymarchShader = resources.LoadShader(util::FileSystem::PathToUtf8(util::ResolveEngineAssetPath(kRaymarchShaderPath)));
    if (!m_fillShader.IsValid() || !m_raymarchShader.IsValid()) {
        outError = "ベイク用シェーダーを読み込めません (Assets/Shaders/Bake/VolumeFlipbook)";
        return false;
    }
    if (!m_pipeline.IsValid()) {
        m_pipeline = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL,
                                                     renderer::BlendMode::OPAQUE_BLEND,
                                                     renderer::DepthMode::DEPTH_OFF });
    }
    if (!m_fillConstants.IsValid())
        m_fillConstants = resources.CreateConstantBuffer(sizeof(VolumeFillHeader));
    if (!m_raymarchConstants.IsValid())
        m_raymarchConstants = resources.CreateConstantBuffer(sizeof(RaymarchConstants));
    for (auto& buffer : m_puffBuffers) {
        if (!buffer.IsValid())
            buffer = resources.CreateStructuredBuffer(nullptr, kVolumeFillMaxPuffs, sizeof(VolumeFillPuffGpu));
        if (!buffer.IsValid()) {
            outError = "puff 用の StructuredBuffer を作れません";
            return false;
        }
    }

    if (m_volumeResolution != volumeResolution) {
        if (m_medium.IsValid()) resources.Release(m_medium);
        if (m_velocity.IsValid()) resources.Release(m_velocity);
        m_medium = resources.CreateComputeTexture3D(volumeResolution, volumeResolution, volumeResolution);
        m_velocity = resources.CreateComputeTexture3D(volumeResolution, volumeResolution, volumeResolution);
        m_volumeResolution = volumeResolution;
    }
    if (!m_medium.IsValid() || !m_velocity.IsValid()) {
        m_volumeResolution = 0;
        outError = "このバックエンドは 3D の書き込み可能テクスチャに対応していません";
        return false;
    }

    if (m_tileSize != tileSize) {
        if (m_target.IsValid()) resources.Release(m_target);
        m_target = resources.CreateRenderTarget(tileSize * kRaymarchTiles, tileSize,
                                                renderer::RenderTargetDesc{ 1, renderer::Format::RGBA16F, false });
        m_tileSize = tileSize;
        m_previewTile = 0;
    }
    if (!m_target.IsValid()) {
        m_tileSize = 0;
        outError = "ベイク用のレンダーターゲットを作れません";
        return false;
    }
    return true;
}

bool VolumeFlipbookBaker::Begin(const VolumeFlipbookBakeSettings& settings,
                                renderer::ResourceManager& resources, std::string& outError)
{
    if (IsBusy()) {
        outError = "ベイク中です";
        return false;
    }
    /// @note 焼きとプレビューで CPU のソルバーが 2 本同時に回ると、焼きが取り分を奪われて倍近く遅くなる。
    m_fluidPreview.Cancel();
    m_settings = settings;
    m_settings.source.frameCount = std::clamp(settings.source.frameCount, 2, 256);
    m_settings.source.frameDt = std::clamp(settings.source.frameDt, 1.0f / 240.0f, 1.0f);
    m_settings.volumeResolution = std::clamp(settings.volumeResolution, 16, kMaxVolumeResolution);
    m_settings.tileSize = std::clamp(settings.tileSize, 32, 1024);
    if (m_settings.outputDirectory.empty()) {
        outError = "出力先がありません";
        return false;
    }
    const bool fluid = m_settings.sourceKind == VolumeSourceKind::Fluid;
    /// @note 歪みマップは色の代わりに焼くもので、6 方向の陰影は意味を持たない。
    if (m_settings.distortion) m_settings.sixWayLightmaps = false;
    FluidRecipe recipe;
    if (fluid && !LoadBakeRecipe(m_settings, recipe, outError)) return false;
    const bool liquid = fluid && recipe.kind == FluidKind::Liquid;
    m_bakeUsesGpu = false;
    m_bakeGpuLiquid = false;
    m_gpuFallbackNote.clear();
    if (fluid && m_settings.fluidSolver != VolumeFluidSolver::Cpu) {
        std::string gpuError;
        const bool ready = liquid
            ? m_liquidGpuBake.Initialize(resources, recipe, m_settings.volumeResolution, m_settings.source.frameDt,
                                         recipe.render.liquidRadiusScale, gpuError)
            : m_fluidGpuBake.Initialize(resources, recipe, m_settings.volumeResolution, m_settings.source.frameDt,
                                        m_settings.fluidDensityScale, gpuError);
        if (ready) {
            m_bakeUsesGpu = true;
            m_bakeGpuLiquid = liquid;
            /// @note 焼いている間はプレビュー用の格子と粒子を返す (160³ だと 2 つで 600 MB 近くになる)。
            m_fluidGpuPreview.Release(resources);
            m_liquidGpuPreview.Release(resources);
            m_previewOpen.reset();
            m_previewFailed.reset();
            m_previewResolution = 0;
            m_previewUsesGpu = false;
            m_previewGpuLiquid = false;
        } else {
            if (liquid) m_liquidGpuBake.Release(resources);
            else        m_fluidGpuBake.Release(resources);
            const std::string name = liquid ? "GPU の液体ソルバー" : "GPU の気体ソルバー";
            const std::string reason = gpuError.empty() ? std::string("理由不明") : gpuError;
            /// @note solver = "gpu" は «GPU で解けたときだけ焼く» という指定。黙って CPU へ落とすと、
            /// @note       同じ .fluid が環境によって別の絵になり、指紋も食い違う。落としてよいのは auto だけ。
            if (m_settings.fluidSolver == VolumeFluidSolver::Gpu) {
                outError = name + "を初期化できません (" + reason
                    + ")。CPU で焼いてよければ [bake] solver を \"auto\" にしてください";
                return false;
            }
            m_gpuFallbackNote = name + "を使えないため CPU で解きました (" + reason + ")";
        }
    }
    const bool gpuFluid = m_bakeUsesGpu;
    if (fluid && !gpuFluid) {
        m_settings.volumeResolution = (std::min)(m_settings.volumeResolution, kMaxFluidResolution);
        if (!m_fluidBake.Open(recipe, m_settings.volumeResolution, m_settings.source.frameDt,
                              m_settings.fluidDensityScale, outError))
            return false;
    } else if (!fluid && FindVolumeSource(m_settings.source.preset) == nullptr) {
        outError = "ソースが登録されていません: " + m_settings.source.preset;
        return false;
    }

    m_grid = ComputeFlipbookGrid(m_settings.source.frameCount, m_settings.columns);
    const auto tile = static_cast<std::uint32_t>(m_settings.tileSize);
    m_atlasWidth = tile * static_cast<std::uint32_t>(m_grid.columns);
    m_atlasHeight = tile * static_cast<std::uint32_t>(m_grid.rows);
    if (m_atlasWidth > kMaxAtlasDimension || m_atlasHeight > kMaxAtlasDimension
        || static_cast<std::size_t>(m_atlasWidth) * m_atlasHeight > kMaxAtlasPixels) {
        outError = "Atlas が大きすぎます (" + std::to_string(m_atlasWidth) + "x" + std::to_string(m_atlasHeight)
            + "。最大 16384px/辺かつ合計 16M pixels)";
        return false;
    }
    /// @note 縮める前の RT は 4 タイル並びなので、横幅が上限を超えない倍率までに抑える。
    m_supersampling = static_cast<std::uint32_t>(std::clamp(m_settings.supersampling, 1, kMaxSupersampling));
    while (m_supersampling > 1 && tile * m_supersampling * kRaymarchTiles > kMaxAtlasDimension) --m_supersampling;
    m_outputTile = tile;
    if (!EnsureGpu(resources, static_cast<std::uint32_t>(m_settings.volumeResolution), tile * m_supersampling, outError))
        return false;
    if (fluid && !gpuFluid
        && !EnsureFluidGpu(resources, static_cast<std::uint32_t>(m_settings.volumeResolution), outError))
        return false;

    const std::size_t pixelCount = static_cast<std::size_t>(m_atlasWidth) * m_atlasHeight;
    m_colorAtlas.assign(pixelCount * 4, 0);
    if (m_settings.distortion) {
        m_motion = {};
        m_coverage = {};
    } else {
        m_motion.assign(pixelCount, math::Vector2::ZERO);
        m_coverage.assign(pixelCount, 0.0f);
    }
    m_loopOverlap = VolumeLoopOverlapFrames(m_settings);
    m_loopHead.clear();
    m_loopHead.resize(static_cast<std::size_t>(m_loopOverlap));
    if (fluid) m_puffs.clear();
    else       m_puffs = BuildVolumePuffs(m_settings.source);
    if (m_settings.sixWayLightmaps) {
        m_sixWayPositive.assign(pixelCount * 4, 0);
        m_sixWayNegative.assign(pixelCount * 4, 0);
        m_sixWayAlbedoColor.assign(pixelCount * 4, 0);
        m_sixWayEmissionColor.assign(pixelCount * 4, 0);
    } else {
        m_sixWayPositive = {};
        m_sixWayNegative = {};
        m_sixWayAlbedoColor = {};
        m_sixWayEmissionColor = {};
    }
    m_clippedTexels = 0;
    m_edgeTouchFrames = 0;
    m_frameIndex = 0;
    m_result = {};
    m_baked = {};
    m_hasBaked = false;
    /// @note 0 コマ目を裏で解き始める。Tick は解けたコマだけを記録する。
    if (fluid && !gpuFluid) m_fluidBake.Request(0);
    m_motionBytes = {};
    m_previewTile = 0;
    m_state = VolumeFlipbookBakeState::Recording;
    return true;
}

void VolumeFlipbookBaker::RecordFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                                       const VolumeFlipbookBakeSettings& settings,
                                       const std::vector<VolumePuff>& puffs, float time,
                                       std::uint32_t displayMode, std::uint32_t background,
                                       const VolumeFlipbookCamera* cameraOverride)
{
    PackVolumeFill(puffs, settings.noise, m_volumeResolution, time, settings.source.frameDt, m_fill);
    resources.Update(m_fillConstants, &m_fill.header, sizeof(m_fill.header));
    const auto puffBuffer = m_puffBuffers[m_puffRing];
    m_puffRing = (m_puffRing + 1) % kPuffBufferRing;
    if (!m_fill.puffs.empty())
        resources.Update(puffBuffer, m_fill.puffs.data(), m_fill.puffs.size() * sizeof(VolumeFillPuffGpu));

    renderer::ComputeCall compute;
    compute.shader = m_fillShader;
    compute.constantBuffers[0] = m_fillConstants;
    compute.srvBuffers[kPuffBufferSlot] = puffBuffer;
    compute.uavOutputs[0] = m_medium;
    compute.uavOutputs[1] = m_velocity;
    compute.dispatchX = compute.dispatchY = compute.dispatchZ = (m_volumeResolution + 3) / 4;
    renderer.Dispatch(compute, resources);

    RecordRaymarch(renderer, resources, settings, displayMode, background, time, cameraOverride);
}

void VolumeFlipbookBaker::RecordRaymarch(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                                           const VolumeFlipbookBakeSettings& settings, std::uint32_t displayMode,
                                           std::uint32_t background, float time,
                                           const VolumeFlipbookCamera* cameraOverride)
{
    const float requestedLutMaxKelvin = settings.blackbodyLutMaxKelvin > 0.0f
        ? settings.blackbodyLutMaxKelvin
        : (std::max)(std::clamp(settings.blackbodyMaxKelvin, 500.0f, 15000.0f), 500.0f) * 4.0f;
    if (std::fabs(m_fireColorLut.MaxKelvin() - requestedLutMaxKelvin) > 1.0e-3f)
        m_fireColorLut.Reset(requestedLutMaxKelvin);
    const RaymarchConstants raymarch = BuildRaymarchConstants(settings, m_tileSize, m_volumeResolution, displayMode,
                                                              background, time, m_jitterFrame++, cameraOverride,
                                                              m_fireColorLut);
    resources.Update(m_raymarchConstants, &raymarch, sizeof(raymarch));

    renderer.SetRenderTarget(m_target, resources);
    renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
    renderer::DrawCall draw;
    draw.shader = m_raymarchShader;
    draw.pipelineState = m_pipeline;
    draw.vertexCount = 3;
    draw.constantBuffers[0] = m_raymarchConstants;
    draw.textures[0] = m_medium;
    draw.textures[1] = m_velocity;
    renderer.Submit(draw, resources);
    /// @note DX12 は別の RT へ切り替えたときに初めて RT をシェーダー読み取り状態へ戻す。
    /// @note       読み戻しと ImGui 表示はその状態を前提にしている。
    renderer.SetRenderTarget({}, resources);
}

bool VolumeFlipbookBaker::EnsureFluidGpu(renderer::ResourceManager& resources, std::uint32_t resolution,
                                         std::string& outError)
{
    if (!m_uploadShader.IsValid())
        m_uploadShader = resources.LoadShader(util::FileSystem::PathToUtf8(util::ResolveEngineAssetPath(kUploadShaderPath)));
    if (!m_uploadConstants.IsValid())
        m_uploadConstants = resources.CreateConstantBuffer(sizeof(UploadConstants));
    if (!m_uploadShader.IsValid() || !m_uploadConstants.IsValid()) {
        outError = "流体の転送シェーダーを読み込めません (Assets/Shaders/Bake/VolumeFlipbook/VolumeUpload.cs.hlsl)";
        return false;
    }
    if (m_fluidResolution != resolution) {
        for (auto* ring : { &m_fluidMediumBuffers, &m_fluidVelocityBuffers }) {
            for (auto& buffer : *ring) {
                if (buffer.IsValid()) resources.Release(buffer);
                buffer = {};
            }
        }
        m_fluidResolution = 0;
    }
    const std::uint32_t cells = resolution * resolution * resolution;
    for (std::size_t i = 0; i < kPuffBufferRing; ++i) {
        if (!m_fluidMediumBuffers[i].IsValid())
            m_fluidMediumBuffers[i] = resources.CreateStructuredBuffer(nullptr, cells, sizeof(math::Vector4));
        if (!m_fluidVelocityBuffers[i].IsValid())
            m_fluidVelocityBuffers[i] = resources.CreateStructuredBuffer(nullptr, cells, sizeof(math::Vector4));
        if (!m_fluidMediumBuffers[i].IsValid() || !m_fluidVelocityBuffers[i].IsValid()) {
            outError = "流体用の StructuredBuffer を作れません";
            return false;
        }
    }
    m_fluidResolution = resolution;
    return true;
}

void VolumeFlipbookBaker::ReleaseFluidGpu(renderer::ResourceManager& resources)
{
    /// @note デバイスリセット後のハンドルは既に実体が無い。返しに行くと別のリソースを消しかねない。
    if (m_resetVersion == resources.GetResetVersion()) {
        for (auto* ring : { &m_fluidMediumBuffers, &m_fluidVelocityBuffers })
            for (const auto& buffer : *ring)
                if (buffer.IsValid()) resources.Release(buffer);
        if (m_uploadConstants.IsValid()) resources.Release(m_uploadConstants);
    }
    m_fluidMediumBuffers = {};
    m_fluidVelocityBuffers = {};
    m_fluidRing = 0;
    m_fluidResolution = 0;
    m_uploadConstants = {};
    m_uploadShader = {};
}

void VolumeFlipbookBaker::RecordFluidFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                                            const VolumeFlipbookBakeSettings& settings, const PackedFluidVolume& volume,
                                            std::uint32_t displayMode, std::uint32_t background, float time,
                                            const VolumeFlipbookCamera* cameraOverride)
{
    const auto resolution = static_cast<std::uint32_t>((std::max)(volume.resolution, 0));
    const std::size_t cells = static_cast<std::size_t>(resolution) * resolution * resolution;
    if (resolution == 0 || resolution != m_volumeResolution || resolution != m_fluidResolution
        || volume.medium.size() != cells || volume.velocity.size() != cells)
        return;

    const UploadConstants constants{ resolution, { 0u, 0u, 0u } };
    resources.Update(m_uploadConstants, &constants, sizeof(constants));
    const auto medium = m_fluidMediumBuffers[m_fluidRing];
    const auto velocity = m_fluidVelocityBuffers[m_fluidRing];
    m_fluidRing = (m_fluidRing + 1) % kPuffBufferRing;
    resources.Update(medium, volume.medium.data(), cells * sizeof(math::Vector4));
    resources.Update(velocity, volume.velocity.data(), cells * sizeof(math::Vector4));

    renderer::ComputeCall compute;
    compute.shader = m_uploadShader;
    compute.constantBuffers[0] = m_uploadConstants;
    compute.srvBuffers[kFluidMediumSlot] = medium;
    compute.srvBuffers[kFluidVelocitySlot] = velocity;
    compute.uavOutputs[0] = m_medium;
    compute.uavOutputs[1] = m_velocity;
    compute.dispatchX = compute.dispatchY = compute.dispatchZ = (resolution + 3) / 4;
    renderer.Dispatch(compute, resources);
    RecordRaymarch(renderer, resources, settings, displayMode, background, time, cameraOverride);
}

void VolumeFlipbookBaker::RecordPreview(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                                        const VolumeFlipbookBakeSettings& settings, float time,
                                        const VolumePreviewOptions& options)
{
    /// @note ディスクの .fluid を見るこれまでの呼び出し。版数 0 = «編集中の中身は無い»。
    RecordPreview(renderer, resources, settings, time, options, nullptr, 0);
}

void VolumeFlipbookBaker::RecordPreview(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                                        const VolumeFlipbookBakeSettings& settings, float time,
                                        const VolumePreviewOptions& options, const FluidRecipe* recipe,
                                        std::uint64_t recipeRevision)
{
    if (IsBusy()) return;
    std::string error;
    const auto fail = [&](const std::string& message) {
        m_result = {};
        m_result.message = message;
        m_previewPending = false;
        m_previewTile = 0;
    };
    const auto tile = static_cast<std::uint32_t>(std::clamp(settings.tileSize, 32, 1024));
    const std::uint32_t displayMode = options.view == VolumePreviewView::Alpha ? kDisplayAlpha : kDisplayColor;
    const auto background = static_cast<std::uint32_t>(options.background);
    const VolumeFlipbookCamera* cameraOverride = options.cameraOverride.has_value() ? &*options.cameraOverride : nullptr;
    if (settings.sourceKind != VolumeSourceKind::Fluid) {
        m_previewPending = false;
        const auto resolution = static_cast<std::uint32_t>(std::clamp(settings.volumeResolution, 16, kMaxVolumeResolution));
        if (!EnsureGpu(resources, resolution, tile, error)) {
            fail(error);
            return;
        }
        const std::vector<VolumePuff> puffs = BuildVolumePuffs(settings.source);
        RecordFrame(renderer, resources, settings, puffs, ResolveVolumeStartTime(settings.source) + time,
                    displayMode, background, cameraOverride);
        m_previewTile = tile;
        return;
    }

    /// @note 要求を書く前に解けたコマを受け取る。FluidVolumeStream::Busy() は結果を受け取るまで
    /// @note       下がらないため、ここで受け取らないと下の «解いている間は開き直しを待つ» が
    /// @note       いつまでも解けなくなる。
    PackedFluidVolume polled;
    if (m_fluidPreview.Poll(polled)) m_fluidPreviewVolume = std::move(polled);

    /// @note 解き直しが要る設定 (解き方・レシピ・解像度・コマの間隔・濃さ) が変わったら開き直す。
    /// @note       ここは «要求を書く» だけで、実際に開き直すのは ApplyPendingPreviewSwitch。開き直しは
    /// @note       «閉じて解き直す» なので、当てる瞬間を呼び手 (再生の切れ目) が選べないと絵が途中で
    /// @note       飛ぶため分ける。当てられるのは焼いていない・CPU の解きが畳まれている・ゲートが
    /// @note       開いているときだけで、それ以外のフレームは前のソルバーのコマを出したまま次へ回す。
    FluidPreviewKey key;
    key.solver = settings.fluidSolver;
    key.path = settings.fluidRecipePath;
    key.resolution = settings.volumeResolution;
    key.frameDt = settings.source.frameDt;
    key.densityScale = settings.fluidDensityScale;
    /// @note ディスクのレシピを見る呼び出し (recipe = nullptr) の鍵を変えないよう、渡されたときだけ
    /// @note       版数を混ぜる。開き直す頻度は «今編集している中身» を見ているときだけ上がる。
    key.revision = recipe != nullptr ? recipeRevision : 0;
    m_previewRequested = key;

    if (HasPendingPreviewSwitch()) {
        /// @note 何も開いていないなら «前のソルバーの絵» が無いので、ゲートに関わらず今開く。
        if (m_previewSwitchAllowed || !m_previewOpen.has_value()) {
            /// @note Close() は走っている CPU の解きが終わるまで止まる。スライダーを掴んでいる間は
            /// @note       毎フレーム版数が変わるので、そのたびに 1 コマ分 (数百 ms) 待つとエディターごと
            /// @note       固まるため、待たずに畳むよう伝えるだけにして前のコマを映したまま次へ回す。
            if (m_fluidPreview.Busy()) {
                m_fluidPreview.Cancel();
                m_previewPending = true;
                return;
            }
            if (!ApplyPendingPreviewSwitch(resources, recipe, error)) {
                m_previewFailed = key;
                fail(error);
                return;
            }
            m_previewFailed.reset();
        }
    }
    if (!m_previewOpen.has_value() || m_previewResolution == 0) {
        m_previewPending = false;
        return;
    }
    /// @note 開いているものの刻みで数える。切り替え待ちの間は要求側の値がもう別物になっている。
    const float frameDt = (std::max)(m_previewOpen->frameDt, 1.0e-4f);
    if (!EnsureGpu(resources, m_previewResolution, tile, error)
        || (!m_previewUsesGpu && !EnsureFluidGpu(resources, m_previewResolution, error))) {
        fail(error);
        return;
    }

    const int frame = std::clamp(static_cast<int>(time / frameDt), 0, (std::max)(settings.source.frameCount, 1) - 1);
    if (m_previewUsesGpu) {
        const auto drive = [&](auto& solver) {
            if (!solver.IsReady()) {
                m_previewPending = false;
                return;
            }
            /// @note GPU は速いので、手前へ戻るときは最初から解き直す (1 Tick に数コマずつ追いつく)。
            if (frame < solver.SolvedFrame()) solver.Restart();
            m_previewPending = !CatchUpGpuSolver(solver, renderer, resources, frame);
            /// @note ソルバーの格子と書き込み先のボリュームは別々に作る。食い違ったまま書くと範囲外になる
            /// @note       (CPU 経路の RecordFluidFrame は同じ確認を持っている。GPU 経路にだけ無かった)。
            if (solver.Resolution() != static_cast<int>(m_volumeResolution)) {
                m_previewPending = false;
                return;
            }
            if (solver.SolvedFrame() >= 0) {
                solver.WriteVolumes(renderer, resources, m_medium, m_velocity);
                RecordRaymarch(renderer, resources, settings, displayMode, background,
                               static_cast<float>(solver.SolvedFrame()) * frameDt, cameraOverride);
                m_previewTile = tile;
                m_previewStale = false;
            }
        };
        if (m_previewGpuLiquid) drive(m_liquidGpuPreview);
        else                    drive(m_fluidGpuPreview);
        return;
    }
    if (!m_fluidPreview.Busy() && m_fluidPreviewVolume.frame != frame) m_fluidPreview.Request(frame);
    m_previewPending = m_fluidPreview.Busy();
    if (m_fluidPreviewVolume.frame >= 0) {
        RecordFluidFrame(renderer, resources, settings, m_fluidPreviewVolume, displayMode, background,
                         static_cast<float>(m_fluidPreviewVolume.frame) * frameDt, cameraOverride);
        m_previewTile = tile;
        m_previewStale = false;
    }
}

bool VolumeFlipbookBaker::ApplyPendingPreviewSwitch(renderer::ResourceManager& resources, const FluidRecipe* recipe,
                                                    std::string& outError)
{
    if (!m_previewRequested.has_value()) return true;
    const FluidPreviewKey key = *m_previewRequested;

    /// @note 開き直しの途中で失敗しても «前のものが開いたまま» には戻せない。先に «何も開いていない» へ倒し、
    /// @note       最後まで通ったときだけ m_previewOpen を書く。
    m_previewOpen.reset();
    m_fluidPreview.Close();
    m_fluidPreviewVolume = {};
    m_previewResolution = 0;
    m_previewUsesGpu = false;
    m_previewGpuLiquid = false;
    m_gpuFallbackNote.clear();
    m_previewNote.clear();
    m_previewNoteFailure = false;
    /// @note RT に残っているのは前のソルバーの絵。新しいコマを描くまで読み戻させない。
    m_previewTile = 0;
    m_previewStale = true;

    /// @note 開けなかった報せは «絵が出ていない»。フォールバックの報せと同じ口で出すと区別できない。
    const auto openFailed = [this, &outError]() {
        m_previewNote = outError;
        m_previewNoteFailure = true;
        return false;
    };

    /// @note レシピはここでだけ読む (気体か液体かで解き方と解像度の上限が決まる)。
    FluidRecipe loaded;
    const FluidRecipe* active = recipe;
    if (active == nullptr) {
        if (!LoadRecipeByPath(key.path, loaded, outError)) return openFailed();
        active = &loaded;
    }
    const bool liquid = active->kind == FluidKind::Liquid;
    const float frameDt = (std::max)(key.frameDt, 1.0e-4f);
    int resolution = std::clamp(key.resolution, 16, kMaxVolumeResolution);
    /// @note 気体は解像度が変わらなければ 3D テクスチャ (160³ で 300 MB) をそのまま使い回し、
    /// @note       場を 0 に戻すだけで済む。液体は湧かせ方がレシピで決まるので粒子バッファだけ作り直すが、
    /// @note       シェーダー・定数・並べ替えの段は残る。Release を挟むとどちらも全部を確保し直すことになり、
    /// @note       1 手編集するたびに VRAM が波打つため、同じ種類のソルバーは Release せず Initialize し直す。
    if (key.solver != VolumeFluidSolver::Cpu) {
        std::string gpuError;
        if (liquid) {
            m_fluidGpuPreview.Release(resources);
            m_previewUsesGpu = m_liquidGpuPreview.Initialize(resources, *active, resolution, frameDt,
                                                             active->render.liquidRadiusScale, gpuError);
            if (!m_previewUsesGpu) m_liquidGpuPreview.Release(resources);
        } else {
            m_liquidGpuPreview.Release(resources);
            m_previewUsesGpu = m_fluidGpuPreview.Initialize(resources, *active, resolution, frameDt,
                                                            key.densityScale, gpuError);
            if (!m_previewUsesGpu) m_fluidGpuPreview.Release(resources);
        }
        if (!m_previewUsesGpu) {
            const std::string name = liquid ? "GPU の液体ソルバー" : "GPU の気体ソルバー";
            const std::string reason = gpuError.empty() ? std::string("理由不明") : gpuError;
            /// @note solver = "gpu" なら焼きも通らない。ここだけ CPU の絵を出すと «見えたのに焼けない» になる。
            if (key.solver == VolumeFluidSolver::Gpu) {
                outError = name + "を初期化できません (" + reason
                    + ")。CPU で見てよければ [bake] solver を \"auto\" にしてください";
                return openFailed();
            }
            /// @note 焼きと同じく CPU へ落とす。失敗ではないので描画は続け、理由だけ結果の欄に残す。
            /// @note       気体はここで諦めていたので、一度こけると再起動まで 3D プレビューが戻らなかった。
            m_gpuFallbackNote = name + "を使えないため CPU で解きます (" + reason + ")";
            m_previewNote = m_gpuFallbackNote;
            m_result = {};
            m_result.message = m_gpuFallbackNote;
        }
        m_previewGpuLiquid = m_previewUsesGpu && liquid;
    }
    if (!m_previewUsesGpu) {
        m_fluidGpuPreview.Release(resources);
        m_liquidGpuPreview.Release(resources);
        resolution = std::clamp(key.resolution, 16, kMaxFluidResolution);
        if (!m_fluidPreview.Open(*active, resolution, frameDt, key.densityScale, outError)) return openFailed();
    }
    m_previewResolution = static_cast<std::uint32_t>(resolution);
    m_previewOpen = key;
    return true;
}

void VolumeFlipbookBaker::AllowPreviewSwitch(bool allow) noexcept
{
    m_previewSwitchAllowed = allow;
}

bool VolumeFlipbookBaker::HasPendingPreviewSwitch() const noexcept
{
    if (!m_previewRequested.has_value()) return false;
    /// @note 一度こけた要求は «待ち» ではない。要求が変われば比較が外れて、もう一度試される。
    if (m_previewFailed.has_value() && *m_previewFailed == *m_previewRequested) return false;
    return !m_previewOpen.has_value() || !(*m_previewOpen == *m_previewRequested);
}

bool VolumeFlipbookBaker::IsPreviewStale() const noexcept
{
    return m_previewStale;
}

const std::string& VolumeFlipbookBaker::PreviewNote() const noexcept
{
    return m_previewNote;
}

bool VolumeFlipbookBaker::PreviewNoteIsFailure() const noexcept
{
    return m_previewNoteFailure;
}

bool VolumeFlipbookBaker::ReadbackPreview(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                                          std::vector<std::uint8_t>& outRgba8, std::uint32_t& outWidth,
                                          std::uint32_t& outHeight)
{
    if (IsBusy() || m_previewPending || !m_target.IsValid() || m_previewTile == 0 || m_previewTile != m_tileSize)
        return false;
    const std::uint32_t tile = m_previewTile;
    std::vector<float> capture;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!renderer.CaptureRenderTargetToLinearRGBA(m_target, resources, capture, width, height)
        || width != tile * kRaymarchTiles || height != tile
        || capture.size() < static_cast<std::size_t>(width) * height * 4)
        return false;

    /// @note プレビューは等倍で描き、RT には表示用の色 (露出・背景との合成・ガンマ込み) が入っている。
    /// @note       縮めも色の変換も要らず、左端の色タイルを 8bit へ落とすだけでよい。
    outRgba8.resize(static_cast<std::size_t>(tile) * tile * 4);
    for (std::uint32_t y = 0; y < tile; ++y) {
        for (std::uint32_t x = 0; x < tile; ++x) {
            const float* texel = &capture[(static_cast<std::size_t>(y) * width + x) * 4];
            std::uint8_t* out = &outRgba8[(static_cast<std::size_t>(y) * tile + x) * 4];
            out[0] = ToUnorm8(texel[0]);
            out[1] = ToUnorm8(texel[1]);
            out[2] = ToUnorm8(texel[2]);
            out[3] = 255;
        }
    }
    outWidth = tile;
    outHeight = tile;
    return true;
}

bool VolumeFlipbookBaker::CaptureFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources)
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    const std::uint32_t ss = m_supersampling;
    const std::uint32_t rtTile = m_outputTile * ss;
    if (!renderer.CaptureRenderTargetToLinearRGBA(m_target, resources, m_capture, width, height)
        || width != rtTile * kRaymarchTiles || height != rtTile
        || m_capture.size() < static_cast<std::size_t>(width) * height * 4) {
        Fail("コマ " + std::to_string(m_frameIndex) + " を GPU から読み戻せません");
        return false;
    }

    const bool distortion = m_settings.distortion;
    const bool sixWay = m_settings.sixWayLightmaps;
    const float frameDt = m_settings.source.frameDt;
    const float inverseSamples = 1.0f / static_cast<float>(ss * ss);
    const std::size_t tilePixels = static_cast<std::size_t>(m_outputTile) * m_outputTile;
    const auto texelAt = [&](std::uint32_t tile, std::uint32_t px, std::uint32_t py) {
        return &m_capture[(static_cast<std::size_t>(py) * width + tile * rtTile + px) * 4];
    };
    CapturedTile captured;
    captured.color.assign(tilePixels, math::Vector4::ZERO);
    if (!distortion) {
        captured.motion.assign(tilePixels, math::Vector2::ZERO);
        captured.coverage.assign(tilePixels, 0.0f);
    }
    if (sixWay) {
        captured.sixWayPositive.assign(tilePixels, math::Vector4::ZERO);
        captured.sixWayNegative.assign(tilePixels, math::Vector4::ZERO);
        captured.sixWayAlbedoColor.assign(tilePixels, math::Vector4::ZERO);
        captured.sixWayEmissionColor.assign(tilePixels, math::Vector4::ZERO);
    }
    bool touchesEdge = false;
    for (std::uint32_t y = 0; y < m_outputTile; ++y) {
        const bool edgeRow = y < kEdgeBandPixels || y + kEdgeBandPixels >= m_outputTile;
        for (std::uint32_t x = 0; x < m_outputTile; ++x) {
            /// @note supersampling 倍で描いた ss×ss 画素を 1 画素へ縮める。色は事前乗算なのでそのまま平均し、
            /// @note       速度・歪みの向きは «見えている量» の重みで、6 方向の Positive は被覆率で重み付けする。
            float color[4] = {};
            float motion[2] = {};
            float motionWeight = 0.0f;
            float positive[4] = {};
            float negative[4] = {};
            float albedoColor[4] = {};
            float emissionColor[4] = {};
            for (std::uint32_t sy = 0; sy < ss; ++sy) {
                for (std::uint32_t sx = 0; sx < ss; ++sx) {
                    const std::uint32_t px = x * ss + sx;
                    const std::uint32_t py = y * ss + sy;
                    const float* c = texelAt(0, px, py);
                    if (distortion) {
                        color[0] += c[0] * c[3];
                        color[1] += c[1] * c[3];
                        color[3] += c[3];
                    } else {
                        for (int i = 0; i < 4; ++i) color[i] += c[i];
                        const float* m = texelAt(1, px, py);
                        motion[0] += m[0] * m[2];
                        motion[1] += m[1] * m[2];
                        motionWeight += m[2];
                    }
                    if (sixWay) {
                        const float* p = texelAt(2, px, py);
                        const float* n = texelAt(3, px, py);
                        const float* a = texelAt(4, px, py);
                        const float* e = texelAt(5, px, py);
                        for (int i = 0; i < 3; ++i) positive[i] += p[i] * p[3];
                        positive[3] += p[3];
                        for (int i = 0; i < 4; ++i) negative[i] += n[i];
                        for (int i = 0; i < 3; ++i) albedoColor[i] += a[i] * a[3];
                        albedoColor[3] += a[3];
                        for (int i = 0; i < 4; ++i) emissionColor[i] += e[i];
                    }
                }
            }
            const std::size_t index = static_cast<std::size_t>(y) * m_outputTile + x;
            if (distortion) {
                const float inverseCoverage = color[3] > 1.0e-5f ? 1.0f / color[3] : 0.0f;
                captured.color[index] = { color[0] * inverseCoverage, color[1] * inverseCoverage, 0.0f,
                                          color[3] * inverseSamples };
            } else {
                captured.color[index] = { color[0] * inverseSamples, color[1] * inverseSamples,
                                          color[2] * inverseSamples, color[3] * inverseSamples };
                /// @note 速度 [タイル UV/秒] × 1 コマの時間 = コマ間の移動量 [タイル UV]
                const float inverseWeight = motionWeight > 1.0e-5f ? 1.0f / motionWeight : 0.0f;
                captured.motion[index] = TileUvToAtlasUv(
                    { motion[0] * inverseWeight * frameDt, motion[1] * inverseWeight * frameDt }, m_grid);
                captured.coverage[index] = motionWeight * inverseSamples;
            }
            if (sixWay) {
                const float inverseCoverage = positive[3] > 1.0e-5f ? 1.0f / positive[3] : 0.0f;
                captured.sixWayPositive[index] = { positive[0] * inverseCoverage, positive[1] * inverseCoverage,
                                                   positive[2] * inverseCoverage, positive[3] * inverseSamples };
                captured.sixWayNegative[index] = { negative[0] * inverseSamples, negative[1] * inverseSamples,
                                                   negative[2] * inverseSamples, negative[3] * inverseSamples };
                const float albedoCoverage = albedoColor[3] > 1.0e-5f ? 1.0f / albedoColor[3] : 0.0f;
                captured.sixWayAlbedoColor[index] = { albedoColor[0] * albedoCoverage,
                    albedoColor[1] * albedoCoverage, albedoColor[2] * albedoCoverage,
                    albedoColor[3] * inverseSamples };
                captured.sixWayEmissionColor[index] = { emissionColor[0] * inverseSamples,
                    emissionColor[1] * inverseSamples, emissionColor[2] * inverseSamples,
                    emissionColor[3] * inverseSamples };
            }
            const bool edgeColumn = x < kEdgeBandPixels || x + kEdgeBandPixels >= m_outputTile;
            if ((edgeRow || edgeColumn) && captured.color[index].w > kEdgeAlphaThreshold) touchesEdge = true;
        }
    }

    const int frame = m_frameIndex;
    if (frame < m_grid.frameCount && touchesEdge) ++m_edgeTouchFrames;
    if (frame < m_loopOverlap && frame < m_grid.frameCount) {
        m_loopHead[static_cast<std::size_t>(frame)] = std::move(captured);
    } else if (frame >= m_grid.frameCount) {
        /// @note 最終コマの続き。先頭の同じ番のコマへ混ぜる (最終コマ → 0 コマ目の継ぎ目がこれで消える)。
        const int head = frame - m_grid.frameCount;
        if (head < static_cast<int>(m_loopHead.size())) {
            CapturedTile& target = m_loopHead[static_cast<std::size_t>(head)];
            BlendLoopTile(target, captured, VolumeLoopKeepWeight(head, m_loopOverlap), distortion);
            CommitTile(head, target);
            target = {};
        }
    } else {
        CommitTile(frame, captured);
    }
    ++m_frameIndex;
    return true;
}

void VolumeFlipbookBaker::BlendLoopTile(CapturedTile& head, const CapturedTile& tail, float keep, bool distortion)
{
    if (head.color.size() != tail.color.size()) return;
    for (std::size_t i = 0; i < head.color.size(); ++i)
        head.color[i] = distortion ? LerpStraight(tail.color[i], head.color[i], keep)
                                   : Lerp4(tail.color[i], head.color[i], keep);
    if (head.motion.size() == tail.motion.size() && head.coverage.size() == tail.coverage.size()
        && head.motion.size() == head.coverage.size()) {
        for (std::size_t i = 0; i < head.motion.size(); ++i) {
            /// @note 変位は «見えている側» の動きを取る。片方が空の画素で平均すると動きが半分に鈍る。
            const float tailWeight = tail.coverage[i] * (1.0f - keep);
            const float headWeight = head.coverage[i] * keep;
            const float weight = tailWeight + headWeight;
            head.motion[i] = weight > 1.0e-6f
                ? (tail.motion[i] * tailWeight + head.motion[i] * headWeight) * (1.0f / weight)
                : math::Vector2::Lerp(tail.motion[i], head.motion[i], keep);
            head.coverage[i] = weight;
        }
    }
    if (head.sixWayPositive.size() == tail.sixWayPositive.size()
        && head.sixWayNegative.size() == tail.sixWayNegative.size()
        && head.sixWayAlbedoColor.size() == tail.sixWayAlbedoColor.size()
        && head.sixWayEmissionColor.size() == tail.sixWayEmissionColor.size()) {
        for (std::size_t i = 0; i < head.sixWayPositive.size(); ++i)
            head.sixWayPositive[i] = LerpStraight(tail.sixWayPositive[i], head.sixWayPositive[i], keep);
        for (std::size_t i = 0; i < head.sixWayNegative.size(); ++i)
            head.sixWayNegative[i] = Lerp4(tail.sixWayNegative[i], head.sixWayNegative[i], keep);
        for (std::size_t i = 0; i < head.sixWayAlbedoColor.size(); ++i)
            head.sixWayAlbedoColor[i] = LerpStraight(tail.sixWayAlbedoColor[i], head.sixWayAlbedoColor[i], keep);
        for (std::size_t i = 0; i < head.sixWayEmissionColor.size(); ++i)
            head.sixWayEmissionColor[i] = Lerp4(tail.sixWayEmissionColor[i], head.sixWayEmissionColor[i], keep);
    }
}

void VolumeFlipbookBaker::CommitTile(int frame, const CapturedTile& tile)
{
    const std::size_t tilePixels = static_cast<std::size_t>(m_outputTile) * m_outputTile;
    if (tile.color.size() < tilePixels) return;
    const bool distortion = m_settings.distortion;
    const bool motion = !distortion && tile.motion.size() >= tilePixels && tile.coverage.size() >= tilePixels
        && !m_motion.empty();
    const bool sixWay = m_settings.sixWayLightmaps && tile.sixWayPositive.size() >= tilePixels
        && tile.sixWayNegative.size() >= tilePixels;
    const float exposure = (std::max)(m_settings.exposure, 1.0e-3f);
    const FlipbookTileOrigin origin = TileOriginPx(frame, m_grid, m_outputTile, m_outputTile);
    for (std::uint32_t y = 0; y < m_outputTile; ++y) {
        for (std::uint32_t x = 0; x < m_outputTile; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * m_outputTile + x;
            const std::size_t atlasIndex = static_cast<std::size_t>(origin.y + y) * m_atlasWidth + (origin.x + x);
            std::uint8_t* out = &m_colorAtlas[atlasIndex * 4];
            const math::Vector4& color = tile.color[index];
            if (distortion) {
                const math::Vector4 encoded =
                    EncodeVolumeDistortion({ color.x, color.y }, color.w, m_settings.distortionScale);
                out[0] = ToUnorm8(encoded.x);
                out[1] = ToUnorm8(encoded.y);
                out[2] = ToUnorm8(encoded.z);
                out[3] = ToUnorm8(encoded.w);
            } else {
                math::Vector4 outputColor = color;
                if (m_settings.glowEmission) {
                    const float inverseAlpha = color.w > 1.0e-5f ? 1.0f / color.w : 0.0f;
                    outputColor.x *= inverseAlpha;
                    outputColor.y *= inverseAlpha;
                    outputColor.z *= inverseAlpha;
                }
                const EncodedColorTexel texel = EncodeColorTexel(outputColor, exposure);
                out[0] = texel.r;
                out[1] = texel.g;
                out[2] = texel.b;
                out[3] = texel.a;
                if (texel.clipped) ++m_clippedTexels;
            }
            if (motion) {
                m_motion[atlasIndex] = tile.motion[index];
                m_coverage[atlasIndex] = tile.coverage[index];
            }
            if (sixWay) {
                const math::Vector4& p = tile.sixWayPositive[index];
                const math::Vector4& n = tile.sixWayNegative[index];
                const math::Vector4& albedo = tile.sixWayAlbedoColor[index];
                const math::Vector4& emission = tile.sixWayEmissionColor[index];
                std::uint8_t* positive = &m_sixWayPositive[atlasIndex * 4];
                std::uint8_t* negative = &m_sixWayNegative[atlasIndex * 4];
                std::uint8_t* albedoColor = &m_sixWayAlbedoColor[atlasIndex * 4];
                std::uint8_t* emissionColor = &m_sixWayEmissionColor[atlasIndex * 4];
                positive[0] = ToUnorm8(p.x);
                positive[1] = ToUnorm8(p.y);
                positive[2] = ToUnorm8(p.z);
                positive[3] = ToUnorm8(p.w);
                negative[0] = ToUnorm8(n.x);
                negative[1] = ToUnorm8(n.y);
                negative[2] = ToUnorm8(n.z);
                negative[3] = ToUnorm8(n.w);
                const EncodedColorTexel albedoTexel = EncodeColorTexel(albedo, 1.0f);
                albedoColor[0] = albedoTexel.r;
                albedoColor[1] = albedoTexel.g;
                albedoColor[2] = albedoTexel.b;
                albedoColor[3] = ToUnorm8(albedo.w);
                const EncodedColorTexel emissionTexel = EncodeColorTexel(emission, exposure);
                emissionColor[0] = emissionTexel.r;
                emissionColor[1] = emissionTexel.g;
                emissionColor[2] = emissionTexel.b;
                emissionColor[3] = 255;
            }
        }
    }
}

void VolumeFlipbookBaker::Finish()
{
    m_fluidBake.Close();
    m_loopHead = {};
    if (m_settings.distortion) {
        /// @note 歪みマップは MV を焼かない (ファイルにも書かない)。比較プレビューは MV の Atlas を要るので、
        /// @note       «動かない» 中央値だけを渡す。
        m_outputStrength = 0.0f;
        const std::size_t pixelCount = static_cast<std::size_t>(m_atlasWidth) * m_atlasHeight;
        m_motionBytes.assign(pixelCount * 4, 0);
        for (std::size_t i = 0; i < pixelCount; ++i) {
            m_motionBytes[i * 4 + 0] = 128;
            m_motionBytes[i * 4 + 1] = 128;
            m_motionBytes[i * 4 + 3] = 255;
        }
    } else {
        DilateMotion(m_motion, m_coverage, m_atlasWidth, m_atlasHeight, m_grid, m_outputTile, m_outputTile,
                     m_settings.dilateIterations);
        m_outputStrength = ComputeRecommendedStrength(m_motion, 1.0f / static_cast<float>(m_atlasWidth));

        m_motionBytes.assign(m_motion.size() * 4, 0);
        for (std::size_t i = 0; i < m_motion.size(); ++i) {
            const EncodedMotionVector encoded = EncodeMotionVector(m_motion[i], m_outputStrength);
            m_motionBytes[i * 4 + 0] = encoded.r;
            m_motionBytes[i * 4 + 1] = encoded.g;
            m_motionBytes[i * 4 + 2] = 0;
            m_motionBytes[i * 4 + 3] = 255;
        }
    }

    const std::filesystem::path directory(m_settings.outputDirectory);
    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    if (directoryError) {
        Fail("出力ディレクトリを作成できません: " + directory.string());
        return;
    }
    /// @note 上書きが既定 (.fluid から焼く経路)。空きを探して _001 を足すと、同じレシピを焼き直すたびに
    /// @note       別のファイルが増え、.mat が指す先と食い違う — AI が «焼いた → 出た絵を見る» を回せなくなる。
    /// @note       人が «別名で残したい» と言ったときだけ空きを探す。
    m_outputBase = m_settings.overwriteOutputs
        ? directory / std::filesystem::path(m_settings.baseName)
        : detail::FindAvailableBase(directory, m_settings.baseName,
                                    { ".png", ".dds", "_mv.png", "_mv.dds", "_6wayP.png", "_6wayP.dds",
                                      "_6wayN.png", "_6wayN.dds", "_6wayC.png", "_6wayC.dds",
                                      "_6wayE.png", "_6wayE.dds" });
    if (m_outputBase.empty()) {
        Fail("空いている出力ファイル名を確保できません");
        return;
    }
    /// @note BC7 の圧縮は Atlas の大きさ次第で数十秒かかる。エディターを止めないよう裏で書き、Tick が受け取る。
    /// @note       書いている間 (Encoding) は IsBusy なので、Atlas のバッファには誰も触らない。
    m_state = VolumeFlipbookBakeState::Encoding;
    m_encodeJob = std::async(std::launch::async, [this, base = m_outputBase]() { return WriteOutputs(base); });
}

std::string VolumeFlipbookBaker::WriteOutputs(const std::filesystem::path& base) const
{
    const ComScope com;
    const auto path = [&](const char* suffix) { return std::filesystem::path(base.string() + suffix); };
    const std::uint32_t tile = m_outputTile;
    std::string error;
    /// @note PNG は Git で保持できる編集原本、DDS はコマを跨がないミップ付き出力。PNG があればマテリアルはそちらを参照する。
    /// @note       Glow は Additive ブレンドがアルファを掛けるためストレート色、それ以外の色は事前乗算で持つ。
    /// @note       薄い炎は RGB > α になり、Straight では表せない (Explosion プリセットと同じ扱い)。
    /// @note       歪みマップは 2D の Distortion と同じく Data・ストレート・素直なミップ (sRGB で読むと 0.5 の «曲げない» がずれる)。
    const bool distortion = m_settings.distortion;
    const TextureType colorType = distortion ? TextureType::Data : TextureType::Color;
    const AlphaMode colorAlpha = distortion || m_settings.glowEmission
        ? AlphaMode::Straight : AlphaMode::Premultiplied;
    const FlipbookMipContent colorMips = distortion ? FlipbookMipContent::Plain
        : m_settings.glowEmission ? FlipbookMipContent::StraightSrgb : FlipbookMipContent::PremultipliedSrgb;
    const bool colorOk =
        detail::SavePngRgba8(path(".png"), m_atlasWidth, m_atlasHeight, m_colorAtlas, error)
        && detail::SaveTextureMeta(path(".png"), colorType, TextureCompression::Auto, colorAlpha, false, error)
        && detail::SaveFlipbookDds(path(".dds"), m_atlasWidth, m_atlasHeight, m_colorAtlas, tile, tile,
                                   colorMips, detail::FlipbookDdsCompression::BC7, error)
        && detail::SaveFlipbookMeta(path(".dds"), colorType, TextureCompression::BC7, colorAlpha, error);
    if (!colorOk) return error.empty() ? std::string("書き出しに失敗しました") : error;
    if (distortion) return {};
    const bool ok =
        detail::SavePngRgba8(path("_mv.png"), m_atlasWidth, m_atlasHeight, m_motionBytes, error)
        && detail::SaveTextureMeta(path("_mv.png"), TextureType::Data, TextureCompression::BC5,
                                   AlphaMode::None, false, error)
        && detail::SaveFlipbookDds(path("_mv.dds"), m_atlasWidth, m_atlasHeight, m_motionBytes, tile, tile,
                                   FlipbookMipContent::Plain, detail::FlipbookDdsCompression::BC5, error)
        && detail::SaveFlipbookMeta(path("_mv.dds"), TextureType::Data, TextureCompression::BC5,
                                    AlphaMode::None, error);
    if (!ok) return error.empty() ? std::string("書き出しに失敗しました") : error;
    if (!m_settings.sixWayLightmaps) return {};

    /// @note 6 方向マップは色ではなく明るさ (データ)。sRGB で読むと重みの中間調がずれる。
    const bool sixWayOk =
        detail::SavePngRgba8(path("_6wayP.png"), m_atlasWidth, m_atlasHeight, m_sixWayPositive, error)
        && detail::SaveTextureMeta(path("_6wayP.png"), TextureType::Data, TextureCompression::BC7,
                                   AlphaMode::Straight, false, error)
        && detail::SaveFlipbookDds(path("_6wayP.dds"), m_atlasWidth, m_atlasHeight, m_sixWayPositive, tile, tile,
                                   FlipbookMipContent::CoverageWeighted, detail::FlipbookDdsCompression::BC7, error)
        && detail::SaveFlipbookMeta(path("_6wayP.dds"), TextureType::Data, TextureCompression::BC7,
                                    AlphaMode::Straight, error)
        && detail::SavePngRgba8(path("_6wayN.png"), m_atlasWidth, m_atlasHeight, m_sixWayNegative, error)
        && detail::SaveTextureMeta(path("_6wayN.png"), TextureType::Data, TextureCompression::BC7,
                                   AlphaMode::Straight, false, error)
        && detail::SaveFlipbookDds(path("_6wayN.dds"), m_atlasWidth, m_atlasHeight, m_sixWayNegative, tile, tile,
                                    FlipbookMipContent::Plain, detail::FlipbookDdsCompression::BC7, error)
        && detail::SaveFlipbookMeta(path("_6wayN.dds"), TextureType::Data, TextureCompression::BC7,
                                    AlphaMode::Straight, error)
        && detail::SavePngRgba8(path("_6wayC.png"), m_atlasWidth, m_atlasHeight, m_sixWayAlbedoColor, error)
        && detail::SaveTextureMeta(path("_6wayC.png"), TextureType::Color, TextureCompression::Auto,
                                   AlphaMode::Straight, false, error)
        && detail::SaveFlipbookDds(path("_6wayC.dds"), m_atlasWidth, m_atlasHeight, m_sixWayAlbedoColor, tile, tile,
                                   FlipbookMipContent::CoverageWeighted, detail::FlipbookDdsCompression::BC7, error)
        && detail::SaveFlipbookMeta(path("_6wayC.dds"), TextureType::Color, TextureCompression::BC7,
                                    AlphaMode::Straight, error)
        && detail::SavePngRgba8(path("_6wayE.png"), m_atlasWidth, m_atlasHeight, m_sixWayEmissionColor, error)
        && detail::SaveTextureMeta(path("_6wayE.png"), TextureType::Color, TextureCompression::Auto,
                                   AlphaMode::None, false, error)
        && detail::SaveFlipbookDds(path("_6wayE.dds"), m_atlasWidth, m_atlasHeight, m_sixWayEmissionColor, tile, tile,
                                   FlipbookMipContent::Plain, detail::FlipbookDdsCompression::BC7, error)
        && detail::SaveFlipbookMeta(path("_6wayE.dds"), TextureType::Color, TextureCompression::BC7,
                                    AlphaMode::None, error);
    return sixWayOk ? std::string{} : (error.empty() ? std::string("6 方向マップを書き出せません") : error);
}

void VolumeFlipbookBaker::FinishOutputs()
{
    const auto path = [&](const char* suffix) { return std::filesystem::path(m_outputBase.string() + suffix); };
    const float strength = m_outputStrength;
    const bool distortion = m_settings.distortion;
    m_result.success = true;
    m_result.colorPath = path(".dds").string();
    if (!distortion) m_result.motionPath = path("_mv.dds").string();
    if (m_settings.sixWayLightmaps) {
        m_result.sixWayPositivePath = path("_6wayP.dds").string();
        m_result.sixWayNegativePath = path("_6wayN.dds").string();
        m_result.sixWayAlbedoColorPath = path("_6wayC.dds").string();
        m_result.sixWayEmissionColorPath = path("_6wayE.dds").string();
        const math::Vector3 hottest = m_settings.blackbodyEmission
            ? (m_settings.fireEmission
                ? m_fireColorLut.Chroma(m_settings.blackbodyMaxKelvin)
                : renderer::ColorFromTemperature(m_settings.blackbodyMaxKelvin))
            : EvaluateVolumeRamp(m_settings.emissionRamp, 1.0f);
        const math::Vector3 emission = hottest * (std::max)(m_settings.emissionIntensity, 0.0f);
        m_result.sixWayEmissionColor = emission;
    }
    m_result.frameCount = m_grid.frameCount;
    m_result.columns = m_grid.columns;
    m_result.rows = m_grid.rows;
    m_result.recommendedStrength = strength;
    /// @note 歪みマップは色ではないので明るさを戻す倍率は要らない (2D の Distortion と同じ 1)。
    m_result.suggestedEmissiveScale = distortion ? 1.0f : 1.0f / (std::max)(m_settings.exposure, 1.0e-3f);
    m_result.edgeTouchFrames = m_edgeTouchFrames;
    /// @note 指紋は «人が見る絵» から取る。ここまで来ていれば Atlas はまだ手元にある (m_baked へ移すのは後)。
    BakeFingerprint fingerprint;
    fingerprint.Add(m_colorAtlas);
    fingerprint.Add(m_motionBytes);
    fingerprint.Add(m_sixWayPositive);
    fingerprint.Add(m_sixWayNegative);
    fingerprint.Add(m_sixWayAlbedoColor);
    fingerprint.Add(m_sixWayEmissionColor);
    m_result.fingerprint = fingerprint.Finish();
    m_result.solverUsed = m_bakeUsesGpu ? "gpu" : "cpu";
    m_result.fallbackReason = m_gpuFallbackNote;
    const std::size_t bakedTexels = static_cast<std::size_t>(m_outputTile) * m_outputTile * m_grid.frameCount;
    m_result.clippedFraction = bakedTexels > 0
        ? static_cast<float>(m_clippedTexels) / static_cast<float>(bakedTexels) : 0.0f;

    char summary[192]{};
    if (distortion)
        std::snprintf(summary, sizeof(summary), "%d コマ / %dx%d / 歪みマップ (MV・6-way なし)", m_grid.frameCount,
                      m_grid.columns, m_grid.rows);
    else
        std::snprintf(summary, sizeof(summary), "%d コマ / %dx%d / Motion Strength %.4f / 白飛び %.1f%%",
                      m_grid.frameCount, m_grid.columns, m_grid.rows, strength, m_result.clippedFraction * 100.0f);
    m_result.message = summary;
    if (m_loopOverlap > 0)
        m_result.message += " / Loop (続き " + std::to_string(m_loopOverlap) + " コマを先頭へ重ねました)";
    if (!m_gpuFallbackNote.empty()) m_result.message += " — " + m_gpuFallbackNote;
    if (m_result.clippedFraction > 0.02f)
        m_result.message += " — Exposure を下げると階調が残ります";
    if (m_edgeTouchFrames > 0)
        m_result.message += " — " + std::to_string(m_edgeTouchFrames)
            + " コマでタイルの縁に煙がかかっています (Framing を広げてください)";
    m_state = VolumeFlipbookBakeState::Finished;

    m_baked.colorRgba8 = std::move(m_colorAtlas);
    m_baked.motionRgba8 = std::move(m_motionBytes);
    m_baked.atlasWidth = m_atlasWidth;
    m_baked.atlasHeight = m_atlasHeight;
    m_baked.tileSize = m_outputTile;
    m_baked.grid = m_grid;
    m_baked.motionStrength = strength;
    m_baked.frameDt = m_settings.source.frameDt;
    m_baked.loop = VolumeBakeLoops(m_settings);
    m_hasBaked = true;
    ReleaseCpuBuffers();
}

bool VolumeFlipbookBaker::TakeBakedFlipbook(BakedVolumeFlipbook& out)
{
    if (!m_hasBaked) return false;
    out = std::move(m_baked);
    m_baked = {};
    m_hasBaked = false;
    return true;
}

void VolumeFlipbookBaker::Tick(renderer::IRenderer& renderer, renderer::ResourceManager& resources)
{
    if (m_state == VolumeFlipbookBakeState::Encoding) {
        if (m_encodeJob.valid() && m_encodeJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const std::string error = m_encodeJob.get();
            if (error.empty()) FinishOutputs();
            else               Fail(error);
        }
        return;
    }
    if (m_state == VolumeFlipbookBakeState::AwaitingCapture) {
        if (!CaptureFrame(renderer, resources)) return;
        if (m_frameIndex >= TotalFrames()) {
            Finish();
            return;
        }
        m_state = VolumeFlipbookBakeState::Recording;
    }
    if (m_state != VolumeFlipbookBakeState::Recording) return;

    std::string error;
    if (!EnsureGpu(resources, static_cast<std::uint32_t>(m_settings.volumeResolution), m_outputTile * m_supersampling,
                   error)) {
        Fail(error);
        return;
    }
    const float frameTime = static_cast<float>(m_frameIndex) * m_settings.source.frameDt;
    if (m_settings.sourceKind == VolumeSourceKind::Fluid && m_bakeUsesGpu) {
        /// @note GPU は 1 Tick に数コマまで進める。warmup もこの追いつきの中で済む。
        const bool solved = m_bakeGpuLiquid ? CatchUpGpuSolver(m_liquidGpuBake, renderer, resources, m_frameIndex)
                                            : CatchUpGpuSolver(m_fluidGpuBake, renderer, resources, m_frameIndex);
        if (!solved) return;
        if (m_bakeGpuLiquid) m_liquidGpuBake.WriteVolumes(renderer, resources, m_medium, m_velocity);
        else                 m_fluidGpuBake.WriteVolumes(renderer, resources, m_medium, m_velocity);
        RecordRaymarch(renderer, resources, m_settings, kDisplayRaw, 0, frameTime);
        m_state = VolumeFlipbookBakeState::AwaitingCapture;
        return;
    }
    if (m_settings.sourceKind == VolumeSourceKind::Fluid) {
        /// @note 解けたコマだけを記録する。解けていなければ Recording のまま次のフレームを待つ。
        PackedFluidVolume volume;
        if (!m_fluidBake.Poll(volume)) {
            if (!m_fluidBake.Busy()) m_fluidBake.Request(m_frameIndex);
            return;
        }
        if (volume.frame != m_frameIndex) {
            m_fluidBake.Request(m_frameIndex);
            return;
        }
        /// @note 次のコマを先に解き始めてから GPU へ載せる (CPU の解きと GPU の描画・読み戻しを重ねる)。
        if (m_frameIndex + 1 < TotalFrames()) m_fluidBake.Request(m_frameIndex + 1);
        if (!EnsureFluidGpu(resources, m_volumeResolution, error)) {
            Fail(error);
            return;
        }
        RecordFluidFrame(renderer, resources, m_settings, volume, kDisplayRaw, 0, frameTime);
        m_state = VolumeFlipbookBakeState::AwaitingCapture;
        return;
    }
    const float time = ResolveVolumeStartTime(m_settings.source)
        + static_cast<float>(m_frameIndex) * m_settings.source.frameDt;
    RecordFrame(renderer, resources, m_settings, m_puffs, time, kDisplayRaw, 0);
    m_state = VolumeFlipbookBakeState::AwaitingCapture;
}

void VolumeFlipbookBaker::ReleaseCpuBuffers()
{
    /// @note 数十 MB になるので、使い終わったら手放す。
    m_colorAtlas = {};
    m_sixWayPositive = {};
    m_sixWayNegative = {};
    m_sixWayAlbedoColor = {};
    m_sixWayEmissionColor = {};
    m_motionBytes = {};
    m_motion = {};
    m_coverage = {};
    m_capture = {};
    m_loopHead = {};
}

void VolumeFlipbookBaker::Fail(std::string message)
{
    m_fluidBake.Close();
    m_result = {};
    m_result.message = std::move(message);
    m_result.solverUsed = m_bakeUsesGpu ? "gpu" : "cpu";
    m_result.fallbackReason = m_gpuFallbackNote;
    m_state = VolumeFlipbookBakeState::Failed;
    ReleaseCpuBuffers();
}

void VolumeFlipbookBaker::Cancel()
{
    if (!IsBusy()) return;
    /// @note 書き出し中のスレッドは Atlas のバッファを読んでいる。終わるまで待ってから手放す。
    if (m_encodeJob.valid()) m_encodeJob.wait();
    m_encodeJob = {};
    m_fluidBake.Close();
    m_state = VolumeFlipbookBakeState::Idle;
    m_result = {};
    m_result.message = "キャンセルしました";
    ReleaseCpuBuffers();
}

void VolumeFlipbookBaker::Release(renderer::ResourceManager& resources)
{
    Cancel();
    m_fluidPreview.Close();
    m_fluidPreviewVolume = {};
    m_previewOpen.reset();
    m_previewRequested.reset();
    m_previewFailed.reset();
    m_previewResolution = 0;
    m_previewStale = false;
    m_previewNote.clear();
    m_previewNoteFailure = false;
    m_previewPending = false;
    m_previewTile = 0;
    ReleaseFluidGpu(resources);
    m_fluidGpuBake.Release(resources);
    m_fluidGpuPreview.Release(resources);
    m_liquidGpuBake.Release(resources);
    m_liquidGpuPreview.Release(resources);
    m_bakeUsesGpu = false;
    m_bakeGpuLiquid = false;
    m_previewUsesGpu = false;
    m_previewGpuLiquid = false;
    m_baked = {};
    m_hasBaked = false;
    /// @note デバイスリセット後のハンドルは既に実体が無い。返しに行くと別のリソースを消しかねない。
    if (m_resetVersion == resources.GetResetVersion()) {
        if (m_medium.IsValid()) resources.Release(m_medium);
        if (m_velocity.IsValid()) resources.Release(m_velocity);
        if (m_target.IsValid()) resources.Release(m_target);
        if (m_fillConstants.IsValid()) resources.Release(m_fillConstants);
        if (m_raymarchConstants.IsValid()) resources.Release(m_raymarchConstants);
        if (m_pipeline.IsValid()) resources.Release(m_pipeline);
        for (const auto& buffer : m_puffBuffers)
            if (buffer.IsValid()) resources.Release(buffer);
    }
    m_puffBuffers = {};
    m_puffRing = 0;
    m_fill = {};
    m_medium = {};
    m_velocity = {};
    m_target = {};
    m_fillConstants = {};
    m_raymarchConstants = {};
    m_pipeline = {};
    m_fillShader = {};
    m_raymarchShader = {};
    m_volumeResolution = 0;
    m_tileSize = 0;
}

}
