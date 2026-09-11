/// @file    VolumeFlipbookBaker.cpp
/// @brief   ボリューム → Flipbook / MV アトラスのベイク状態機械の実装。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/VolumeFlipbookBaker.hpp>

#include "FlipbookImageIO.hpp"

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Renderer/ComputeCall.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/Format.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Math/Vector4.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace fbzz::asset {
namespace {

constexpr const char* kFillShaderPath = "Assets/Shaders/Bake/VolumeFlipbook/VolumeFill.cs.hlsl";
constexpr const char* kRaymarchShaderPath = "Assets/Shaders/Bake/VolumeFlipbook/VolumeRaymarch.hlsl";
constexpr std::uint32_t kMaxAtlasDimension = 16384;
constexpr std::size_t kMaxAtlasPixels = 16u * 1024u * 1024u;
constexpr float kDegreesToRadians = 3.14159265358979f / 180.0f;
// タイル外周のこの幅 [px] に α がこれ以上あれば «縁にかかっている» とみなす。
constexpr std::uint32_t kEdgeBandPixels = 2;
constexpr float kEdgeAlphaThreshold = 0.02f;

constexpr std::uint32_t kDisplayRaw = 0;
constexpr std::uint32_t kDisplayColor = 1;
constexpr std::uint32_t kDisplayAlpha = 2;

// VolumeRaymarch.hlsl の cbuffer と 1:1。
struct alignas(16) RaymarchConstants {
    float camRight[3];   float halfExtent;
    float camUp[3];      std::uint32_t tileSize;
    float camForward[3]; std::uint32_t raySteps;
    float toLight[3];    std::uint32_t shadowSteps;
    float lightColor[3]; float extinction;
    float ambient[3];    float emissionIntensity;
    float smokeAlbedo[3]; float exposure;
    std::uint32_t displayMode;
    float anisotropy;
    float previewMotionScale;
    std::uint32_t background;
};
static_assert(sizeof(RaymarchConstants) == 128, "VolumeRaymarch.hlsl の cbuffer と一致させること");

void Store(float (&out)[3], const math::Vector3& v)
{
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

RaymarchConstants BuildRaymarchConstants(const VolumeFlipbookBakeSettings& settings, std::uint32_t tileSize,
                                         std::uint32_t displayMode, std::uint32_t background)
{
    const VolumeFlipbookCamera camera = ComputeVolumeFlipbookCamera(settings);
    RaymarchConstants constants{};
    Store(constants.camRight, camera.right);
    Store(constants.camUp, camera.up);
    Store(constants.camForward, camera.forward);
    Store(constants.toLight, camera.toLight);
    Store(constants.lightColor, settings.lightColor);
    Store(constants.ambient, settings.ambient);
    Store(constants.smokeAlbedo, { settings.smokeAlbedo, settings.smokeAlbedo, settings.smokeAlbedo });
    constants.halfExtent = (std::max)(settings.halfExtent, 0.05f);
    constants.tileSize = tileSize;
    constants.raySteps = static_cast<std::uint32_t>(std::clamp(settings.raySteps, 8, 512));
    constants.shadowSteps = static_cast<std::uint32_t>(std::clamp(settings.shadowSteps, 1, 64));
    constants.extinction = (std::max)(settings.extinction, 0.0f);
    constants.emissionIntensity = (std::max)(settings.emissionIntensity, 0.0f);
    constants.exposure = (std::max)(settings.exposure, 1.0e-3f);
    constants.displayMode = displayMode;
    constants.anisotropy = std::clamp(settings.anisotropy, -0.95f, 0.95f);
    // 1 秒でタイルの 1/4 動く速さを色の振り切りにする。見て分かる程度の目安でよい。
    constants.previewMotionScale = 2.0f;
    constants.background = background;
    return constants;
}

} // namespace

VolumeFlipbookCamera ComputeVolumeFlipbookCamera(const VolumeFlipbookBakeSettings& settings)
{
    // DirectX の左手系: yaw 0 で +Z を向き、+X が画面右。
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

VolumeFramingReport AnalyzeVolumeFraming(const VolumeFlipbookBakeSettings& settings)
{
    VolumeFramingReport report;
    const int frames = std::clamp(settings.source.frameCount, 1, 256);
    const float frameDt = (std::max)(settings.source.frameDt, 1.0e-4f);
    const float halfExtent = (std::max)(settings.halfExtent, 0.05f);
    const VolumeFlipbookCamera camera = ComputeVolumeFlipbookCamera(settings);
    const std::vector<VolumePuff> puffs = BuildVolumePuffs(settings.source);
    const float start = ResolveVolumeStartTime(settings.source);

    report.frameIssues.assign(static_cast<std::size_t>(frames), 0);
    for (int frame = 0; frame < frames; ++frame) {
        std::uint8_t issues = 0;
        for (const VolumeBound& bound :
             CollectVisibleVolumeBounds(puffs, settings.noise, start + static_cast<float>(frame) * frameDt)) {
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
    }
    return report;
}

bool VolumeFlipbookBaker::EnsureGpu(renderer::ResourceManager& resources, std::uint32_t volumeResolution,
                                    std::uint32_t tileSize, std::string& outError)
{
    // デバイスリセット後の古いハンドルは返せない (実体ごと消えている)。捨てて作り直す。
    if (m_resetVersion != resources.GetResetVersion()) {
        m_fillShader = {};
        m_raymarchShader = {};
        m_pipeline = {};
        m_fillConstants = {};
        m_raymarchConstants = {};
        m_medium = {};
        m_velocity = {};
        m_target = {};
        m_volumeResolution = 0;
        m_tileSize = 0;
        m_resetVersion = resources.GetResetVersion();
    }

    if (!m_fillShader.IsValid())
        m_fillShader = resources.LoadShader(AssetManager::ResolveAssetPath(kFillShaderPath));
    if (!m_raymarchShader.IsValid())
        m_raymarchShader = resources.LoadShader(AssetManager::ResolveAssetPath(kRaymarchShaderPath));
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
        m_fillConstants = resources.CreateConstantBuffer(sizeof(VolumeFillConstants));
    if (!m_raymarchConstants.IsValid())
        m_raymarchConstants = resources.CreateConstantBuffer(sizeof(RaymarchConstants));

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
        m_target = resources.CreateRenderTarget(tileSize * 2, tileSize,
                                                renderer::RenderTargetDesc{ 1, renderer::Format::RGBA16F, false });
        m_tileSize = tileSize;
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
    m_settings = settings;
    m_settings.source.frameCount = std::clamp(settings.source.frameCount, 2, 256);
    m_settings.source.frameDt = std::clamp(settings.source.frameDt, 1.0f / 240.0f, 1.0f);
    m_settings.volumeResolution = std::clamp(settings.volumeResolution, 16, 128);
    m_settings.tileSize = std::clamp(settings.tileSize, 32, 1024);
    if (m_settings.outputDirectory.empty()) {
        outError = "出力先がありません";
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
    if (!EnsureGpu(resources, static_cast<std::uint32_t>(m_settings.volumeResolution), tile, outError))
        return false;

    const std::size_t pixelCount = static_cast<std::size_t>(m_atlasWidth) * m_atlasHeight;
    m_colorAtlas.assign(pixelCount * 4, 0);
    m_motion.assign(pixelCount, math::Vector2::ZERO);
    m_coverage.assign(pixelCount, 0.0f);
    m_puffs = BuildVolumePuffs(m_settings.source);
    m_clippedTexels = 0;
    m_edgeTouchFrames = 0;
    m_frameIndex = 0;
    m_result = {};
    m_baked = {};
    m_hasBaked = false;
    m_state = VolumeFlipbookBakeState::Recording;
    return true;
}

void VolumeFlipbookBaker::RecordFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                                      const VolumeFlipbookBakeSettings& settings,
                                      const std::vector<VolumePuff>& puffs, float time,
                                      std::uint32_t displayMode, std::uint32_t background)
{
    VolumeFillConstants fill;
    PackVolumeFillConstants(puffs, settings.noise, m_volumeResolution, time, settings.source.frameDt, fill);
    resources.Update(m_fillConstants, &fill, sizeof(fill));

    renderer::ComputeCall compute;
    compute.shader = m_fillShader;
    compute.constantBuffers[0] = m_fillConstants;
    compute.uavOutputs[0] = m_medium;
    compute.uavOutputs[1] = m_velocity;
    compute.dispatchX = compute.dispatchY = compute.dispatchZ = (m_volumeResolution + 3) / 4;
    renderer.Dispatch(compute, resources);

    const RaymarchConstants raymarch = BuildRaymarchConstants(settings, m_tileSize, displayMode, background);
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
    // DX12 は別の RT へ切り替えたときに初めて RT をシェーダー読み取り状態へ戻す。
    // 読み戻しと ImGui 表示はその状態を前提にしている。
    renderer.SetRenderTarget({}, resources);
}

void VolumeFlipbookBaker::RecordPreview(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                                        const VolumeFlipbookBakeSettings& settings, float time,
                                        const VolumePreviewOptions& options)
{
    if (IsBusy()) return;
    std::string error;
    const auto resolution = static_cast<std::uint32_t>(std::clamp(settings.volumeResolution, 16, 128));
    const auto tile = static_cast<std::uint32_t>(std::clamp(settings.tileSize, 32, 1024));
    if (!EnsureGpu(resources, resolution, tile, error)) {
        m_result = {};
        m_result.message = error;
        return;
    }
    const std::vector<VolumePuff> puffs = BuildVolumePuffs(settings.source);
    const std::uint32_t displayMode = options.view == VolumePreviewView::Alpha ? kDisplayAlpha : kDisplayColor;
    RecordFrame(renderer, resources, settings, puffs, ResolveVolumeStartTime(settings.source) + time,
                displayMode, static_cast<std::uint32_t>(options.background));
}

bool VolumeFlipbookBaker::CaptureFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources)
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!renderer.CaptureRenderTargetToLinearRGBA(m_target, resources, m_capture, width, height)
        || width != m_tileSize * 2 || height != m_tileSize
        || m_capture.size() < static_cast<std::size_t>(width) * height * 4) {
        Fail("コマ " + std::to_string(m_frameIndex) + " を GPU から読み戻せません");
        return false;
    }

    const float exposure = (std::max)(m_settings.exposure, 1.0e-3f);
    const float frameDt = m_settings.source.frameDt;
    const FlipbookTileOrigin origin = TileOriginPx(m_frameIndex, m_grid, m_tileSize, m_tileSize);
    bool touchesEdge = false;
    for (std::uint32_t y = 0; y < m_tileSize; ++y) {
        const bool edgeRow = y < kEdgeBandPixels || y + kEdgeBandPixels >= m_tileSize;
        for (std::uint32_t x = 0; x < m_tileSize; ++x) {
            const float* color = &m_capture[(static_cast<std::size_t>(y) * width + x) * 4];
            const float* motion = &m_capture[(static_cast<std::size_t>(y) * width + m_tileSize + x) * 4];
            const std::size_t atlasIndex =
                static_cast<std::size_t>(origin.y + y) * m_atlasWidth + (origin.x + x);

            const EncodedColorTexel texel =
                EncodeColorTexel({ color[0], color[1], color[2], color[3] }, exposure);
            m_colorAtlas[atlasIndex * 4 + 0] = texel.r;
            m_colorAtlas[atlasIndex * 4 + 1] = texel.g;
            m_colorAtlas[atlasIndex * 4 + 2] = texel.b;
            m_colorAtlas[atlasIndex * 4 + 3] = texel.a;
            if (texel.clipped) ++m_clippedTexels;
            const bool edgeColumn = x < kEdgeBandPixels || x + kEdgeBandPixels >= m_tileSize;
            if ((edgeRow || edgeColumn) && color[3] > kEdgeAlphaThreshold) touchesEdge = true;

            // 速度 [タイル UV/秒] × 1 コマの時間 = コマ間の移動量 [タイル UV]
            m_motion[atlasIndex] = TileUvToAtlasUv({ motion[0] * frameDt, motion[1] * frameDt }, m_grid);
            m_coverage[atlasIndex] = motion[2];
        }
    }
    if (touchesEdge) ++m_edgeTouchFrames;
    ++m_frameIndex;
    return true;
}

void VolumeFlipbookBaker::Finish()
{
    DilateMotion(m_motion, m_coverage, m_atlasWidth, m_atlasHeight, m_grid, m_tileSize, m_tileSize,
                 m_settings.dilateIterations);
    const float strength = ComputeRecommendedStrength(m_motion, 1.0f / static_cast<float>(m_atlasWidth));

    std::vector<std::uint8_t> motionBytes(m_motion.size() * 4);
    for (std::size_t i = 0; i < m_motion.size(); ++i) {
        const EncodedMotionVector encoded = EncodeMotionVector(m_motion[i], strength);
        motionBytes[i * 4 + 0] = encoded.r;
        motionBytes[i * 4 + 1] = encoded.g;
        motionBytes[i * 4 + 2] = 0;
        motionBytes[i * 4 + 3] = 255;
    }

    const std::filesystem::path directory(m_settings.outputDirectory);
    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    if (directoryError) {
        Fail("出力ディレクトリを作成できません: " + directory.string());
        return;
    }
    const std::filesystem::path base = detail::FindAvailableBase(directory, m_settings.baseName, { ".png", "_mv.png" });
    if (base.empty()) {
        Fail("空いている出力ファイル名を確保できません");
        return;
    }
    const std::filesystem::path colorPath = base.string() + ".png";
    const std::filesystem::path motionPath = base.string() + "_mv.png";

    std::string error;
    // 色は事前乗算で持つ。薄い炎は RGB > α になり、Straight では表せない (Explosion プリセットと同じ扱い)。
    if (!detail::SavePngRgba8(colorPath, m_atlasWidth, m_atlasHeight, m_colorAtlas, error)
        || !detail::SaveTextureMeta(colorPath, TextureType::Color, TextureCompression::Auto,
                                    AlphaMode::Premultiplied, false, error)
        || !detail::SavePngRgba8(motionPath, m_atlasWidth, m_atlasHeight, motionBytes, error)
        || !detail::SaveTextureMeta(motionPath, TextureType::Data, TextureCompression::BC5,
                                    AlphaMode::None, false, error)) {
        Fail(error);
        return;
    }

    m_result.success = true;
    m_result.colorPath = colorPath.string();
    m_result.motionPath = motionPath.string();
    m_result.frameCount = m_grid.frameCount;
    m_result.columns = m_grid.columns;
    m_result.rows = m_grid.rows;
    m_result.recommendedStrength = strength;
    m_result.suggestedEmissiveScale = 1.0f / (std::max)(m_settings.exposure, 1.0e-3f);
    m_result.edgeTouchFrames = m_edgeTouchFrames;
    const std::size_t bakedTexels = static_cast<std::size_t>(m_tileSize) * m_tileSize * m_grid.frameCount;
    m_result.clippedFraction = bakedTexels > 0
        ? static_cast<float>(m_clippedTexels) / static_cast<float>(bakedTexels) : 0.0f;

    char summary[192]{};
    std::snprintf(summary, sizeof(summary), "%d コマ / %dx%d / Motion Strength %.4f / 白飛び %.1f%%",
                  m_grid.frameCount, m_grid.columns, m_grid.rows, strength, m_result.clippedFraction * 100.0f);
    m_result.message = summary;
    if (m_result.clippedFraction > 0.02f)
        m_result.message += " — Exposure を下げると階調が残ります";
    if (m_edgeTouchFrames > 0)
        m_result.message += " — " + std::to_string(m_edgeTouchFrames)
            + " コマでタイルの縁に煙がかかっています (Framing を広げてください)";
    m_state = VolumeFlipbookBakeState::Finished;

    m_baked.colorRgba8 = std::move(m_colorAtlas);
    m_baked.motionRgba8 = std::move(motionBytes);
    m_baked.atlasWidth = m_atlasWidth;
    m_baked.atlasHeight = m_atlasHeight;
    m_baked.tileSize = m_tileSize;
    m_baked.grid = m_grid;
    m_baked.motionStrength = strength;
    m_baked.frameDt = m_settings.source.frameDt;
    m_baked.loop = m_settings.source.loop && m_settings.source.preset == VolumeFlipbookPreset::RisingPlume;
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
    if (m_state == VolumeFlipbookBakeState::AwaitingCapture) {
        if (!CaptureFrame(renderer, resources)) return;
        if (m_frameIndex >= m_grid.frameCount) {
            Finish();
            return;
        }
        m_state = VolumeFlipbookBakeState::Recording;
    }
    if (m_state != VolumeFlipbookBakeState::Recording) return;

    std::string error;
    if (!EnsureGpu(resources, static_cast<std::uint32_t>(m_settings.volumeResolution),
                   static_cast<std::uint32_t>(m_settings.tileSize), error)) {
        Fail(error);
        return;
    }
    const float time = ResolveVolumeStartTime(m_settings.source)
        + static_cast<float>(m_frameIndex) * m_settings.source.frameDt;
    RecordFrame(renderer, resources, m_settings, m_puffs, time, kDisplayRaw, 0);
    m_state = VolumeFlipbookBakeState::AwaitingCapture;
}

void VolumeFlipbookBaker::ReleaseCpuBuffers()
{
    // 数十 MB になるので、使い終わったら手放す。
    m_colorAtlas = {};
    m_motion = {};
    m_coverage = {};
    m_capture = {};
}

void VolumeFlipbookBaker::Fail(std::string message)
{
    m_result = {};
    m_result.message = std::move(message);
    m_state = VolumeFlipbookBakeState::Failed;
    ReleaseCpuBuffers();
}

void VolumeFlipbookBaker::Cancel()
{
    if (!IsBusy()) return;
    m_state = VolumeFlipbookBakeState::Idle;
    m_result = {};
    m_result.message = "キャンセルしました";
    ReleaseCpuBuffers();
}

void VolumeFlipbookBaker::Release(renderer::ResourceManager& resources)
{
    Cancel();
    m_baked = {};
    m_hasBaked = false;
    // デバイスリセット後のハンドルは既に実体が無い。返しに行くと別のリソースを消しかねない。
    if (m_resetVersion == resources.GetResetVersion()) {
        if (m_medium.IsValid()) resources.Release(m_medium);
        if (m_velocity.IsValid()) resources.Release(m_velocity);
        if (m_target.IsValid()) resources.Release(m_target);
        if (m_fillConstants.IsValid()) resources.Release(m_fillConstants);
        if (m_raymarchConstants.IsValid()) resources.Release(m_raymarchConstants);
        if (m_pipeline.IsValid()) resources.Release(m_pipeline);
    }
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

} // namespace fbzz::asset
