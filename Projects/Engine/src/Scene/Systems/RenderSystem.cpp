/// @file    RenderSystem.cpp
/// @brief   Scene から DrawCall を生成するオーケストレーター。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// @note 各描画パスの実装は RenderPasses/ 以下の Execute*Pass 関数に委譲する。
#include "Engine/Scene/Systems/RenderSystem.hpp"
#include "Engine/Scene/SceneUtils.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/TerrainRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/WaterRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/FiberRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/MeshTrailRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/TrailRenderPass.hpp"
#include "Engine/Renderer/RenderSettings.hpp"
#include "Engine/Renderer/RenderDebugOverlay.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassCapture.hpp>
#include "Engine/Renderer/DebugDraw.hpp"
#include "RenderPasses/Debug/DebugPasses.hpp"
#include <Physics/World.hpp>
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include <Engine/Scene/Components/ParticleEmitterSpace.hpp>
#include "Engine/Scene/Components/ParticleEmitter.hpp"
#include "Engine/Scene/Components/ParticleGpuSimulation.hpp"
#include "Engine/Scene/Components/ParticleLightSelection.hpp"
#include "RenderPasses/PostProcess/PostProcessPasses.hpp"
#include "RenderPasses/PostProcess/CloudNoiseBake.hpp"
#include <Engine/Scene/Systems/RenderPasses/InstanceBatch.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include "RenderPasses/Debug/SelectionPasses.hpp"
#include "Engine/Core/Application.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/ScriptComponent.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/LightComponent.hpp"
#include "Engine/Scene/Components/EnvironmentLightComponent.hpp"
#include "Engine/Scene/Components/AtmosphericScatteringComponent.hpp"
#include "Engine/Scene/Components/SkyRenderer.hpp"
#include "Engine/Scene/Components/PostProcessVolumeComponent.hpp"
#include "Engine/Scene/Components/WeatherComponent.hpp"
#include "Engine/Renderer/PostProcessBlend.hpp"
#include "Engine/Scene/Components/VFXScreenEffect.hpp"
#include "Engine/Scene/Components/ReflectionProbeComponent.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/Camera.hpp"
#include "Engine/Renderer/ColorTemperature.hpp"
#include "Engine/Renderer/PipelineDiagnostics.hpp"
#include "Engine/Renderer/LightSystem.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Renderer/PrimitiveMesh.hpp"
#include "Engine/Scene/Systems/RenderPasses/RenderPipeline.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/DynamicBufferPool.hpp"
#include "Engine/Asset/Skeleton.hpp"
#include <Engine/Profiler/ProfileScope.hpp>
#include <Math/Frustum.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

/// @note SceneUtils.hpp の ResolveGameCullingSettings は、呼び出し側が明示しなかったときのフォールバック解決に使う。
namespace fbzz::scene {

namespace {

struct SceneShadowBounds {
    math::Vector3 center = math::Vector3::ZERO;
    float radius = 0.0f;
    bool valid = false;
};

constexpr uint32_t PROCEDURAL_LUT_SIZE = 32u;

uint64_t HashLutSettings(const renderer::LUTColorGradingSettings& settings)
{
    uint64_t hash = 1469598103934665603ull;
    const auto append = [&](float value) {
        hash ^= std::bit_cast<uint32_t>(value);
        hash *= 1099511628211ull;
    };
    append(settings.contrast);
    append(settings.saturation);
    append(settings.hueShift);
    append(settings.temperature);
    append(settings.tint);
    return hash;
}

/// @brief RendererがサンプルするLDR RGB座標と同じ順序で32^3 RGBA8 LUTを生成する。
/// @note x=R, y=G, z=B、xが最速で並ぶD3D11 Texture3Dのメモリ配置にする。
std::vector<uint8_t> GenerateProceduralColorLut(const renderer::LUTColorGradingSettings& settings)
{
    constexpr float PI = 3.14159265358979323846f;
    const float angle = settings.hueShift * (PI / 180.0f);
    const float s = std::sin(angle);
    const float c = std::cos(angle);
    const float hue[3][3] = {
        { 0.299f + 0.701f*c + 0.168f*s, 0.587f - 0.587f*c + 0.330f*s, 0.114f - 0.114f*c - 0.497f*s },
        { 0.299f - 0.299f*c - 0.328f*s, 0.587f + 0.413f*c + 0.035f*s, 0.114f - 0.114f*c + 0.292f*s },
        { 0.299f - 0.300f*c + 1.250f*s, 0.587f - 0.588f*c - 1.050f*s, 0.114f + 0.886f*c - 0.203f*s },
    };
    const float balance[3] = {
        (std::max)(1.0f + settings.temperature * 0.08f - settings.tint * 0.03f, 0.0f),
        (std::max)(1.0f + settings.tint * 0.06f, 0.0f),
        (std::max)(1.0f - settings.temperature * 0.08f - settings.tint * 0.03f, 0.0f),
    };

    std::vector<uint8_t> pixels(PROCEDURAL_LUT_SIZE * PROCEDURAL_LUT_SIZE * PROCEDURAL_LUT_SIZE * 4u);
    const auto toUnorm = [](float value) {
        return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    const float denominator = static_cast<float>(PROCEDURAL_LUT_SIZE - 1u);
    for (uint32_t b = 0; b < PROCEDURAL_LUT_SIZE; ++b) {
        for (uint32_t g = 0; g < PROCEDURAL_LUT_SIZE; ++g) {
            for (uint32_t r = 0; r < PROCEDURAL_LUT_SIZE; ++r) {
                const float input[3] = { r / denominator, g / denominator, b / denominator };
                const float balanced[3] = {
                    input[0] * balance[0], input[1] * balance[1], input[2] * balance[2]
                };
                float color[3] = {
                    hue[0][0]*balanced[0] + hue[0][1]*balanced[1] + hue[0][2]*balanced[2],
                    hue[1][0]*balanced[0] + hue[1][1]*balanced[1] + hue[1][2]*balanced[2],
                    hue[2][0]*balanced[0] + hue[2][1]*balanced[1] + hue[2][2]*balanced[2],
                };
                for (float& channel : color)
                    channel = (channel - 0.5f) * (1.0f + settings.contrast) + 0.5f;
                const float luma = color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
                for (float& channel : color)
                    channel = luma + (channel - luma) * settings.saturation;

                const size_t index = ((static_cast<size_t>(b) * PROCEDURAL_LUT_SIZE + g) *
                                      PROCEDURAL_LUT_SIZE + r) * 4u;
                pixels[index + 0] = toUnorm(color[0]);
                pixels[index + 1] = toUnorm(color[1]);
                pixels[index + 2] = toUnorm(color[2]);
                pixels[index + 3] = 255u;
            }
        }
    }
    return pixels;
}

/// @note Viewport ごとに解像度依存の中間リソースを保持する。
/// @note static で共有すると SceneView と GameView が 1 フレーム内でリサイズし合う。
struct ViewRenderTargets {
    /// @note View ごとの RenderGraph 計画と transient RT をフレーム間で保持する。
    /// @note スタック生成だと DX12 の descriptor heap を毎フレーム作り直し、CPU が詰まる。
    RenderPipeline pipeline;
    renderer::ResourceHandle<renderer::RenderTargetTag> hdr;
    renderer::ResourceHandle<renderer::RenderTargetTag> ldr;
    renderer::ResourceHandle<renderer::RenderTargetTag> selectionMask;
    renderer::ResourceHandle<renderer::RenderTargetTag> outline;
    /// @note ランタイム輪郭のシルエット (RGB=色 / A=太さ)。エディタ選択のマスクとは別物。
    renderer::ResourceHandle<renderer::RenderTargetTag> objectMask;
    renderer::ResourceHandle<renderer::RenderTargetTag> customPostProcess[2];
    /// @note ポストプロセスチェーンの終着点。描画スケールが等倍でないフレームだけ持ち、
    /// @note UpscalePass がここから出力先の実寸へ解像する。等倍なら確保しない。
    renderer::ResourceHandle<renderer::RenderTargetTag> upscaleSrc;
    renderer::ResourceHandle<renderer::RenderTargetTag> gbuffer;
    /// @note モーションベクター (RG=速度, B=書き込み済みフラグ)。TAA / MotionBlur が有効な
    /// @note フレームだけ描く。深度は自前で持つので、本描画の深度バッファとは共有しない。
    renderer::ResourceHandle<renderer::RenderTargetTag> velocity;
    renderer::ResourceHandle<renderer::RenderTargetTag> decalDepth;
    renderer::ResourceHandle<renderer::RenderTargetTag> decalMask;
    /// @note Bloom のミップ連鎖。bloomChain[0] が半解像度で、以降 1/2 ずつ。
    /// @note bloomHalf は bloomChain[0] の別名 (Composite 側の参照名)。
    /// @note 1 枚のミップ付きにしないのは CreateComputeTexture がミップを持たないため。
    renderer::ResourceHandle<renderer::TextureTag> bloomChain[kBloomMipCount];
    renderer::ResourceHandle<renderer::TextureTag> bloomUpChain[kBloomMipCount];
    renderer::ResourceHandle<renderer::TextureTag> bloomHalf;
    renderer::ResourceHandle<renderer::TextureTag> bloomFull;
    renderer::ResourceHandle<renderer::TextureTag> ssaoRaw;
    renderer::ResourceHandle<renderer::TextureTag> ssaoBlur;
    /// @name Advanced Graphics (解像度依存・ビュー単位)
    /// @note 解像度非依存な static リソース (BRDF LUT 等) は別途 static 変数が持つ。
    renderer::ResourceHandle<renderer::TextureTag>        ssrResult;           ///< @note SSR CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        volumetricResult;    ///< @note Volumetric CS 出力
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryA;         ///< @note TAA ping-pong A
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryB;         ///< @note TAA ping-pong B
    /// @note TAA の ping-pong の向き。RenderPassHandles はフレームごとに作り直すので、
    /// @note ここに持たないと毎フレーム false から始まり «A を読んで B に書く» しか起きない。
    /// @note A は一度も書かれず、履歴は最初の中身のまま固定される。
    bool taaFlip = false;
    renderer::ResourceHandle<renderer::TextureTag>        motionBlurResult;    ///< @note Motion Blur CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        gtaoRaw;             ///< @note GTAO RAW CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        gtaoBlur;            ///< @note GTAO Blur CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        contactShadowResult; ///< @note Contact Shadow CS 出力
    /// @name ビュー別定数バッファ / 再投影行列
    /// @note static で共有すると SceneView と GameView が互いのカメラ行列を引き、
    /// @note MotionBlur / TAA の再投影が常に壊れる。
    renderer::ResourceHandle<renderer::ConstantBufferTag> advancedGraphicsCB;
    /// @name 自動露出 (ビュー単位・解像度非依存)
    /// @note exposureResult は「順応済みの平均輝度」でフレームをまたぐ状態。static で共有すると
    /// @note SceneView と GameView が交互に順応を進め、互いの明るさへ引きずられて露出が振れる。
    renderer::ResourceHandle<renderer::StructuredBufferTag> exposureHistogram;
    renderer::ResourceHandle<renderer::StructuredBufferTag> exposureResult;
    uint32_t exposureResetGeneration = 0;
    /// @name 体積雲の作業 RT (ビュー単位・解像度依存)
    renderer::SizedRenderTarget cloudRT;
    renderer::SizedRenderTarget cloudDepthRT;
    /// @name 水面の屈折用コピー (ビュー単位・解像度依存)
    renderer::SizedRenderTarget waterSceneColorRT;
    renderer::SizedRenderTarget waterSceneDepthRT;
    /// @name 歪みパーティクルの背景退避 / 重なり計数 / コースティクスの深度コピー
    /// @note 水面と同じくビュー単位。共有すると 2 ビューで寸法を取り合い、毎フレーム作り直す。
    renderer::SizedRenderTarget particleSceneColorRT;
    renderer::SizedRenderTarget particleOverdrawRT;
    renderer::SizedRenderTarget particleReactiveRT;
    renderer::SizedRenderTarget causticsDepthRT;
    /// @name フロクセル霧 (ビュー単位・解像度非依存)
    /// @note グリッドは視錐台に貼り付くので、共有すると互いの履歴を上書きして霧が明滅する。
    /// @note 寸法は設定値 (既定 160x90x64) で画面サイズと無関係なので、リサイズでは作り直さない。
    renderer::ResourceHandle<renderer::TextureTag> froxelScatter;
    renderer::ResourceHandle<renderer::TextureTag> froxelScatterHistory;
    renderer::ResourceHandle<renderer::TextureTag> froxelIntegrated;
    uint32_t                     froxelGrid[3] = { 0u, 0u, 0u };
    FroxelFogViewState           froxelState;
    math::Matrix4 prevViewProjection    = math::Matrix4::Identity();
    math::Matrix4 invPrevViewProjection = math::Matrix4::Identity();
    /// @note TAA ジッター列の現在位置。ビュー別に持たないと SceneView と GameView が
    /// @note 同じ番号を取り合って、どちらもサンプル点が飛び飛びになる。
    uint32_t taaFrameIndex = 0;
    /// @brief TAA 履歴 (taaHistoryA/B) に «前のフレームの絵» が入っているか。
    /// @note 作り直した直後と TAA を切っていた後は中身が未定義か古い絵。false の間は taaFeedback を 0 にし、
    ///       今のフレームだけで履歴を作り直す。リサイズでは保存・復元しないので false に戻る。
    bool taaHistoryValid = false;
    uint32_t width = 0;
    uint32_t height = 0;
};

/// @note Halton 列 (基数 base) の index 番目。
/// @note TAA には少ない枚数でも偏らない散り方が要る。乱数だと数フレームでは固まる。
float HaltonRadicalInverse(uint32_t index, uint32_t base)
{
    const float invBase = 1.0f / static_cast<float>(base);
    float result   = 0.0f;
    float fraction = invBase;
    while (index > 0u) {
        result   += static_cast<float>(index % base) * fraction;
        index    /= base;
        fraction *= invBase;
    }
    return result;
}

/// @note Resize 前のネイティブリソースを ResourceManager から確実に解放する。
void ReleaseViewRenderTargets(ViewRenderTargets& targets, renderer::ResourceManager& resources)
{
    /// @note transient RT も同じ Viewport 寿命に属するため、固定 RT より先に明示解放する。
    targets.pipeline.ReleaseViewResources(resources);
    if (targets.hdr.IsValid())                  resources.Release(targets.hdr);
    if (targets.ldr.IsValid())                  resources.Release(targets.ldr);
    if (targets.selectionMask.IsValid())        resources.Release(targets.selectionMask);
    if (targets.outline.IsValid())              resources.Release(targets.outline);
    if (targets.objectMask.IsValid())          resources.Release(targets.objectMask);
    if (targets.customPostProcess[0].IsValid()) resources.Release(targets.customPostProcess[0]);
    if (targets.customPostProcess[1].IsValid()) resources.Release(targets.customPostProcess[1]);
    if (targets.upscaleSrc.IsValid())           resources.Release(targets.upscaleSrc);
    if (targets.gbuffer.IsValid())              resources.Release(targets.gbuffer);
    if (targets.velocity.IsValid())             resources.Release(targets.velocity);
    if (targets.decalDepth.IsValid())           resources.Release(targets.decalDepth);
    if (targets.decalMask.IsValid())            resources.Release(targets.decalMask);
    /// @note bloomHalf は bloomChain[0] の別名なので、ここでは解放しない (二重解放になる)。
    for (auto& mip : targets.bloomChain)
        if (mip.IsValid())                      resources.Release(mip);
    for (auto& mip : targets.bloomUpChain)
        if (mip.IsValid())                      resources.Release(mip);
    targets.bloomHalf = {};
    if (targets.bloomFull.IsValid())            resources.Release(targets.bloomFull);
    if (targets.ssaoRaw.IsValid())               resources.Release(targets.ssaoRaw);
    if (targets.ssaoBlur.IsValid())              resources.Release(targets.ssaoBlur);
    if (targets.ssrResult.IsValid())             resources.Release(targets.ssrResult);
    if (targets.volumetricResult.IsValid())      resources.Release(targets.volumetricResult);
    if (targets.taaHistoryA.IsValid())           resources.Release(targets.taaHistoryA);
    if (targets.taaHistoryB.IsValid())           resources.Release(targets.taaHistoryB);
    if (targets.motionBlurResult.IsValid())      resources.Release(targets.motionBlurResult);
    if (targets.gtaoRaw.IsValid())               resources.Release(targets.gtaoRaw);
    if (targets.gtaoBlur.IsValid())              resources.Release(targets.gtaoBlur);
    if (targets.contactShadowResult.IsValid())   resources.Release(targets.contactShadowResult);
    /// @note どちらも解像度非依存。作り直すと prevVP が Identity へ戻り、
    /// @note MotionBlur / TAA が 1 フレーム乱れるので、保存して復元する。
    auto savedCB         = targets.advancedGraphicsCB;
    auto savedPrevVP     = targets.prevViewProjection;
    auto savedPrevInvVP  = targets.invPrevViewProjection;
    auto savedTaaIndex   = targets.taaFrameIndex;
    /// @note フロクセル霧のボリュームは解像度非依存なので、リサイズで作り直さない。
    /// @note 作り直すと 14MB の確保が走ってフレームが飛ぶうえ、履歴が切れて霧が 1 度暗転する。
    auto savedFroxelA     = targets.froxelScatter;
    auto savedFroxelB     = targets.froxelScatterHistory;
    auto savedFroxelInt   = targets.froxelIntegrated;
    const uint32_t savedFroxelGrid[3] = { targets.froxelGrid[0], targets.froxelGrid[1],
                                          targets.froxelGrid[2] };
    auto savedFroxelState = targets.froxelState;
    /// @note 露出の順応も解像度非依存。リサイズで捨てると画面が一瞬白飛び / 黒潰れする。
    auto savedExposureHistogram = targets.exposureHistogram;
    auto savedExposureResult    = targets.exposureResult;
    const uint32_t savedExposureGeneration = targets.exposureResetGeneration;
    /// @note 雲の作業 RT は解像度依存。`targets = {}` で握ったまま忘れると漏れるので先に返す。
    targets.cloudRT.Release(resources);
    targets.waterSceneColorRT.Release(resources);
    targets.waterSceneDepthRT.Release(resources);
    targets.particleSceneColorRT.Release(resources);
    targets.particleOverdrawRT.Release(resources);
    targets.particleReactiveRT.Release(resources);
    targets.causticsDepthRT.Release(resources);
    targets.cloudDepthRT.Release(resources);
    targets = {};
    targets.exposureHistogram       = savedExposureHistogram;
    targets.exposureResult          = savedExposureResult;
    targets.exposureResetGeneration = savedExposureGeneration;
    targets.froxelScatter        = savedFroxelA;
    targets.froxelScatterHistory = savedFroxelB;
    targets.froxelIntegrated     = savedFroxelInt;
    targets.froxelGrid[0] = savedFroxelGrid[0];
    targets.froxelGrid[1] = savedFroxelGrid[1];
    targets.froxelGrid[2] = savedFroxelGrid[2];
    targets.froxelState   = savedFroxelState;
    targets.advancedGraphicsCB  = savedCB;
    targets.prevViewProjection    = savedPrevVP;
    targets.invPrevViewProjection = savedPrevInvVP;
    targets.taaFrameIndex         = savedTaaIndex;
}

void AccumulateBounds(SceneShadowBounds& aggregate, const WorldBounds& bounds)
{
    if (bounds.radius <= 0.0f) return;
    if (!aggregate.valid) {
        aggregate.center = bounds.center;
        aggregate.radius = bounds.radius;
        aggregate.valid = true;
        return;
    }

    const math::Vector3 delta = bounds.center - aggregate.center;
    const float distance = delta.Length();
    if (distance + bounds.radius <= aggregate.radius) return;
    if (distance + aggregate.radius <= bounds.radius) {
        aggregate.center = bounds.center;
        aggregate.radius = bounds.radius;
        return;
    }

    const float newRadius = (aggregate.radius + distance + bounds.radius) * 0.5f;
    if (distance > 0.0001f) {
        aggregate.center = aggregate.center + delta * ((newRadius - aggregate.radius) / distance);
    }
    aggregate.radius = newRadius;
}

bool ComputeTerrainWorldBounds(const Transform& transform, const TerrainComponent& terrain, WorldBounds& outBounds)
{
    if (!terrain.enabled || terrain.heightData.empty() || terrain.columns < 2 || terrain.rows < 2)
        return false;

    auto [minIt, maxIt] = std::minmax_element(terrain.heightData.begin(), terrain.heightData.end());
    const float minY = (*minIt) * terrain.maxHeight;
    const float maxY = (*maxIt) * terrain.maxHeight;
    const math::Vector3 localCenter = {
        static_cast<float>(terrain.columns - 1) * terrain.cellSize * 0.5f,
        (minY + maxY) * 0.5f,
        static_cast<float>(terrain.rows - 1) * terrain.cellSize * 0.5f
    };
    const math::Vector3 localExtents = {
        static_cast<float>(terrain.columns - 1) * terrain.cellSize * 0.5f,
        (maxY - minY) * 0.5f,
        static_cast<float>(terrain.rows - 1) * terrain.cellSize * 0.5f
    };

    const math::Vector4 worldCenter = transform.GetWorldMatrix()
        * math::Vector4{ localCenter.x, localCenter.y, localCenter.z, 1.0f };
    const float maxScale = (std::max)(
        (std::max)(std::abs(transform.worldScale.x), std::abs(transform.worldScale.y)),
        std::abs(transform.worldScale.z));

    outBounds.center = { worldCenter.x, worldCenter.y, worldCenter.z };
    outBounds.radius = localExtents.Length() * (std::max)(maxScale, 0.0001f);
    return outBounds.radius > 0.0f;
}

/// @note 視錐台スライス [nearZ, farZ] の外接球。centerDistance はカメラ前方への距離。
/// @note 8 頂点へ合わせるとカメラの回転で箱の大きさが変わり、影の精細度が脈動する。
/// @note 外接球の半径は向きに依存しない。
struct FrustumSliceSphere {
    float centerDistance = 0.0f;
    float radius         = 0.0f;
};

FrustumSliceSphere ComputeFrustumSliceSphere(const renderer::Camera& camera,
                                             float nearZ, float farZ)
{
    constexpr float DEG_TO_RAD = 0.01745329251994329577f;
    nearZ = (std::max)(nearZ, 0.01f);
    farZ  = (std::max)(farZ, nearZ + 0.01f);

    /// @note 平行投影は錐台ではなく直方体なので、対角の傾きという概念が無い。
    /// @note スライスの外接球は「中央 + 半対角」でそのまま求まる。
    if (camera.m_projection == renderer::ProjectionMode::Orthographic) {
        const float halfH = (std::max)(camera.m_orthoHeight, 0.01f) * 0.5f;
        const float halfW = halfH * camera.m_aspect;
        const float halfD = (farZ - nearZ) * 0.5f;
        FrustumSliceSphere box;
        box.centerDistance = (nearZ + farZ) * 0.5f;
        box.radius = std::sqrt(halfW * halfW + halfH * halfH + halfD * halfD);
        return box;
    }

    /// @note 視錐台の対角方向の傾き。k = |(±aspect*t, ±t, 1)| の xy 成分の長さ。
    const float tanHalfFov = std::tan(camera.m_fovY * 0.5f * DEG_TO_RAD);
    const float k  = tanHalfFov * std::sqrt(1.0f + camera.m_aspect * camera.m_aspect);
    const float k2 = k * k;

    FrustumSliceSphere sphere;
    /// @note near 面が far 面より広いほど中心は手前へ寄る。k² が十分大きいときは
    /// @note far 面の外接円がスライス全体を包むので、中心は far 面上に載る。
    if (k2 >= (farZ - nearZ) / (farZ + nearZ)) {
        sphere.centerDistance = farZ;
        sphere.radius         = farZ * k;
        return sphere;
    }

    const float sum  = farZ + nearZ;
    const float diff = farZ - nearZ;
    sphere.centerDistance = 0.5f * sum * (1.0f + k2);
    sphere.radius = 0.5f * std::sqrt(diff * diff
                                   + 2.0f * (farZ * farZ + nearZ * nearZ) * k2
                                   + sum * sum * k2 * k2);
    return sphere;
}

/// @note practical split scheme。対数分割 (手前を細かく) と等分割 (遠方を細かく) を lambda で補間する。
/// @note 対数だけだと遠景の影が溶け、等分だけだと足元が粗くなる。
/// @note outSplits[i] は「カスケード i が担当する far 距離」。outSplits[count-1] == shadowDistance。
void ComputeCascadeSplits(float nearZ, float shadowDistance, int cascadeCount,
                          float lambda, float* outSplits)
{
    const float clampedLambda = std::clamp(lambda, 0.0f, 1.0f);
    const float range         = shadowDistance - nearZ;

    for (int i = 0; i < cascadeCount; ++i) {
        const float ratio = static_cast<float>(i + 1) / static_cast<float>(cascadeCount);
        const float logSplit     = nearZ * std::pow(shadowDistance / nearZ, ratio);
        const float uniformSplit = nearZ + range * ratio;
        outSplits[i] = clampedLambda * logSplit + (1.0f - clampedLambda) * uniformSplit;
    }
    /// @note 丸め誤差で最遠が shadowDistance を下回ると、影の到達距離が設定より短くなる。
    outSplits[cascadeCount - 1] = shadowDistance;
}

SceneShadowBounds ComputeSceneShadowBounds(Scene& scene, fbzz::LayerMask cullingMask)
{
    SceneShadowBounds result{};
    for (auto& go : scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, cullingMask)) continue;

        if (auto* mr = go.GetComponent<MeshRenderer>()) {
            if (mr->enabled && mr->mesh && !mr->mesh->isSkinned && mr->mesh->boundsRadius > 0.0f)
                AccumulateBounds(result, ComputeWorldBounds(go.transform, *mr->mesh));
        }

        if (auto* smr = go.GetComponent<SkinnedMeshRenderer>()) {
            if (smr->enabled && smr->model) {
                WorldBounds bounds{};
                if (ComputeSkinnedWorldBounds(go, *smr, bounds))
                    AccumulateBounds(result, bounds);
            }
        }

        if (auto* terrain = go.GetComponent<TerrainComponent>()) {
            WorldBounds bounds{};
            if (ComputeTerrainWorldBounds(go.transform, *terrain, bounds))
                AccumulateBounds(result, bounds);
        }
    }
    return result;
}

} // namespace

void RenderSystem(Scene& scene,
                  renderer::IRenderer& renderer,
                  renderer::ResourceManager& resources,
                  const renderer::Camera& inputCamera,
                  renderer::ResourceHandle<renderer::RenderTargetTag> outputRT,
                  const renderer::RenderSettings* settings,
                  fbzz::LayerMask cullingMask,
                  const RenderSystemUIOptions* uiOptions,
                  const physics::World* physicsWorld,
                  const CameraCullingSettings* cullingSettings,
                  RenderPassCapture* capture)
{
    FBZZ_PROFILE_SCOPE("RenderSystem");

    /// @note カリング挙動は「明示指定 > シーンのメインカメラ > 既定値」の順で解決する。
    /// @note シーンから引くのは、RenderSystem を素で呼ぶ Standalone でも CameraComponent の設定を効かせるため。
    const CameraCullingSettings resolvedCulling =
        cullingSettings ? *cullingSettings : ResolveGameCullingSettings(scene);

    /// @note VFXCameraShake — VFX グラフの Camera Shake ノードによる揺れ。
    /// @note カメラ本体を書き換えると DebugCamera の yaw/pitch と乖離して操作が壊れる。
    /// @note 描画用のコピーだけをずらす。カリングも揺れた視点で行うので画面端の不整合も出ない。
    renderer::Camera shakenCamera = inputCamera;
    {
        math::Vector3 offset = math::Vector3::ZERO;
        float rollDegrees = 0.0f;
        /// @note 方向性のキックだけはワールド空間で積む。«どちらから押されたか» が本体なので、
        /// @note カメラのローカル軸へ畳むと向きの情報が消える。
        math::Vector3 worldKick = math::Vector3::ZERO;
        for (EntityID id : scene.GetEntities<VFXCameraShake>()) {
            GameObject* go       = scene.GetGameObject(id);
            auto*       shakePtr = scene.GetComponent<VFXCameraShake>(id);
            if (!go || !shakePtr || !go->activeInHierarchy()) continue;
            const VFXCameraShake& shake = *shakePtr;
            const float weight = shake.enabled ? std::clamp(shake.weight, 0.0f, 1.0f) : 0.0f;
            if (weight <= 0.0f) continue;
            /// @note 発生源から遠いほど弱める。radius <= 0 は距離減衰なし。
            float distanceScale = 1.0f;
            if (shake.radius > 0.0f) {
                const float distance = (go->transform.worldPosition - inputCamera.m_position).Length();
                distanceScale = std::clamp(1.0f - distance / shake.radius, 0.0f, 1.0f);
            }
            const float amount = weight * distanceScale;
            if (amount <= 0.0f) continue;
            /// @note 軸ごとに位相をずらした正弦の合成。決定論的で、フレームレートに依存しない。
            const float phase = shake.elapsed * shake.frequency;
            offset.x += std::sin(phase * 1.00f) * shake.amplitude * amount;
            offset.y += std::sin(phase * 1.37f + 1.7f) * shake.amplitude * amount;
            offset.z += std::sin(phase * 0.83f + 3.1f) * shake.amplitude * amount * 0.5f;
            rollDegrees += std::sin(phase * 1.11f + 0.6f) * shake.rotationAmplitude * amount;

            /// @note 方向性のキック。«発生源から見て押しのけられる» 向きへ 1 回だけ動かす。
            if (shake.kick != 0.0f) {
                math::Vector3 direction = shake.kickDirection;
                if (direction.LengthSq() <= 0.0001f)
                    direction = inputCamera.m_position - go->transform.worldPosition;
                /// @note 発生源とカメラが同じ点にあるときは «押す向き» が決まらない。
                /// @note 揺れだけを残し、キックは捨てる (前方へ倒すと爆心で毎回同じ癖が出る)。
                worldKick += direction.NormalizedOr(math::Vector3::ZERO) * (shake.kick * amount);
            }
        }
        if (offset.LengthSq() > 0.0f || worldKick.LengthSq() > 0.0f || rollDegrees != 0.0f) {
            constexpr float DEG_TO_RAD = 0.01745329251994329577f;
            /// @note オフセットはカメラのローカル軸で与え、向きに依らず自然に揺れるようにする。
            shakenCamera.m_position = inputCamera.m_position
                + inputCamera.GetRight() * offset.x
                + inputCamera.GetUp() * offset.y
                + inputCamera.GetForward() * offset.z
                + worldKick;
            shakenCamera.m_rotation = inputCamera.m_rotation
                * math::Quaternion::FromEuler({ 0.0f, 0.0f, rollDegrees * DEG_TO_RAD });
        }
    }
    const renderer::Camera& camera = shakenCamera;

    static renderer::RenderSettings sDefaultSettings;
    renderer::RenderSettings effectiveSettings = settings ? *settings : sDefaultSettings;

    /// @name ルック設定の解決
    /// @note ベースは VolumeSettings の既定値で、ボリュームが唯一の供給源
    /// @note (ProjectSettings からポストプロセスと高度グラフィクスの公開は撤去済み)。
    /// @note 隠れた全体ベースを持つと、同じプロファイルが別プロジェクトで違う絵になる。
    renderer::VolumeSettings volumeSettings;
    if (const auto* runtimePostProcess = scene.TryGetRuntimePostProcessSettings())
        volumeSettings.post = *runtimePostProcess;

    /// @name コンポーネントによる設定上書き (ProjectSettings < runtimePostProcess < Component)
    /// @note View<> を使わないのは GameObject が取れず activeInHierarchy() を見られないため。
    /// @note enabled (コンポーネントを切る) と activeInHierarchy() (オブジェクトごと切る) の
    /// @note どちらでも絵から消える必要がある。
    ///
    /// @note EnvironmentLightComponent — シーン Inspector から IBL を上書きする。先着優先。
    /// @note 空連動 IBL: 採用された EnvironmentLight の source
    IblSource activeIblSource = IblSource::StaticDDS;
    for (EntityID id : scene.GetEntities<EnvironmentLightComponent>()) {
        GameObject* go     = scene.GetGameObject(id);
        auto*       elcPtr = scene.GetComponent<EnvironmentLightComponent>(id);
        if (!go || !elcPtr || !go->activeInHierarchy() || !elcPtr->enabled) continue;
        const EnvironmentLightComponent& elc = *elcPtr;
        activeIblSource = elc.source;
        effectiveSettings.ibl.enabled       = elc.source == IblSource::DynamicSky
            || (!elc.irradiancePath.empty() && !elc.prefilterPath.empty());
        effectiveSettings.ibl.irradiancePath = elc.irradiancePath;
        effectiveSettings.ibl.prefilterPath  = elc.prefilterPath;
        effectiveSettings.ibl.intensity      = elc.intensity;
        effectiveSettings.ibl.diffuseScale   = elc.diffuseScale;
        effectiveSettings.ibl.specularScale  = elc.specularScale;
        effectiveSettings.ibl.maxMipLevel    = elc.maxMipLevel;
        break;
    }
    /// @note AtmosphericScatteringComponent — シーン Inspector から霧設定を上書きする。
    for (EntityID id : scene.GetEntities<AtmosphericScatteringComponent>()) {
        GameObject* go     = scene.GetGameObject(id);
        auto*       atmPtr = scene.GetComponent<AtmosphericScatteringComponent>(id);
        if (!go || !atmPtr || !go->activeInHierarchy() || !atmPtr->enabled) continue;
        const AtmosphericScatteringComponent& atm = *atmPtr;
        auto& fog      = volumeSettings.post.fog;
        fog.enabled    = atm.fogEnabled;
        fog.source     = static_cast<int>(atm.fogSource);
        fog.density    = atm.fogDensity;
        fog.farDistance = atm.fogFar;
        fog.color[0]   = atm.fogColor.x;
        fog.color[1]   = atm.fogColor.y;
        fog.color[2]   = atm.fogColor.z;
        break;
    }
    /// @name PostProcessVolumeComponent の合成
    /// @note ベースの上に、有効なボリュームを priority 昇順で重み付きブレンドする。
    /// @note 走査順に任せると GameObject を作り直しただけで重なり順が変わる。
    {
        struct VolumeEntry {
            const PostProcessVolumeComponent* volume  = nullptr;
            const asset::PostProcessProfile*  profile = nullptr;
            float weight = 0.0f;
        };
        std::vector<VolumeEntry> entries;

        /// @note 距離判定はシェイク適用後のカメラ位置で行う。
        /// @note シェイク量は influenceRadius に対して無視できるので、視点を使い分けない。
        const math::Vector3 viewPosition = camera.m_position;

        for (EntityID id : scene.GetEntities<PostProcessVolumeComponent>()) {
            GameObject* go     = scene.GetGameObject(id);
            auto*       ppvPtr = scene.GetComponent<PostProcessVolumeComponent>(id);
            if (!go || !ppvPtr || !go->activeInHierarchy() || !ppvPtr->enabled) continue;
            const PostProcessVolumeComponent& ppv = *ppvPtr;

            /// @note プロファイル未アサイン / 参照切れのボリュームは何も適用しない。
            /// @note 既定値へ倒すと「素のルック」を主張して priority 次第で他を打ち消す。
            const asset::PostProcessProfile* resolved = ppv.Resolve();
            if (!resolved) continue;

            float weight = std::clamp(ppv.blendWeight, 0.0f, 1.0f);
            if (!ppv.isGlobal) {
                const float distance = (viewPosition - go->transform.worldPosition).Length();
                weight *= renderer::PostProcessVolumeDistanceWeight(
                    distance, ppv.influenceRadius, ppv.blendDistance);
            }
            if (weight <= 0.0f) continue;

            entries.push_back({ &ppv, resolved, weight });
        }

        /// @note priority 昇順。同値は安定ソートで走査順を保つ (再現性のため)。
        std::stable_sort(entries.begin(), entries.end(),
            [](const VolumeEntry& lhs, const VolumeEntry& rhs) {
                return lhs.volume->priority < rhs.volume->priority;
            });

        for (const VolumeEntry& entry : entries) {
            /// @note 持っているオーバーライドだけが混ざり、載っていない効果は素通し。
            /// @note 「洞窟プロファイルは Fog と Color Grading だけ持つ」差分オーサリングが成立する。
            entry.profile->ApplyTo(volumeSettings, entry.weight);
        }

        /// @note 以降のコードは effectiveSettings.postProcess や .ssr をフラットに読む。
        renderer::ApplyVolumeSettings(volumeSettings, effectiveSettings);
    }
    /// @note VFXScreenEffect — VFX グラフの ScreenEffect ノードが出す一時的な画面演出。
    /// @note PostProcessVolume は「設定の差し替え」なので一瞬の上乗せや同時発生を表現できない。
    /// @note 解決済み設定へ後段で加算する。フラッシュだけは飽和するので最大値を採る。
    {
        renderer::PostProcessSettings& pp = effectiveSettings.postProcess;
        float strongestFlash = 0.0f;
        /// @note 輪だけは «一番強い 1 枚» を採る。中心と半径を足すと、2 つの爆発が
        /// @note «画面のどこにも無い中心を持つ 1 つの輪» に化ける。
        float strongestRing = 0.0f;
        /// @note 露出・色は «押し» の合計。掛け算ではなく加算なので、同時に走った演出は
        /// @note それぞれのぶんだけ深くなる (0 が «素» になる設計)。
        float exposureOffset = 0.0f;
        float saturationOffset = 0.0f;
        float contrastOffset = 0.0f;
        for (EntityID id : scene.GetEntities<VFXScreenEffect>()) {
            GameObject* go        = scene.GetGameObject(id);
            auto*       effectPtr = scene.GetComponent<VFXScreenEffect>(id);
            if (!go || !effectPtr || !go->activeInHierarchy()) continue;
            const VFXScreenEffect& effect = *effectPtr;
            const float weight = effect.enabled ? std::clamp(effect.weight, 0.0f, 1.0f) : 0.0f;
            if (weight <= 0.0f) continue;
            if (effect.bloomBoost > 0.0f) {
                pp.bloom.enabled = true;
                pp.bloom.intensity += effect.bloomBoost * weight;
            }
            if (effect.chromaticAberration > 0.0f) {
                pp.lens.chromaticAberrationEnabled = true;
                pp.lens.chromaticAberration += effect.chromaticAberration * weight;
            }
            if (effect.lensDistortion != 0.0f) {
                pp.lens.distortionEnabled = true;
                pp.lens.distortion += effect.lensDistortion * weight;
            }
            if (effect.vignette > 0.0f) {
                pp.vignette.enabled = true;
                pp.vignette.intensity += effect.vignette * weight;
            }
            /// @note 放射ブラーは «有効フラグ» を持たない。0 のときシェーダー側が
            /// @note 早期 return するので、加算するだけで «掛かっていない» が成立する。
            if (effect.radialBlur > 0.0f)
                pp.lens.radialBlur += effect.radialBlur * weight;
            /// @note 輪は «進捗» で外へ走る。progress=0 (窓の頭 / 配り手が居ない) では
            /// @note 半径 0 の点になってしまうので、そのフレームは掛けない。
            const float ring = effect.shockRingAmplitude * weight;
            if (ring > strongestRing && effect.progress > 0.0f) {
                strongestRing = ring;
                pp.lens.shockRingAmplitude = ring;
                pp.lens.shockRingRadius = effect.shockRingRadius * std::clamp(effect.progress, 0.0f, 1.0f);
                pp.lens.shockRingWidth = effect.shockRingWidth;
                pp.lens.shockRingCenter[0] = effect.shockRingCenter.x;
                pp.lens.shockRingCenter[1] = effect.shockRingCenter.y;
            }
            exposureOffset += effect.exposureOffset * weight;
            saturationOffset += effect.saturationOffset * weight;
            contrastOffset += effect.contrastOffset * weight;
            const float flash = effect.flashIntensity * weight;
            if (flash > strongestFlash) {
                strongestFlash = flash;
                pp.screenFadeColor[0] = effect.flashColor.x;
                pp.screenFadeColor[1] = effect.flashColor.y;
                pp.screenFadeColor[2] = effect.flashColor.z;
            }
        }
        if (strongestFlash > 0.0f)
            pp.screenFadeAlpha = std::clamp(pp.screenFadeAlpha + strongestFlash, 0.0f, 1.0f);
        if (exposureOffset != 0.0f)
            pp.exposure = (std::max)(pp.exposure + exposureOffset, 0.0f);
        if (saturationOffset != 0.0f || contrastOffset != 0.0f) {
            /// @note 切ってあったグレーディングを «押し» のために点けるときは、必ず素の値から
            /// @note 始める。プロファイルが書いた値がぶら下がったまま有効になると、
            /// @note 止めの一瞬だけ «誰も指示していない色» へ飛ぶ。
            if (!pp.colorGrading.enabled) {
                pp.colorGrading = renderer::ColorGradingSettings{};
                pp.colorGrading.enabled = true;
            }
            pp.colorGrading.saturation = (std::max)(pp.colorGrading.saturation + saturationOffset, 0.0f);
            pp.colorGrading.contrast += contrastOffset;
        }
    }
    /// @note ReflectionProbeComponent は将来の局所反射ブレンド実装で使用予定。
    /// @note 現時点は Inspector / Serializer のみ対応し、RenderSystem での適用は未実装。

    const renderer::RenderSettings& rs = effectiveSettings;

    /// @note 静的ハンドルの検証・デバイスリセット復旧・初回バッファ生成をまとめて計測する。
    profiler::Profiler::BeginSample(
        profiler::ProfilerMarker("RenderSystem::StaticResourceSetup", "Rendering"));

    /// @note 静的リソースの遅延初期化
    static uint64_t sResourceResetVersion = resources.GetResetVersion();
    /// @note シャドウアトラスは «解像度が動く» リソース (画質プリセットとエディタの Play/Stop)。
    /// @note 作り直しと解放は SizedRenderTarget に任せる ─ 自前で書くと、返し忘れた 1 か所が
    /// @note そのまま «ShadowPass だけ突然重い» になる。
    static renderer::SizedRenderTarget shadowMapRT;
    /// @note Spot / Point 用のシャドウアトラス。Directional の CSM とは面積を共有しない。
    static renderer::SizedRenderTarget punctualShadowRT;
    /// @note ライト Cookie を敷き詰めるアトラス。寸法は固定なので作り直しは起きない。
    static renderer::SizedRenderTarget lightCookieRT;
    static auto cookieBlitShader =
        resources.LoadShader("Assets/Shaders/Pipeline/Lighting/CookieBlit.hlsl");
    static auto shadowShader         = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMap.hlsl");
    /// @note 束ねた caster 用の変種。@see Docs/design/gpu-instancing.md
    static auto shadowInstancedShader =
        resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMapInstanced.hlsl");
    static auto skinnedShadowShader  = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/SkinnedShadowMap.hlsl");
    static auto velocityShader        = resources.LoadShader("Assets/Shaders/Motion/Velocity.hlsl");
    /// @note 束ねた物体の速度用の変種。@see Docs/design/gpu-instancing.md
    static auto velocityInstancedShader =
        resources.LoadShader("Assets/Shaders/Motion/VelocityInstanced.hlsl");
    static auto velocitySkinnedShader = resources.LoadShader("Assets/Shaders/Motion/VelocitySkinned.hlsl");
    /// @note コンピュートスキニング。無効ならスキンド描画は従来の VS スキニング経路へ落ちる。
    static auto skinningComputeCS    = resources.LoadShader("Assets/Shaders/Pipeline/Skinning/SkinningCompute.cs.hlsl");

    /// @note スキンドメッシュに AnimatorComponent がない場合のアイデンティティボーンパレット
    static renderer::ResourceHandle<renderer::ConstantBufferTag> bindPoseSkinningCB;
    if (!bindPoseSkinningCB.IsValid()) {
        struct BindPoseData { math::Matrix4 bones[asset::MAX_SKINNING_BONES]; };
        BindPoseData bp{};
        for (auto& m : bp.bones) m = math::Matrix4::Identity();
        bindPoseSkinningCB = resources.CreateConstantBuffer(sizeof(BindPoseData));
        resources.Update(bindPoseSkinningCB, &bp, sizeof(BindPoseData));
    }

    static auto compositeShader         = resources.LoadShader("Assets/Shaders/PostProcess/Color/Composite.hlsl");
    static auto causticsShader          = resources.LoadShader("Assets/Shaders/PostProcess/Water/Caustics.hlsl");
    static auto volumetricCloudShader   = resources.LoadShader("Assets/Shaders/PostProcess/Cloud/VolumetricCloud.hlsl");
    static auto cloudUpscaleShader      = resources.LoadShader("Assets/Shaders/PostProcess/Cloud/CloudUpscale.hlsl");
    static auto ssaoShader              = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAO.cs.hlsl");
    static auto ssaoBlurShader          = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAOBlur.cs.hlsl");
    static auto bloomDownShader         = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomDownsample.cs.hlsl");
    static auto bloomUpShader           = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomUpsample.cs.hlsl");
    static auto selectionMaskShader     = resources.LoadShader("Assets/Shaders/Debug/SelectionMask.hlsl");
    static auto selectionMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskSkinnedMesh.hlsl");
    static auto selectionMaskParticleShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskParticle.hlsl");
    static auto selectionMaskParticleGpuShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskParticleGPU.hlsl");
    static auto selectionOutlineShader  = resources.LoadShader("Assets/Shaders/PostProcess/Outline/SelectionOutline.hlsl");
    static auto objectMaskShader       = resources.LoadShader("Assets/Shaders/Pipeline/Mask/ObjectMask.hlsl");
    /// @note 束ねたシルエット用の変種。@see Docs/design/gpu-instancing.md
    static auto objectMaskInstancedShader =
        resources.LoadShader("Assets/Shaders/Pipeline/Mask/ObjectMaskInstanced.hlsl");
    static auto objectMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Pipeline/Mask/ObjectMaskSkinned.hlsl");
    static auto copyColorShader         = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
    static auto customComposeShader     = resources.LoadShader("Assets/Shaders/PostProcess/Custom/CustomCompose.hlsl");
    static auto fxaaShader              = resources.LoadShader("Assets/Shaders/PostProcess/AntiAliasing/FXAA.hlsl");
    static auto upscaleShader           = resources.LoadShader("Assets/Shaders/PostProcess/Upscale/Upscale.hlsl");
    static auto downscaleShader         = resources.LoadShader("Assets/Shaders/PostProcess/Upscale/Downscale.hlsl");

    /// @name Advanced Graphics シェーダー (static で初回ロード、Reset 後に再ロード)
    static auto iblBrdfBakeShader   = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/BRDFIntegration.cs.hlsl");
    static auto gtaoShader          = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/GTAO.cs.hlsl");
    static auto gtaoBlurShader      = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/GTAOBlur.cs.hlsl");
    static auto ssrShader           = resources.LoadShader("Assets/Shaders/PostProcess/Reflections/SSR.cs.hlsl");
    static auto volumetricShader    = resources.LoadShader("Assets/Shaders/PostProcess/Lighting/VolumetricLight.cs.hlsl");
    static auto contactShadowShader = resources.LoadShader("Assets/Shaders/PostProcess/Shadow/ContactShadows.cs.hlsl");
    static auto taaShader           = resources.LoadShader("Assets/Shaders/PostProcess/AntiAliasing/TAA.hlsl");
    static auto motionBlurShader    = resources.LoadShader("Assets/Shaders/PostProcess/Motion/MotionBlur.cs.hlsl");
    static auto lensFlareShader     = resources.LoadShader("Assets/Shaders/PostProcess/Flare/LensFlare.hlsl");
    /// @note BRDF LUT は 512x512 の定数テーブルで、解像度・シーンが変わっても内容は変わらない。
    /// @note 毎フレーム再生成するコストを避けるため static で一度だけ生成し、IBLBakePass でのみ書き込む。
    static auto iblBrdfLut          = resources.CreateComputeTexture(512, 512);
    static renderer::ResourceHandle<renderer::TextureTag> proceduralColorLut;
    static uint64_t proceduralColorLutHash = 0u;

    static auto skydomeShader = resources.LoadShader("Assets/Shaders/Material/Sky/Skydome.hlsl");
    static auto sunMoonShader = resources.LoadShader("Assets/Shaders/Material/Sky/SunMoon.hlsl");
    static auto skydomeMesh   = renderer::PrimitiveMesh::Sphere(resources, 32);

    /// @note 空連動 IBL (環境システム Phase A): 空を焼くキューブマップ RT と、面ごとの view/proj 用 CB。
    /// @note EnvironmentResources はフレームをまたいで保持し、SkyRenderer が変化した時だけ再キャプチャする。
    static constexpr uint32_t kSkyEnvCubeSize = 128;
    static EnvironmentResources sEnvironmentResources;
    static uint64_t sEnvResetVersion = resources.GetResetVersion();
    static renderer::ResourceHandle<renderer::RenderTargetTag>   skyEnvCubeRT;
    static renderer::ResourceHandle<renderer::ConstantBufferTag> skyCaptureFrameCB;
    if (!skyEnvCubeRT.IsValid() || sEnvResetVersion != resources.GetResetVersion()) {
        sEnvResetVersion  = resources.GetResetVersion();
        skyEnvCubeRT      = resources.CreateCubemapRenderTarget(kSkyEnvCubeSize, 1);
        skyCaptureFrameCB = resources.CreateConstantBuffer(sizeof(PerFrameCB));
        /// @note リソース再生成後 (デバイスリセット等) は古い動的 IBL ハンドルが無効。クリアして焼き直す。
        sEnvironmentResources.skyEnvCube    = {};
        sEnvironmentResources.skyIrradiance = {};
        sEnvironmentResources.skyPrefilter  = {};
        sEnvironmentResources.needsConvolution = false;
        sEnvironmentResources.MarkDirty();
    }

    /// @note ボリューメトリック雲の 3D ノイズ (Shape 128³ + Detail 32³) を起動時に 1 回だけ CPU 焼きする。
    /// @note タイラブルなので WRAP サンプルで無限に並べられる。デバイスリセット後は SRV が無効になるため焼き直す。
    static renderer::ResourceHandle<renderer::TextureTag> cloudShapeTex;
    static renderer::ResourceHandle<renderer::TextureTag> cloudDetailTex;
    if (resources.Get(cloudShapeTex) == nullptr || resources.Get(cloudDetailTex) == nullptr) {
        const std::vector<uint8_t> shape  = cloudnoise::BakeShape(128);
        const std::vector<uint8_t> detail = cloudnoise::BakeDetail(32);
        cloudShapeTex  = resources.CreateTexture3D(shape.data(),  128, 128, 128);
        cloudDetailTex = resources.CreateTexture3D(detail.data(),  32,  32,  32);
        FBZZ_LOG_DEBUG("VolumetricCloud: baked tileable 3D noise (shape 128^3, detail 32^3)");
    }

    static auto gbufferShader          = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBuffer.hlsl");
    /// @note 束ねた不透明メッシュ用の変種。@see Docs/design/gpu-instancing.md
    static auto gbufferInstancedShader =
        resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBufferInstanced.hlsl");
    /// @note スキンドを GBuffer へ入れる経路のフォールバック。@see Docs/design/pipeline-boundary.md
    static auto gbufferSkinnedShader =
        resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBufferSkinned.hlsl");
    static auto deferredLightingShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DeferredLighting.hlsl");
    static auto depthCopyShader        = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");

    /// @note クラスタライトカリング (Forward+ / Deferred+)。
    static auto clusterCullCS = resources.LoadShader("Assets/Shaders/Pipeline/Clustered/ClusterLightCull.cs.hlsl");
    /// @note clusterIndexBuffer は解像度非依存の固定長なので確保は初回の 1 回だけ。
    /// @note CS が u2 へ書き PS が t30 から読むので RW。
    /// @note punctualLightBuffer (CPU が書いて GPU が読むだけ) は 1 枚を共有せず、描くたびに借りる。
    /// @note DX12 の読み取り専用 StructuredBuffer は Upload ヒープへの直 memcpy。1 枚だと
    /// @note Scene View の Draw が Game View の書いた配列を読み、GPU がまだ読んでいる
    /// @note 前フレームの配列も上書きする (ライトが増減したフレームにだけ幽霊が出る)。
    static renderer::DynamicStructuredBufferPool punctualLightPool;
    static auto clusterIndexBuffer = resources.CreateRWStructuredBuffer(
        nullptr, kClusterCount * kClusterStride, static_cast<uint32_t>(sizeof(uint32_t)));

    /// @note 自動露出のバッファはビュー単位 (ViewRenderTargets) で確保する。シェーダーだけ共有。
    static auto exposureHistogramCS =
        resources.LoadShader("Assets/Shaders/PostProcess/Color/ExposureHistogram.cs.hlsl");
    static auto exposureAverageCS =
        resources.LoadShader("Assets/Shaders/PostProcess/Color/ExposureAverage.cs.hlsl");

    /// @note フロクセル霧の CS。ボリューム本体はビュー単位なので、下の per-view ブロックで確保する。
    static auto froxelInjectCS =
        resources.LoadShader("Assets/Shaders/PostProcess/Cloud/FroxelInject.cs.hlsl");
    static auto froxelIntegrateCS =
        resources.LoadShader("Assets/Shaders/PostProcess/Cloud/FroxelIntegrate.cs.hlsl");

    /// @note Light Probe Volume の焼き。ボリュームと面の RT は各コンポーネントが持つので、ここは共有の CS と定数だけ。
    static auto lightProbeProjectCS =
        resources.LoadShader("Assets/Shaders/IBL/LightProbeProject.cs.hlsl");
    static auto lightProbeProjectCB         = resources.CreateConstantBuffer(kLightProbeProjectCBSize);
    static auto lightProbeCaptureFrameCB    = resources.CreateConstantBuffer(sizeof(PerFrameCB));
    static auto lightProbeCaptureAdvancedCB = resources.CreateConstantBuffer(sizeof(AdvancedGraphicsCB));
    static auto lightProbeDilateCS =
        resources.LoadShader("Assets/Shaders/IBL/LightProbeDilate.cs.hlsl");
    static auto lightProbeDilateCB     = resources.CreateConstantBuffer(kLightProbeDilateCBSize);
    static auto lightProbeFacingShader = resources.LoadShader("Assets/Shaders/IBL/LightProbeFacing.hlsl");
    static auto clusterCB = resources.CreateConstantBuffer(sizeof(ClusterConstantsCB));
    /// @note 別視点から描くパス用に、供給モードだけ Linear へ落とした同内容の CB。
    static auto clusterLinearCB = resources.CreateConstantBuffer(sizeof(ClusterConstantsCB));

    static auto decalShader     = resources.LoadShader("Assets/Shaders/Material/Decal/Decal.hlsl");
    static auto decalMaskShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMask.hlsl");
    static auto decalMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMaskSkinned.hlsl");

    static auto particleShader      = resources.LoadShader("Assets/Shaders/Material/Effects/Particle.hlsl");
    static auto particleGpuSimCS   = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSim.cs.hlsl");
    static auto particleGpuShader  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGPU.hlsl");
    /// @note GPU ソート 3 段 (キー生成 / グローバル段 / LDS 段)。sortMode != None のときだけ走る。
    static auto particleGpuSortKeysCS  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortKeys.cs.hlsl");
    static auto particleGpuSortStepCS  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortStep.cs.hlsl");
    static auto particleGpuSortLocalCS = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortLocal.cs.hlsl");
    static auto particleGpuMeshShader  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuMesh.hlsl");
    /// @note 自己影: 光源から見た密度を積む。selfShadowStrength > 0 のエミッターがあるときだけ走る。
    static auto particleSelfShadowShader = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleSelfShadowDensity.hlsl");
    static auto trailShader    = resources.LoadShader("Assets/Shaders/Material/Effects/Trail.hlsl");
    static auto meshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/MeshTrail.hlsl");
    static auto skinnedMeshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/SkinnedMeshTrail.hlsl");
    static auto frameCB    = resources.CreateConstantBuffer(sizeof(PerFrameCB));
    static auto objectCB   = resources.CreateConstantBuffer(sizeof(PerObjectCB));
    static auto lightCB    = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    static auto shadowCB   = resources.CreateConstantBuffer(sizeof(ShadowConstantsCB));
    static auto punctualShadowCB = resources.CreateConstantBuffer(sizeof(PunctualShadowConstantsCB));
    static auto cookieBlitCB     = resources.CreateConstantBuffer(sizeof(CookieBlitCB));
    static auto exposureCB       = resources.CreateConstantBuffer(sizeof(AutoExposureCB));
    static auto froxelFogCB      = resources.CreateConstantBuffer(sizeof(FroxelFogCB));
    /// @note コンピュートスキニングの b0 (頂点数のみ)。16 バイト境界へ切り上げられる。
    static auto skinningCB = resources.CreateConstantBuffer(16);
    static auto postprocCB = resources.CreateConstantBuffer(sizeof(PostProcCB));
    static auto outlineCB  = resources.CreateConstantBuffer(sizeof(OutlineCB));
    static auto objectMaskCB = resources.CreateConstantBuffer(sizeof(ObjectMaskCB));
    static auto atmCB      = resources.CreateConstantBuffer(sizeof(AtmosphereCB));
    static auto decalCB    = resources.CreateConstantBuffer(sizeof(DecalCB));
    static auto decalMaterialCB = resources.CreateConstantBuffer(sizeof(DecalMaterialCB));
    static auto decalReceiverCB = resources.CreateConstantBuffer(sizeof(DecalReceiverCB));
    static auto volumetricCloudCB = resources.CreateConstantBuffer(176);
    /// @note パーティクル自己影: 光源側の密度 RT と、光源行列を入れる専用 frame CB。
    /// @note RenderPassHandles は毎フレーム作り直される値型なので、パス側で遅延生成すると RT を漏らす。
    /// @note 解像度が固定なのは、拾うのが「煙の内部で光がどれだけ減るか」という低周波の情報だから。
    static renderer::SizedRenderTarget particleSelfShadowRT;
    static auto particleSelfShadowFrameCB = resources.CreateConstantBuffer(sizeof(PerFrameCB));

    /// @note static handle は通常フレームでは再利用し、ResourceManager::Reset() 後だけ世代差分で再生成する。
    /// @note これによりデバイスロスト復帰時も旧ネイティブリソースへ触らない。
    static auto defaultPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto wireframePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::WIREFRAME,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto selectionMaskPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto skydomePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_SKY
    });
    static auto sunMoonPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_SKY
    });
    static auto particlePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_READ
    });
    static auto particleAlphaPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    static auto particlePremultipliedPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::PREMULTIPLIED,
        renderer::DepthMode::DEPTH_READ
    });
    static auto particleGpuPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_READ
    });
    static auto particleGpuAlphaPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    static auto particleGpuPremultipliedPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::PREMULTIPLIED,
        renderer::DepthMode::DEPTH_READ
    });
    static auto trailPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    static auto meshTrailPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    static auto meshTrailDoubleSidedPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    static auto postprocPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    static auto causticsPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_OFF
    });
    static auto volumetricCloudPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    /// @note VolumetricCloud.hlsl は scatter.rgb に既に透過率を積分した premultiplied 値を返す。
    /// @note Overdraw 可視化も volumetricCloudPSO を共有するため、雲の合成だけ専用 PSO に分離する。
    static auto volumetricCloudPremultipliedPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::PREMULTIPLIED,
        renderer::DepthMode::DEPTH_OFF
    });
    /// @name Advanced Graphics PSO / 定数バッファ
    /// @note taaPSO: OPAQUE — TAA は ping-pong バッファへ上書きするため α ブレンドは不要
    static auto taaPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    /// @note lensFlarePSO: ADDITIVE — ゴーストはフレアを HDR バッファに加算合成する
    static auto lensFlarePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_OFF
    });
    static auto decalPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    static auto decalMaskPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });

    const bool resourcesWereReset = sResourceResetVersion != resources.GetResetVersion();

    /// @note GPU リソースが «増え続けていないか» をフレーム単位で見張る。
    /// @note 描画は 1 フレームに 1 度だけ通り、resources を持っているのでここに置く。
    resources.TickLeakWatchdog();

    /// @note シャドウアトラス。解像度は «動く» ─ 画質プリセットの適用 (GameSettings が各シーンの
    /// @note OnStart で行う) と、エディタの Play / Stop による RenderSettings の差し替えで、
    /// @note 1 回の試遊につき数回作り直される。1 枚で数十 MB あるので、返し忘れると数回の Play で
    /// @note 数百 MB 漏れ «ShadowPass だけ突然重い / エディタ再起動で直る» として出る (2026-09-01)。
    /// @note 解像度は CSM と punctual で別設定なので、それぞれ独立に作り直す。
    const uint32_t punctualShadowRes = (std::max)(rs.shadow.punctualMapResolution, 64u);
    (void)punctualShadowRT.Ensure(resources, punctualShadowRes, punctualShadowRes, 0);

    if (lightCookieRT.Ensure(resources, kLightCookieAtlasWidth, kLightCookieAtlasHeight, 1)) {
        cookieBlitShader =
            resources.LoadShader("Assets/Shaders/Pipeline/Lighting/CookieBlit.hlsl");
        /// @note アトラスの中身は作り直しで失われる。焼き直し済みの記録も捨てる。
        ReleaseLightCookieCache();
    }

    /// @note 下のシェーダー再ロードとは別に確保する。同居させると影解像度が 1 段変わるだけで
    /// @note シェーダー約 40 本・定数バッファ十数個・PSO・BRDF LUT まで作り直し、旧ハンドルが漏れる。
    (void)shadowMapRT.Ensure(resources, rs.shadow.mapResolution, rs.shadow.mapResolution, 0);

    (void)particleSelfShadowRT.Ensure(resources, RenderPassHandles::kSelfShadowResolution,
                                      RenderPassHandles::kSelfShadowResolution, 1);

    /// @note シェーダー / 定数バッファ / PSO の作り直しはデバイスリセット時と初回のみ通す。
    /// @note どちらも実体ごと失われるため Release は書かない。実体が生きているうちに通すと漏れる。
    static bool sStaticsLoaded = false;
    if (resourcesWereReset || !sStaticsLoaded) {
        sStaticsLoaded        = true;
        sResourceResetVersion = resources.GetResetVersion();

        shadowShader        = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMap.hlsl");
        shadowInstancedShader =
            resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMapInstanced.hlsl");
        skinnedShadowShader = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/SkinnedShadowMap.hlsl");
        velocityShader        = resources.LoadShader("Assets/Shaders/Motion/Velocity.hlsl");
        velocityInstancedShader = resources.LoadShader("Assets/Shaders/Motion/VelocityInstanced.hlsl");
        velocitySkinnedShader = resources.LoadShader("Assets/Shaders/Motion/VelocitySkinned.hlsl");
        skinningComputeCS   = resources.LoadShader("Assets/Shaders/Pipeline/Skinning/SkinningCompute.cs.hlsl");
        /// @note Mesh* / AnimatorComponent* をキーにしたキャッシュはリソースリセットで無効になる。
        ReleaseSkinningComputeCaches();
        /// @note インスタンスバッファの貸出プールも同じ理由で手放す。
        ReleaseInstanceBatchCaches(resources);
        compositeShader     = resources.LoadShader("Assets/Shaders/PostProcess/Color/Composite.hlsl");
        causticsShader      = resources.LoadShader("Assets/Shaders/PostProcess/Water/Caustics.hlsl");
        volumetricCloudShader = resources.LoadShader("Assets/Shaders/PostProcess/Cloud/VolumetricCloud.hlsl");
        cloudUpscaleShader    = resources.LoadShader("Assets/Shaders/PostProcess/Cloud/CloudUpscale.hlsl");
        ssaoShader          = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAO.cs.hlsl");
        ssaoBlurShader      = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAOBlur.cs.hlsl");
        bloomDownShader     = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomDownsample.cs.hlsl");
        bloomUpShader       = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomUpsample.cs.hlsl");
        selectionMaskShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMask.hlsl");
        selectionMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskSkinnedMesh.hlsl");
        selectionMaskParticleShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskParticle.hlsl");
        selectionMaskParticleGpuShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskParticleGPU.hlsl");
        selectionOutlineShader = resources.LoadShader("Assets/Shaders/PostProcess/Outline/SelectionOutline.hlsl");
        objectMaskShader = resources.LoadShader("Assets/Shaders/Pipeline/Mask/ObjectMask.hlsl");
        objectMaskInstancedShader =
            resources.LoadShader("Assets/Shaders/Pipeline/Mask/ObjectMaskInstanced.hlsl");
        objectMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Pipeline/Mask/ObjectMaskSkinned.hlsl");
        copyColorShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
        customComposeShader = resources.LoadShader("Assets/Shaders/PostProcess/Custom/CustomCompose.hlsl");
        fxaaShader = resources.LoadShader("Assets/Shaders/PostProcess/AntiAliasing/FXAA.hlsl");
        upscaleShader = resources.LoadShader("Assets/Shaders/PostProcess/Upscale/Upscale.hlsl");
        downscaleShader = resources.LoadShader("Assets/Shaders/PostProcess/Upscale/Downscale.hlsl");
        skydomeShader = resources.LoadShader("Assets/Shaders/Material/Sky/Skydome.hlsl");
        sunMoonShader = resources.LoadShader("Assets/Shaders/Material/Sky/SunMoon.hlsl");
        skydomeMesh   = renderer::PrimitiveMesh::Sphere(resources, 32);
        gbufferShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBuffer.hlsl");
        gbufferInstancedShader =
            resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBufferInstanced.hlsl");
        gbufferSkinnedShader =
            resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBufferSkinned.hlsl");
        deferredLightingShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DeferredLighting.hlsl");
        depthCopyShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
        clusterCullCS = resources.LoadShader("Assets/Shaders/Pipeline/Clustered/ClusterLightCull.cs.hlsl");
        decalShader = resources.LoadShader("Assets/Shaders/Material/Decal/Decal.hlsl");
        decalMaskShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMask.hlsl");
        decalMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMaskSkinned.hlsl");
        /// @note .mat の解決結果はシェーダー・テクスチャ・cbuffer のハンドルを持つ。
        ReleaseDecalMaterialCache();
        ReleaseCustomPassMaterialCache();
        particleShader     = resources.LoadShader("Assets/Shaders/Material/Effects/Particle.hlsl");
        particleGpuSimCS  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSim.cs.hlsl");
        particleGpuShader = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGPU.hlsl");
        particleGpuSortKeysCS  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortKeys.cs.hlsl");
        particleGpuSortStepCS  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortStep.cs.hlsl");
        particleGpuSortLocalCS = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortLocal.cs.hlsl");
        particleGpuMeshShader  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuMesh.hlsl");
        particleSelfShadowShader = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleSelfShadowDensity.hlsl");
        trailShader = resources.LoadShader("Assets/Shaders/Material/Effects/Trail.hlsl");
        meshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/MeshTrail.hlsl");
        skinnedMeshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/SkinnedMeshTrail.hlsl");

        /// @name Advanced Graphics: デバイスリセット後に再ロード
        iblBrdfBakeShader   = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/BRDFIntegration.cs.hlsl");
        gtaoShader          = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/GTAO.cs.hlsl");
        gtaoBlurShader      = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/GTAOBlur.cs.hlsl");
        ssrShader           = resources.LoadShader("Assets/Shaders/PostProcess/Reflections/SSR.cs.hlsl");
        volumetricShader    = resources.LoadShader("Assets/Shaders/PostProcess/Lighting/VolumetricLight.cs.hlsl");
        contactShadowShader = resources.LoadShader("Assets/Shaders/PostProcess/Shadow/ContactShadows.cs.hlsl");
        taaShader           = resources.LoadShader("Assets/Shaders/PostProcess/AntiAliasing/TAA.hlsl");
        motionBlurShader    = resources.LoadShader("Assets/Shaders/PostProcess/Motion/MotionBlur.cs.hlsl");
        lensFlareShader     = resources.LoadShader("Assets/Shaders/PostProcess/Flare/LensFlare.hlsl");
        taaPSO              = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF });
        lensFlarePSO        = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::ADDITIVE,      renderer::DepthMode::DEPTH_OFF });
        /// @note デバイスリセット後は iblBrdfLut の内容が失われるため再生成する。
        /// @note IBLBakePass は焼き済み対象を世代付きハンドルで追跡し、新ハンドルを次フレームで再生成する。
        iblBrdfLut          = resources.CreateComputeTexture(512, 512);

        bindPoseSkinningCB = {};
        {
            struct BindPoseData { math::Matrix4 bones[asset::MAX_SKINNING_BONES]; };
            BindPoseData bp{};
            for (auto& m : bp.bones) m = math::Matrix4::Identity();
            bindPoseSkinningCB = resources.CreateConstantBuffer(sizeof(BindPoseData));
            resources.Update(bindPoseSkinningCB, &bp, sizeof(BindPoseData));
        }
        /// @note モデル側のリファレンスポーズ CB もデバイスリセットで失われる。
        /// @note ハンドルを落としておけば下の遅延生成が次フレームで作り直す。
        for (auto& go : scene.GameObjects()) {
            if (auto* smr = go.GetComponent<SkinnedMeshRenderer>())
                if (smr->model) smr->model->referencePoseCB = {};
        }
        frameCB    = resources.CreateConstantBuffer(sizeof(PerFrameCB));
        objectCB   = resources.CreateConstantBuffer(sizeof(PerObjectCB));
        lightCB    = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
        shadowCB   = resources.CreateConstantBuffer(sizeof(ShadowConstantsCB));
        punctualShadowCB = resources.CreateConstantBuffer(sizeof(PunctualShadowConstantsCB));
        cookieBlitCB = resources.CreateConstantBuffer(sizeof(CookieBlitCB));
        exposureCB   = resources.CreateConstantBuffer(sizeof(AutoExposureCB));
        froxelFogCB  = resources.CreateConstantBuffer(sizeof(FroxelFogCB));
        postprocCB = resources.CreateConstantBuffer(sizeof(PostProcCB));
        outlineCB  = resources.CreateConstantBuffer(sizeof(OutlineCB));
        objectMaskCB = resources.CreateConstantBuffer(sizeof(ObjectMaskCB));
        atmCB      = resources.CreateConstantBuffer(sizeof(AtmosphereCB));
        decalCB    = resources.CreateConstantBuffer(sizeof(DecalCB));
        decalMaterialCB = resources.CreateConstantBuffer(sizeof(DecalMaterialCB));
        decalReceiverCB = resources.CreateConstantBuffer(sizeof(DecalReceiverCB));
        volumetricCloudCB = resources.CreateConstantBuffer(176);
        particleSelfShadowFrameCB = resources.CreateConstantBuffer(sizeof(PerFrameCB));

        defaultPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        wireframePSO = resources.CreatePipelineState({ renderer::RasterizerMode::WIREFRAME, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        selectionMaskPso = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        skydomePSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_SKY });
        sunMoonPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ADDITIVE, renderer::DepthMode::DEPTH_SKY });
        particlePSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ADDITIVE, renderer::DepthMode::DEPTH_READ });
        particleAlphaPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        particlePremultipliedPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::PREMULTIPLIED, renderer::DepthMode::DEPTH_READ });
        particleGpuPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ADDITIVE, renderer::DepthMode::DEPTH_READ });
        particleGpuAlphaPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        particleGpuPremultipliedPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::PREMULTIPLIED, renderer::DepthMode::DEPTH_READ });
        trailPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        meshTrailPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        meshTrailDoubleSidedPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        postprocPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF });
        causticsPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::ADDITIVE, renderer::DepthMode::DEPTH_OFF });
        volumetricCloudPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_OFF });
        volumetricCloudPremultipliedPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::PREMULTIPLIED, renderer::DepthMode::DEPTH_OFF });
        decalPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_OFF });
        decalMaskPso = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF });
    }

    /// @note パーティクルのクワッド用インデックスバッファ (最大描画数分を事前確保)。
    /// @note 頂点側は共有せず、描画時に DynamicVertexBufferPool から 1 エミッターぶんずつ借りる。
    static renderer::ResourceHandle<renderer::BufferTag> particleIB;
    static uint64_t sParticleResetVersion = 0;
    if (sParticleResetVersion != resources.GetResetVersion()) {
        particleIB = {};
        sParticleResetVersion = resources.GetResetVersion();
    }
    if (!particleIB.IsValid())
    {
        std::vector<uint32_t> idx;
        idx.reserve(kMaxParticleDraw * 6);
        for (int i = 0; i < kMaxParticleDraw; ++i) {
            uint32_t b = static_cast<uint32_t>(i * 4);
            idx.insert(idx.end(), { b, b+1, b+2, b+1, b+3, b+2 });
        }
        particleIB = resources.CreateIndexBuffer(idx.data(), static_cast<uint32_t>(idx.size()));
    }

    profiler::Profiler::EndSample();

    /// @note UI の描画先種別を安定キーにして、Scene / Game の中間リソースを分離する。
    /// @note uiOptions がない通常ゲーム描画は key=0 の単一コンテキストを使う。
    const uint32_t viewKey = uiOptions
        ? static_cast<uint32_t>(uiOptions->targetView) + 1u
        : 0u;
    static std::unordered_map<uint32_t, ViewRenderTargets> s_viewTargets;
    static uint64_t sRenderTargetResetVersion = 0;
    if (sRenderTargetResetVersion != resources.GetResetVersion()) {
        /// @note ResourceManager::Reset() 後は旧ハンドルが無効なので Release せずキャッシュだけ破棄する。
        s_viewTargets.clear();
        sRenderTargetResetVersion = resources.GetResetVersion();
    }

    ViewRenderTargets& viewTargets = s_viewTargets[viewKey];
    auto& hdrRT                   = viewTargets.hdr;
    auto& ldrRT                   = viewTargets.ldr;
    auto& selectionMaskRT         = viewTargets.selectionMask;
    auto& outlineRT               = viewTargets.outline;
    auto& objectMaskRT           = viewTargets.objectMask;
    auto& customPostProcessRT     = viewTargets.customPostProcess;
    auto& upscaleSrcRT            = viewTargets.upscaleSrc;
    auto& gbufferRT               = viewTargets.gbuffer;
    auto& velocityRT              = viewTargets.velocity;
    auto& decalDepthRT            = viewTargets.decalDepth;
    auto& decalMaskRT             = viewTargets.decalMask;
    auto& bloomHalf               = viewTargets.bloomHalf;
    auto& bloomFull               = viewTargets.bloomFull;
    auto& ssaoRaw                 = viewTargets.ssaoRaw;
    auto& ssaoBlur                = viewTargets.ssaoBlur;
    auto& ssrResult               = viewTargets.ssrResult;
    auto& volumetricResult        = viewTargets.volumetricResult;
    auto& taaHistoryA             = viewTargets.taaHistoryA;
    auto& taaHistoryB             = viewTargets.taaHistoryB;
    auto& motionBlurResult        = viewTargets.motionBlurResult;
    auto& gtaoRaw                 = viewTargets.gtaoRaw;
    auto& gtaoBlur                = viewTargets.gtaoBlur;
    auto& contactShadowResult     = viewTargets.contactShadowResult;
    uint32_t& sHdrW               = viewTargets.width;
    uint32_t& sHdrH               = viewTargets.height;
    /// @note advancedGraphicsCB はビュー別に生成する。
    /// @note s_viewTargets.clear() によるデバイスリセット後は無効になるため、ここで lazily 再生成する。
    if (!viewTargets.advancedGraphicsCB.IsValid())
        viewTargets.advancedGraphicsCB = resources.CreateConstantBuffer(sizeof(AdvancedGraphicsCB));
    auto& advancedGraphicsCB = viewTargets.advancedGraphicsCB;
    /// @note 自動露出。ヒストグラムは「読んだ後に自分でクリアする」設計なので初回だけ 0 が要る
    /// @note (DEFAULT ヒープの初期内容は未定義)。
    if (!viewTargets.exposureHistogram.IsValid()) {
        const std::vector<uint32_t> zeros(kExposureHistogramBins, 0u);
        viewTargets.exposureHistogram = resources.CreateRWStructuredBuffer(
            zeros.data(), kExposureHistogramBins, static_cast<uint32_t>(sizeof(uint32_t)));
    }
    if (!viewTargets.exposureResult.IsValid()) {
        /// @note 負値は「まだ順応していない」の印。CS 側が reset と同じ扱いで拾う。
        const float initial = -1.0f;
        viewTargets.exposureResult =
            resources.CreateRWStructuredBuffer(&initial, 1, static_cast<uint32_t>(sizeof(float)));
    }

    /// @note 出力先の実寸。UI はこの寸法で描く (描画スケールの影響を受けない)。
    uint32_t nativeW = 0;
    uint32_t nativeH = 0;
    /// @note 内部解像度と出力先の実寸が食い違うフレームか。UpscalePass の要否そのもの。
    bool needsUpscale = false;
    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ResizeRenderTargets");
        const auto* output = resources.Get(outputRT);
        nativeW = output ? output->GetWidth()  : renderer.GetWidth();
        nativeH = output ? output->GetHeight() : renderer.GetHeight();
        if (nativeW == 0 || nativeH == 0) return;

        /// @note 内部描画解像度。ここで倍率を掛ければ中間 RT もビューポートも texelSize も追従する。
        /// @note 実寸へ戻すのは UpscalePass ただ 1 つ。ポストの各段が outputRT へ直接書くと、
        /// @note その段だけが実寸で走り、描画スケールで浮かせたはずのコストが最後に戻ってくる。
        uint32_t curW = 0;
        uint32_t curH = 0;
        renderer::ResolveRenderResolution(nativeW, nativeH, rs.renderScale, curW, curH);
        if (curW == 0 || curH == 0) return;
        if (!hdrRT.IsValid() || sHdrW != curW || sHdrH != curH)
        {
            /// @note 全画面ポストの中継先。深度テストも深度書き込みもしないので深度を持たない。
            /// @note 深度が要るのは «ジオメトリを描く RT» と «深度を SRV で読まれる RT» の 2 つだけで、
            /// @note 中継先はどちらでもない。1080p で 1 枚 8MB、ビューごとに 7 枚ぶん浮く。
            constexpr renderer::RenderTargetDesc kPostChainRT{
                /*colorCount=*/1, renderer::Format::RGBA16F, /*withDepth=*/false };
            /// @note カメラ視点の深度を持つ RT は Reversed-Z。GPU へ渡す射影 (Camera::GetGpuProjectionMatrix) と対。
            const auto cameraDepthRT = renderer::CameraDepthTargetDesc;

            ReleaseViewRenderTargets(viewTargets, resources);
            hdrRT           = resources.CreateRenderTarget(curW, curH, cameraDepthRT(1));
            ldrRT           = resources.CreateRenderTarget(curW, curH, kPostChainRT);
            /// @note 選択マスクと輪郭マスクはジオメトリを描き、深度も読まれる。
            selectionMaskRT = resources.CreateRenderTarget(curW, curH, cameraDepthRT(1));
            outlineRT       = resources.CreateRenderTarget(curW, curH, kPostChainRT);
            objectMaskRT   = resources.CreateRenderTarget(curW, curH, cameraDepthRT(1));
            customPostProcessRT[0] = resources.CreateRenderTarget(curW, curH, kPostChainRT);
            customPostProcessRT[1] = resources.CreateRenderTarget(curW, curH, kPostChainRT);
            gbufferRT       = resources.CreateRenderTarget(curW, curH, cameraDepthRT(2));
            velocityRT      = resources.CreateRenderTarget(curW, curH, cameraDepthRT(1));
            decalDepthRT    = resources.CreateRenderTarget(curW, curH, cameraDepthRT(0));
            decalMaskRT     = resources.CreateRenderTarget(curW, curH, 1);
            /// @note Bloom のミップ連鎖。段ごとに 1/2、1 まで来たら以降は同寸法のまま確保する。
            /// @note ダウンサンプル用と足し戻し用の 2 系統。足し戻しは「1 段小さいぼけ + 自分の段の元」
            /// @note を読んで書くので、読みながら書けない以上は書き先を分ける必要がある。
            for (uint32_t i = 0; i < kBloomMipCount; ++i) {
                /// @note 2, 4, 8, 16, 32
                const uint32_t div = 2u << i;
                const uint32_t mw = (std::max)(1u, curW / div);
                const uint32_t mh = (std::max)(1u, curH / div);
                viewTargets.bloomChain[i]   = resources.CreateComputeTexture(mw, mh);
                viewTargets.bloomUpChain[i] = resources.CreateComputeTexture(mw, mh);
            }
            bloomHalf       = viewTargets.bloomChain[0];
            bloomFull       = resources.CreateComputeTexture(curW, curH);
            /// @note AO / 接触影は低周波なので半解像度 (コスト約 1/4)。消費側が linear サンプルで戻す。
            /// @note SSR は鏡面が崩れるためフル解像度のまま。
            ssaoRaw         = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2));
            ssaoBlur        = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2));
            /// @name Advanced Graphics per-view テクスチャ
            ssrResult           = resources.CreateComputeTexture(curW, curH);
            volumetricResult    = resources.CreateComputeTexture(curW, curH);
            taaHistoryA         = resources.CreateRenderTarget(curW, curH, kPostChainRT);
            taaHistoryB         = resources.CreateRenderTarget(curW, curH, kPostChainRT);
            motionBlurResult    = resources.CreateComputeTexture(curW, curH);
            /// @note 半解像度 AO
            gtaoRaw             = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2));
            /// @note 半解像度 AO
            gtaoBlur            = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2));
            /// @note 半解像度 接触影
            contactShadowResult = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2));
            sHdrW = curW;
            sHdrH = curH;
        }

        /// @note アップスケール元。等倍のときは 1 枚まるごと無駄なので持たない。
        /// @note 上のリサイズ判定とは別に見る: 内部解像度は kMinRenderWidth で床に張り付くので、
        /// @note 出力先だけが動いて curW/curH が変わらないフレームがある。そこで倍率が等倍を
        /// @note またぐと、必要な RT が無いまま UpscalePass だけが登録される。
        needsUpscale = (sHdrW != nativeW) || (sHdrH != nativeH);
        if (needsUpscale) {
            if (!upscaleSrcRT.IsValid())
                upscaleSrcRT = resources.CreateRenderTarget(
                    sHdrW, sHdrH,
                    renderer::RenderTargetDesc{ 1, renderer::Format::RGBA16F, /*withDepth=*/false });
        } else if (upscaleSrcRT.IsValid()) {
            resources.Release(upscaleSrcRT);
            upscaleSrcRT = {};
        }

        /// @note フロクセル霧のボリューム。解像度ではなくグリッド寸法で作り直す。
        /// @note リサイズのたびに 14MB を作り直すとフレームが飛び、履歴が切れて霧が暗転する。
        /// @note 無効化しても解放しないのは、切り戻した瞬間に確保が走るのを避けるため。
        if (rs.froxelFog.enabled) {
            const uint32_t fx = (std::max)(rs.froxelFog.gridX, 1u);
            const uint32_t fy = (std::max)(rs.froxelFog.gridY, 1u);
            const uint32_t fz = (std::max)(rs.froxelFog.gridZ, 1u);
            if (viewTargets.froxelGrid[0] != fx || viewTargets.froxelGrid[1] != fy
                || viewTargets.froxelGrid[2] != fz
                || !viewTargets.froxelScatter.IsValid()
                || !viewTargets.froxelScatterHistory.IsValid()
                || !viewTargets.froxelIntegrated.IsValid()) {
                if (viewTargets.froxelScatter.IsValid())
                    resources.Release(viewTargets.froxelScatter);
                if (viewTargets.froxelScatterHistory.IsValid())
                    resources.Release(viewTargets.froxelScatterHistory);
                if (viewTargets.froxelIntegrated.IsValid())
                    resources.Release(viewTargets.froxelIntegrated);
                viewTargets.froxelGrid[0] = fx;
                viewTargets.froxelGrid[1] = fy;
                viewTargets.froxelGrid[2] = fz;
                viewTargets.froxelScatter        = resources.CreateComputeTexture3D(fx, fy, fz);
                viewTargets.froxelScatterHistory = resources.CreateComputeTexture3D(fx, fy, fz);
                viewTargets.froxelIntegrated     = resources.CreateComputeTexture3D(fx, fy, fz);
                /// @note 中身が未初期化の 2 枚を「前フレーム」として読ませない。
                viewTargets.froxelState.grid[0] = 0u;
                viewTargets.froxelState.grid[1] = 0u;
                viewTargets.froxelState.grid[2] = 0u;
            }
        }
    }

    /// @note ライト収集とシャドウ範囲計算はシーン全体を走査するため、独立して計測する。
    profiler::Profiler::BeginSample(
        profiler::ProfilerMarker("RenderSystem::LightingSetup", "Rendering"));

    /// @note ライト定数バッファを構築
    renderer::LightConstantsCB lightData{};
    lightData.lightDir       = { 0.0f, -1.0f, 0.5f };
    lightData.lightColor     = { 1.0f,  1.0f, 1.0f };
    lightData.lightIntensity = 1.0f;

    /// @note Directional Light のシャドウ設定 (LightComponent から取得)
    bool  dirCastShadows    = true;
    float dirShadowBias     = 1.0f;
    float dirShadowStrength = 1.0f;
    float dirShadowDistance = 0.0f;

    /// @note クラスタライティング用の統合ライト配列。b3 の固定長配列と並行して構築する。
    /// @note b3 は点 8 / スポット 4 で打ち切るが、こちらは 256 本まで拾う。
    /// @note b3 の詰め方は変えないので、クラスタ未対応のパスの見た目は据え置き。
    std::vector<PunctualLightGPU> punctualLights;
    punctualLights.reserve(32);

    /// @note 影を落とせるライトの候補 (Directional 以外の全型)。
    /// @note アトラスは 16 タイルしかなく、走査順に配ると「シーンのどこに置いたか」で
    /// @note 影の有無が決まる。全部集めてから捨てる相手を選ぶ。
    struct PunctualShadowCandidate {
        size_t        punctualIndex;   ///< @note punctualLights 内の位置
        int           legacySlot;      ///< @note b3 側の位置 (点 0-7 / スポット 8-11)。-1 = b3 に入らない
        /// @note 全方位のライトはキューブ 6 面 = 6 タイルを使う。Spot / Area は 1 タイル。
        bool          needsCube;
        math::Vector3 position;
        math::Vector3 direction;       ///< @note 1 タイル側の照射方向 (キューブでは未使用)
        float         range;
        float         outerCone;       ///< @note [degrees] 1 タイル側の半画角
        float         nearPlane;
        float         bias;
        float         strength;
        float         sourceRadius;  ///< @note 半影の広がりを決める光源半径 [m]
        float         cameraDistSq;
    };
    std::vector<PunctualShadowCandidate> shadowCandidates;

    /// @note Cookie を持つスポットの候補。割り当ての考え方は影と同じで、タイル数が
    /// @note 有限 (8 枚) なのでカメラから近い順に配る。
    struct LightCookieCandidate {
        size_t        punctualIndex;
        int           legacySlot;
        math::Vector3 position;
        math::Vector3 direction;
        float         range;
        float         outerCone;   ///< @note [degrees]
        float         nearPlane;
        float         rotationRad;
        std::string   path;
        float         cameraDistSq;
    };
    std::vector<LightCookieCandidate> cookieCandidates;

    /// @note b3 経路の点光源 / スポットの光源半径。添字は legacyShadowSlots と同じ。
    float legacySourceRadius[kMaxLegacyPunctualLights] = {};

    constexpr float kDegToRad = 3.14159265f / 180.0f;
    /// @note キューブ 1 面ぶんの半画角 (= 90 度の半分)。Point シャドウの 6 面で使う。
    constexpr float kQuarterPi = 3.14159265f / 4.0f;
    /// @note Area の影を焼く錐台の半画角 [degrees]。Spot の outerCone に相当する値として渡す。
    /// @note 面光源は法線側の半球 (= 90 度) を照らすが、透視投影は 90 度で無限に広がるため
    /// @note 張れない。75 度は「パネルの正面に置いた物の影は出る / 真横は諦める」の線。
    constexpr float kAreaShadowOuterConeDeg = 75.0f;
    /// @note View<> だと GameObject が取れず activeInHierarchy() を見られないので、GO を切っても
    /// @note 光だけが残る。GetEntities<> は View<> と同じ基底 span なので走査順は変わらない。
    for (EntityID id : scene.GetEntities<LightComponent>()) {
        GameObject*     go    = scene.GetGameObject(id);
        LightComponent* light = scene.GetComponent<LightComponent>(id);
        if (!go || !light || !go->activeInHierarchy() || !light->enabled) continue;
        const Transform&      tf = go->transform;
        const LightComponent& lc = *light;

        /// @note 色温度モードでは color 欄ではなく colorTemperature が正本。
        /// @note 毎フレーム引き直す。キャッシュは Inspector の反映漏れという見つけにくい種になる。
        const math::Vector3 lightColor =
            lc.useColorTemperature ? renderer::ColorFromTemperature(lc.colorTemperature)
                                   : lc.color;

        /// @note 点光源 / スポット / 大きさを持つ光源は上限に達するまで統合配列へも積む。
        /// @note b3 は「点を全部→スポットを全部」の 2 配列だがこちらは 1 本なので評価順が変わりうる。
        /// @note 加算なので結果は同じ (順序による丸め差のみ)。
        if (lc.type != LightComponent::Type::Directional
            && punctualLights.size() < kMaxPunctualLights) {
            const size_t punctualIndex = punctualLights.size();
            PunctualLightGPU& gpu = punctualLights.emplace_back();
            /// @note worldPosition を使う: Transform::position は親基準のローカル座標。
            /// @note 子 GameObject にライトを置くと (キャラクターの発光部・車のヘッドライト・
            /// @note ボーンに付けた松明)、親の姿勢が一切効かず原点付近に光が落ちる。
            /// @note 向き (forward / right / up) は worldRotation から作られるので既に
            /// @note ワールド空間で、位置だけが取り残されていた。
            gpu.position  = tf.worldPosition;
            gpu.range     = lc.range;
            gpu.color     = lightColor;
            gpu.intensity = lc.intensity;
            /// @note 既定は Point。他の型が以降で上書きする。
            gpu.direction   = { 0.0f, -1.0f, 0.0f };
            gpu.innerCos    = 0.0f;
            gpu.outerCos    = 0.0f;
            gpu.type        = static_cast<uint32_t>(PunctualLightType::Point);
            gpu.shadowIndex = -1;
            gpu.cookieIndex = -1;
            gpu.tangent     = { 1.0f, 0.0f, 0.0f };
            gpu.bitangent   = { 0.0f, 1.0f, 0.0f };
            /// @note 点光源 / スポットでも halfWidth は光源半径として意味を持つ
            /// @note (形状は点のまま、ハイライトの広がりと影のにじみ幅にだけ効く)。
            gpu.halfWidth   = (std::max)(lc.sourceRadius, 0.0f);
            gpu.halfHeight  = 0.0f;

            if (lc.type == LightComponent::Type::Spot) {
                gpu.direction = tf.forward.Normalized();
                gpu.innerCos  = std::cos(lc.innerCone * kDegToRad);
                gpu.outerCos  = std::cos(lc.outerCone * kDegToRad);
                gpu.type      = static_cast<uint32_t>(PunctualLightType::Spot);
            } else if (lc.type == LightComponent::Type::Sphere) {
                gpu.type      = static_cast<uint32_t>(PunctualLightType::Sphere);
            } else if (lc.type == LightComponent::Type::Tube) {
                /// @note 管の軸は Transform の Right。蛍光灯を横向きに置く姿勢が既定になる。
                gpu.tangent    = tf.right.NormalizedOr({ 1.0f, 0.0f, 0.0f });
                gpu.halfHeight = (std::max)(lc.sourceLength, 0.0f) * 0.5f;
                gpu.type       = static_cast<uint32_t>(PunctualLightType::Tube);
            } else if (lc.type == LightComponent::Type::Area) {
                gpu.direction  = tf.forward.NormalizedOr({ 0.0f, 0.0f, 1.0f });
                gpu.tangent    = tf.right.NormalizedOr({ 1.0f, 0.0f, 0.0f });
                gpu.bitangent  = tf.up.NormalizedOr({ 0.0f, 1.0f, 0.0f });
                gpu.halfWidth  = (std::max)(lc.areaWidth,  0.001f) * 0.5f;
                gpu.halfHeight = (std::max)(lc.areaHeight, 0.001f) * 0.5f;
                /// @note Area では innerCos / outerCos が空くので、両面フラグの運搬に使う。
                /// @note 専用フィールドを足すと 96 バイトの構造体がキャッシュライン 2 本に収まらない。
                gpu.outerCos   = lc.areaTwoSided ? 1.0f : 0.0f;
                gpu.type       = static_cast<uint32_t>(PunctualLightType::Area);
            }

            const math::Vector3 toCamera = tf.worldPosition - camera.m_position;
            const float cameraDistSq = math::Vector3::Dot(toCamera, toCamera);

            /// @note b3 側でこのライトが取る添字。直後のブロックが末尾へ 1 つ足すだけなので、
            /// @note 採番される番号は今のカウンタ値そのもの。
            int legacySlot = -1;
            if (lc.type == LightComponent::Type::Point && lightData.pointLightCount < 8)
                legacySlot = lightData.pointLightCount;
            else if (lc.type == LightComponent::Type::Spot && lightData.spotLightCount < 4)
                legacySlot = kLegacySpotSlotBase + lightData.spotLightCount;
            if (legacySlot >= 0)
                legacySourceRadius[legacySlot] = (std::max)(lc.sourceRadius, 0.0f);

            /// @note Cookie の候補。Spot 専用 — Point はキューブマップ、Directional は
            /// @note ワールド空間のタイリングという別の仕組みが要る。
            if (lc.type == LightComponent::Type::Spot && !lc.cookiePath.empty()) {
                LightCookieCandidate cookie{};
                cookie.punctualIndex = punctualIndex;
                cookie.legacySlot    = legacySlot;
                cookie.position      = tf.worldPosition;
                cookie.direction     = tf.forward.NormalizedOr({ 0.0f, 0.0f, 1.0f });
                cookie.range         = (std::max)(lc.range, 0.05f);
                cookie.outerCone     = lc.outerCone;
                cookie.nearPlane     = (std::max)(lc.shadowNearPlane, 0.01f);
                cookie.rotationRad   = lc.cookieRotation * kDegToRad;
                cookie.path          = lc.cookiePath;
                cookie.cameraDistSq  = cameraDistSq;
                cookieCandidates.push_back(std::move(cookie));
            }

            /// @note 影の候補として控える。Directional 以外は全型が落とせる。
            /// @note 形状を持つ光源も点から焼いた影でよい: 影の形は遮蔽物と受光面の配置でほぼ決まり、
            /// @note 光源の大きさは半影の広さ (sourceRadius から作る penumbraTexels) にしか効かない。
            /// @note 管が長いと本来は半影が軸方向へ伸びるが、それには軸に沿った複数枚が要り 16 タイルでは足りない。
            const bool canCastShadow =
                lc.castShadows && lc.shadowStrength > 0.0f &&
                lc.type != LightComponent::Type::Directional;
            if (canCastShadow) {
                /// @note 遠すぎるライトへタイルを割り当てない。判定距離に range を足すのは、
                /// @note range の大きいライトは離れていても画面を広く照らすため。
                const float limit = rs.shadow.punctualShadowDistance + lc.range;
                if (cameraDistSq <= limit * limit) {
                    PunctualShadowCandidate cand{};
                    cand.punctualIndex = punctualIndex;
                    cand.legacySlot    = legacySlot;
                    /// @note Sphere / Tube は Point と同じ全方位。Area だけが向きを持つ。
                    cand.needsCube     = (lc.type == LightComponent::Type::Point
                                       || lc.type == LightComponent::Type::Sphere
                                       || lc.type == LightComponent::Type::Tube);
                    cand.position      = tf.worldPosition;
                    cand.direction     = cand.needsCube
                                       ? math::Vector3{ 0.0f, -1.0f, 0.0f }
                                       : tf.forward.NormalizedOr({ 0.0f, 0.0f, 1.0f });
                    cand.range         = (std::max)(lc.range, 0.05f);
                    /// @note Area は法線側の半球を照らすが、1 枚の透視投影では 180 度を張れない。
                    /// @note 実用上そこまでで、これ以上広げると端のテクセル密度が落ちるだけ。
                    cand.outerCone     = (lc.type == LightComponent::Type::Area)
                                       ? kAreaShadowOuterConeDeg
                                       : lc.outerCone;
                    cand.nearPlane     = (std::max)(lc.shadowNearPlane, 0.01f);
                    cand.bias          = (std::max)(lc.shadowBias, 0.0f);
                    cand.strength      = std::clamp(lc.shadowStrength, 0.0f, 1.0f);
                    cand.sourceRadius  = (std::max)(lc.sourceRadius, 0.0f);
                    cand.cameraDistSq  = cameraDistSq;
                    shadowCandidates.push_back(cand);
                }
            }
        }

        if (lc.type == LightComponent::Type::Directional) {
            lightData.lightDir       = tf.forward.Normalized();
            lightData.lightColor     = lightColor;
            lightData.lightIntensity = lc.intensity;
            dirCastShadows    = lc.castShadows;
            dirShadowBias     = lc.shadowBias;
            dirShadowStrength = lc.shadowStrength;
            dirShadowDistance = lc.shadowDistance;
        } else if (lc.type == LightComponent::Type::Point
                   && lightData.pointLightCount < 8) {
            auto& pl    = lightData.pointLights[lightData.pointLightCount++];
            pl.position  = tf.worldPosition;
            pl.range     = lc.range;
            pl.color     = lightColor;
            pl.intensity = lc.intensity;
        } else if (lc.type == LightComponent::Type::Spot
                   && lightData.spotLightCount < 4) {
            auto& sl    = lightData.spotLights[lightData.spotLightCount++];
            sl.position  = tf.worldPosition;
            sl.direction = tf.forward.Normalized();
            sl.range     = lc.range;
            sl.innerCos  = std::cos(lc.innerCone * kDegToRad);
            sl.outerCos  = std::cos(lc.outerCone * kDegToRad);
            sl.color     = lightColor;
            sl.intensity = lc.intensity;
        }
    }

    /// @note 粒子を点光源にする (ParticleEmitter の Lights モジュール)。LightComponent の後に積むので、
    /// @note 枠が足りないときに削られるのは粒子の光の方。Legacy (b3) には載せない。
    /// @note GPU シミュレーションの粒子は位置が GPU にしか無いので対象外 (Inspector に注記がある)。
    std::vector<ParticleLightEmission> particleLights;
    for (EntityID id : scene.GetEntities<ParticleEmitter>()) {
        if (punctualLights.size() >= kMaxPunctualLights) break;
        GameObject*      go      = scene.GetGameObject(id);
        ParticleEmitter* emitter = scene.GetComponent<ParticleEmitter>(id);
        if (!go || !emitter || !go->activeInHierarchy() || !emitter->settings.enabled
            || !emitter->settings.light.lightEnabled
            || CanUseGpuSimulation(emitter->settings, &emitter->runtime.material))
            continue;
        SelectParticleLights(emitter->settings.light, emitter->runtime.particles,
                             kMaxPunctualLights - punctualLights.size(), particleLights);
        const bool localSpace = emitter->settings.simulationSpace == ParticleSimulationSpace::Local;
        for (const ParticleLightEmission& emission : particleLights) {
            PunctualLightGPU& gpu = punctualLights.emplace_back();
            gpu.position  = localSpace ? TransformEmitterPoint(go->transform, emission.position) : emission.position;
            gpu.range     = emission.range;
            gpu.color     = emission.color;
            gpu.intensity = emission.intensity;
            gpu.direction = { 0.0f, -1.0f, 0.0f };
            gpu.type      = static_cast<uint32_t>(PunctualLightType::Point);
        }
    }

    /// @name Spot / Point シャドウのスロット割り当てと行列の組み立て
    /// @note カメラから近い順。遠いライトの影は数ピクセルにしかならず落としても気づかれにくい。
    /// @note 距離キーは連続に変化するので、あふれの切り替わりも端から 1 つずつ起きる。
    std::sort(shadowCandidates.begin(), shadowCandidates.end(),
              [](const PunctualShadowCandidate& a, const PunctualShadowCandidate& b) {
                  return a.cameraDistSq < b.cameraDistSq;
              });

    /// @note アトラスは 4x4 = kMaxPunctualShadows タイル。Spot が 1 枚、Point が 6 枚を使う。
    constexpr uint32_t kPunctualTilesPerSide = 4u;
    static_assert(kPunctualTilesPerSide * kPunctualTilesPerSide
                      == static_cast<uint32_t>(kMaxPunctualShadows),
                  "punctual shadow atlas tiling must cover exactly kMaxPunctualShadows tiles");
    const uint32_t punctualTileSize =
        (std::max)(punctualShadowRes / kPunctualTilesPerSide, 1u);
    const float    punctualAtlasResF = static_cast<float>(punctualShadowRes);
    const float    punctualUvScale   =
        static_cast<float>(punctualTileSize) / punctualAtlasResF;

    /// @note キューブ 6 面の向きと up。順序は PunctualShadow.hlsli の FBZZ_CubeFaceIndex と
    /// @note 一致させること (+X, -X, +Y, -Y, +Z, -Z)。
    /// @note up は描く行列と引く行列が同じなら何でもよい (両方ここで作った 1 本を使う)。
    static constexpr math::Vector3 kCubeFaceDir[6] = {
        {  1.0f,  0.0f,  0.0f }, { -1.0f,  0.0f,  0.0f },
        {  0.0f,  1.0f,  0.0f }, {  0.0f, -1.0f,  0.0f },
        {  0.0f,  0.0f,  1.0f }, {  0.0f,  0.0f, -1.0f },
    };
    static constexpr math::Vector3 kCubeFaceUp[6] = {
        { 0.0f, 1.0f,  0.0f }, { 0.0f, 1.0f, 0.0f },
        { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f, 1.0f },
        { 0.0f, 1.0f,  0.0f }, { 0.0f, 1.0f, 0.0f },
    };

    PunctualShadowView punctualViews[kMaxPunctualShadows] = {};
    int punctualViewCount   = 0;
    /// @note キューブ 6 面を使ったライトの本数 (Point / Sphere / Tube)。
    int shadowedCubeCount   = 0;
    /// @note b3 経路 (既定の Forward) 向けのスロット番号。-1 = 影なし。
    int legacyShadowSlots[kMaxLegacyPunctualLights];
    for (int& slot : legacyShadowSlots) slot = -1;

    /// @note タイル 1 枚を組み立てる。halfFovRad はそのタイルの投影半画角。
    const auto buildPunctualView =
        [&](int slot, const math::Vector3& eye, const math::Vector3& dir,
            const math::Vector3& up, float halfFovRad, float nearZ, float farZ,
            float biasScale, float strength, float sourceRadius) {
        PunctualShadowView& view = punctualViews[slot];
        view.view           = math::Matrix4::LookAt(eye, eye + dir, up);
        view.viewProjection =
            math::Matrix4::Perspective(halfFovRad * 2.0f, 1.0f, nearZ, farZ) * view.view;
        view.eyePos         = eye;
        view.frustum        = math::Frustum::FromViewProjection(view.viewProjection);
        view.shadowStrength = strength;

        const uint32_t tileX = static_cast<uint32_t>(slot) % kPunctualTilesPerSide;
        const uint32_t tileY = static_cast<uint32_t>(slot) / kPunctualTilesPerSide;
        view.viewportX    = tileX * punctualTileSize;
        view.viewportY    = tileY * punctualTileSize;
        view.viewportSize = punctualTileSize;
        view.atlasRect    = {
            static_cast<float>(view.viewportX) / punctualAtlasResF,
            static_cast<float>(view.viewportY) / punctualAtlasResF,
            punctualUvScale, punctualUvScale
        };

        /// @note 基本バイアスは「1 テクセルが覆うワールド距離」。アクネはテクセルの幅の中で
        /// @note 面の深度が変わることから出るので、補正量はテクセルの実寸そのものになる
        /// @note (斜め面ぶんの tan(theta) はシェーダー側の FBZZ_PunctualSlopeBias が掛ける)。
        /// @note 評価点が range の中ほどなのは、透視投影ではテクセル実寸が深度に比例するため。
        const float midZ       = (std::max)((nearZ + farZ) * 0.5f, nearZ * 2.0f);
        const float texelWorld =
            2.0f * midZ * std::tan(halfFovRad) / static_cast<float>(punctualTileSize);
        /// @note 透視投影の NDC 深度は非線形なので、ワールド距離をそのまま渡せない。
        /// @note z_ndc = f/(f-n) * (1 - n/z)  →  dz_ndc/dz = f*n / ((f-n) * z^2)
        const float ndcPerWorld =
            (farZ * nearZ) / ((std::max)(farZ - nearZ, 0.001f) * midZ * midZ);
        view.biasNDC = texelWorld * ndcPerWorld * biasScale;

        /// @note 1 テクセルが張る角度。ShadowPass の極小 caster カリングが使う。
        view.texelAngularSize =
            2.0f * std::tan(halfFovRad) / static_cast<float>(punctualTileSize);

        /// @note 光源半径がシャドウマップ上で何テクセルぶんの半影になるか。
        /// @note 本来は「光源の大きさ × 遮蔽物と受光面の距離比」だが、ブロッカー探索が無いので
        /// @note 比を 1 とみなす。遮蔽物が遠いほど硬くなるが「大きな電球ほど柔らかい」は出る。
        view.penumbraTexels = (texelWorld > 0.0f) ? (sourceRadius / texelWorld) : 0.0f;
    };

    if (rs.shadowEnabled) {
        for (const PunctualShadowCandidate& cand : shadowCandidates) {
            const int needed = cand.needsCube ? 6 : 1;
            /// @note break ではなく continue。全方位のライトが入らなかっただけで、後ろに続く
            /// @note Spot / Area は 1 枚で収まる可能性がある。
            if (punctualViewCount + needed > kMaxPunctualShadows) continue;
            if (cand.needsCube && shadowedCubeCount >= rs.shadow.maxShadowedPointLights) continue;

            /// @note Inspector で range より大きい shadowNearPlane を入れられるので、
            /// @note ここで潰さないと Matrix4::Perspective の assert を踏む。
            const float farZ  = cand.range;
            const float nearZ = (std::min)(cand.nearPlane, farZ * 0.5f);

            const int baseSlot = punctualViewCount;
            if (cand.needsCube) {
                for (int face = 0; face < 6; ++face) {
                    buildPunctualView(baseSlot + face, cand.position,
                                      kCubeFaceDir[face], kCubeFaceUp[face],
                                      kQuarterPi, nearZ, farZ, cand.bias, cand.strength,
                                      cand.sourceRadius);
                }
                punctualViewCount += 6;
                ++shadowedCubeCount;
            } else {
                /// @note 錐台は円錐へ外接させる。outerCone は半角なので画角はその 2 倍。
                /// @note 少し広げるのは、ぴったり切ると PCF が縁ではみ出して影が欠けるため。
                /// @note Area はコーンを持たないので kAreaShadowOuterConeDeg が入っている。
                const float halfFov = (std::min)(
                    std::clamp(cand.outerCone, 1.0f, 79.0f) * kDegToRad * 1.05f,
                    kQuarterPi * 1.9f);
                const math::Vector3 up = (std::abs(cand.direction.y) > 0.99f)
                                         ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                                         : math::Vector3{ 0.0f, 1.0f, 0.0f };
                buildPunctualView(baseSlot, cand.position, cand.direction, up,
                                  halfFov, nearZ, farZ, cand.bias, cand.strength,
                                  cand.sourceRadius);
                punctualViewCount += 1;
            }
            /// @note クラスタ経路はライト構造体から、レガシー経路は b12 の対応表から番号を引く。
            /// @note どちらの経路でも同じスロットを指すよう、ここで両方へ書く。
            punctualLights[cand.punctualIndex].shadowIndex = baseSlot;
            if (cand.legacySlot >= 0 && cand.legacySlot < kMaxLegacyPunctualLights)
                legacyShadowSlots[cand.legacySlot] = baseSlot;
        }
    }

    /// @name Cookie のスロット割り当て
    /// @note 影と同じくカメラから近い順。タイルは 8 枚しかない。
    std::sort(cookieCandidates.begin(), cookieCandidates.end(),
              [](const LightCookieCandidate& a, const LightCookieCandidate& b) {
                  return a.cameraDistSq < b.cameraDistSq;
              });

    LightCookieView cookieViews[kMaxLightCookies];
    int cookieViewCount = 0;
    int legacyCookieSlots[kMaxLegacyPunctualLights];
    for (int& slot : legacyCookieSlots) slot = -1;

    for (const LightCookieCandidate& cand : cookieCandidates) {
        if (cookieViewCount >= kMaxLightCookies) break;

        const int slot = cookieViewCount++;
        LightCookieView& view = cookieViews[slot];

        /// @note 投影は影と同じ「スポットの円錐に外接する透視錐台」。
        /// @note 影の行列を流用しないのは、Cookie が影を落とさないライトにも付くため。
        /// @note 縁を広げないのは、近傍サンプルが無く広げると模様がコーンより内側で終わるため。
        const float farZ    = cand.range;
        const float nearZ   = (std::min)(cand.nearPlane, farZ * 0.5f);
        const float halfFov = std::clamp(cand.outerCone, 1.0f, 79.0f) * kDegToRad;
        const math::Vector3 up = (std::abs(cand.direction.y) > 0.99f)
                                 ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                                 : math::Vector3{ 0.0f, 1.0f, 0.0f };

        const math::Matrix4 cookieView =
            math::Matrix4::LookAt(cand.position, cand.position + cand.direction, up);
        view.viewProjection =
            math::Matrix4::Perspective(halfFov * 2.0f, 1.0f, nearZ, farZ) * cookieView;

        const uint32_t tileX = static_cast<uint32_t>(slot) % kLightCookieAtlasCols;
        const uint32_t tileY = static_cast<uint32_t>(slot) / kLightCookieAtlasCols;
        view.viewportX = tileX * kLightCookieTileSize;
        view.viewportY = tileY * kLightCookieTileSize;
        view.atlasRect = {
            static_cast<float>(view.viewportX) / static_cast<float>(kLightCookieAtlasWidth),
            static_cast<float>(view.viewportY) / static_cast<float>(kLightCookieAtlasHeight),
            static_cast<float>(kLightCookieTileSize) / static_cast<float>(kLightCookieAtlasWidth),
            static_cast<float>(kLightCookieTileSize) / static_cast<float>(kLightCookieAtlasHeight)
        };
        view.sourcePath  = cand.path;
        view.rotationRad = cand.rotationRad;

        punctualLights[cand.punctualIndex].cookieIndex = slot;
        if (cand.legacySlot >= 0 && cand.legacySlot < kMaxLegacyPunctualLights)
            legacyCookieSlots[cand.legacySlot] = slot;
    }

    /// @name 昼夜の色・強度カーブ (Phase B)
    /// @note 太陽の向きは DirectionalLight の transform が唯一のソース (lightDir は上書きしない)。
    /// @note dayNightEnabled のときは、その光源の太陽高度から色と強度の遷移だけを駆動する。
    /// @note ライトを回すと 太陽ディスク・空・月・空連動 IBL・ライティングが一緒に動く。
    /// @note 雲シャドウ params (Phase C) も SkyRenderer から読み、passCtx へ後で転送する。
    float skyCloudShadowStrength = 0.0f, skyCloudShadowCoverage = 0.5f,
          skyCloudShadowScale = 0.02f, skyCloudShadowSpeed = 1.0f;
    /// @note 太陽の向きは DirectionalLight 側で決まるため SkyRenderer の Transform は使わない。
    for (EntityID id : scene.GetEntities<SkyRenderer>()) {
        GameObject* go     = scene.GetGameObject(id);
        auto*       skyPtr = scene.GetComponent<SkyRenderer>(id);
        if (!go || !skyPtr || !go->activeInHierarchy() || !skyPtr->enabled) continue;
        const SkyRenderer& sky = *skyPtr;

        /// @note 雲シャドウは昼夜サイクルとは独立に常に反映する。
        skyCloudShadowStrength = sky.cloudShadowStrength;
        skyCloudShadowCoverage = sky.cloudShadowCoverage;
        /// @note Component は「まだら 1 周期の大きさ [m]」。シェーダーは world→UV スケールを要る。
        skyCloudShadowScale    = 1.0f / (std::max)(sky.cloudShadowSize, 1.0f);
        skyCloudShadowSpeed    = sky.cloudShadowSpeed;

        if (sky.dayNightEnabled) {
            /// @note 太陽方向 (toward sun) = -lightDir。その高度 [度] を軸に 夜 ↔ 夕方 ↔ 昼 を補間する。
            /// @note 高度 0° を夕方のキーに置くと「ライトを水平 = 夕方」になり、昼側と夜側それぞれ
            /// @note 独立した帯幅で抜けられる。旧実装は夕焼けの重みに昼の重みを掛けており、
            /// @note 地平線上で重みが 0.17 まで落ちて夕方を作れなかった。
            const math::Vector3 sunToSun =
                math::Vector3{ -lightData.lightDir.x, -lightData.lightDir.y, -lightData.lightDir.z }.Normalized();

            auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
            auto lerp1   = [](float a, float b, float t) { return a + (b - a) * t; };
            auto lerp3   = [](const math::Vector3& a, const math::Vector3& b, float t) {
                return math::Vector3{ a.x + (b.x - a.x) * t,
                                      a.y + (b.y - a.y) * t,
                                      a.z + (b.z - a.z) * t };
            };

            constexpr float kRadToDeg = 57.29577951f;
            const float sinAlt      = (std::max)(-1.0f, (std::min)(1.0f, sunToSun.y));
            const float altitudeDeg = std::asin(sinAlt) * kRadToDeg;

            const bool  above = altitudeDeg >= 0.0f;
            const float span  = above ? (std::max)(sky.dayAltitude,   0.1f)
                                      : (std::max)(sky.nightAltitude, 0.1f);
            float t = clamp01(std::fabs(altitudeDeg) / span);
            /// @note smoothstep: 帯の端で色・明るさが折れないようにする
            t = t * t * (3.0f - 2.0f * t);

            lightData.lightColor     = lerp3(sky.sunsetColor,
                                             above ? sky.dayColor : sky.nightColor, t);
            lightData.lightIntensity = lerp1(sky.sunsetIntensity,
                                             above ? sky.dayIntensity : sky.nightIntensity, t);
            /// @note 空の明るさは太陽光の強さとは別軸。共用していた頃は太陽を強くすると空も白飛びした。
            /// @note 詳細は SkyRenderer::skyDayBrightness。
            lightData.skyDimmer      = lerp1(sky.skySunsetBrightness,
                                             above ? sky.skyDayBrightness : sky.skyNightBrightness, t);
        }
        break;
    }

    /// @note ambientColor: Lit モードでは AMBIENT_SCALE 相当値、Unlit 系では白に上書き
    lightData.ambientColor = { 0.08f, 0.08f, 0.08f };
    if (rs.IsUnlit()) {
        lightData.ambientColor    = { 1.0f, 1.0f, 1.0f };
        lightData.lightIntensity  = 0.0f;
        lightData.pointLightCount = 0;
        lightData.spotLightCount  = 0;
        /// @note 空も消灯する。分離前は lightIntensity=0 が空系シェーダーにも効いていたので、
        /// @note Unlit 表示で空が黒く落ちる従来の挙動を skyDimmer 側で維持する。
        lightData.skyDimmer       = 0.0f;
    }

    math::Vector3 lightDir = lightData.lightDir.Normalized();
    SceneShadowBounds shadowBounds;
    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ComputeShadowBounds");
        shadowBounds = ComputeSceneShadowBounds(scene, cullingMask);
    }
    if (!shadowBounds.valid) {
        shadowBounds.center = camera.m_position + camera.GetForward() * 20.0f;
        shadowBounds.radius = 40.0f;
        shadowBounds.valid = true;
    }

    /// @name カスケードシャドウ (CSM) のフィッティング
    /// @note 精細さを決めるのは解像度ではなく「1 テクセルが覆うワールド距離」。単一マップでは
    /// @note 到達距離を伸ばすと分母が伸びるだけで、近距離の精細さと両立しない。
    /// @note 視錐台を距離で区切り、手前ほど狭い範囲へ 1 タイルを割り当てて足元の密度だけ上げる。
    /// @note shadowBounds (シーン全体) は「これ以上大きくしない」上限としてだけ使う。
    const int cascadeCount =
        std::clamp(rs.shadow.cascadeCount, 1, fbzz::renderer::kMaxShadowCascades);

    /// @note アトラス配置: 1 分割なら全面、2 分割以上なら 2x2 タイル。
    /// @note 1 枚に収めれば影を読む 20 以上のシェーダーがバインドもサンプラーも変えずに済む。
    /// @note 解像度とメモリは分割数によらず一定で、変わるのは面積の配分だけ。
    const uint32_t atlasResolution = (std::max)(rs.shadow.mapResolution, 1u);
    const uint32_t tilesPerSide    = (cascadeCount > 1) ? 2u : 1u;
    const uint32_t tileSize        = (std::max)(atlasResolution / tilesPerSide, 1u);

    /// @note 影の最大到達距離。LightComponent::shadowDistance > 0 は従来どおり手動指定を優先する。
    const float shadowDistance = (dirShadowDistance > 0.0f)
        ? (std::max)(dirShadowDistance, 1.0f)
        : (std::max)(rs.shadow.autoFitDistance, 1.0f);

    float cascadeSplits[fbzz::renderer::kMaxShadowCascades] = {};
    ComputeCascadeSplits((std::max)(camera.m_near, 0.01f), shadowDistance,
                         cascadeCount, rs.shadow.cascadeSplitLambda, cascadeSplits);

    const math::Vector3 up = (std::abs(lightDir.y) > 0.99f)
                             ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                             : math::Vector3{ 0.0f, 1.0f, 0.0f };
    /// @note 位置を持たない回転だけのライト空間。テクセルスナップの量子化格子として使う。
    const math::Matrix4 snapView     = math::Matrix4::LookAt(math::Vector3::ZERO, lightDir, up);
    const math::Matrix4 snapViewInv  = math::Matrix4::Inverse(snapView);

    ShadowCascade cascades[fbzz::renderer::kMaxShadowCascades] = {};
    float cascadeSliceNear = (std::max)(camera.m_near, 0.01f);

    for (int i = 0; i < cascadeCount; ++i) {
        const float sliceFar = cascadeSplits[i];

        /// @note このカスケードが担当する視錐台スライスの外接球 (向きに依存しないので回転で脈動しない)。
        const FrustumSliceSphere slice =
            ComputeFrustumSliceSphere(camera, cascadeSliceNear, sliceFar);

        /// @note シーン全体より大きい影ボリュームを作っても無駄なテクセルが増えるだけ。
        float radius = (std::max)((std::min)(slice.radius, shadowBounds.radius), 1.0f);

        math::Vector3 center = camera.m_position + camera.GetForward() * slice.centerDistance;
        if (radius < shadowBounds.radius) {
            /// @note シーン球からはみ出さないよう、中心をシーン球内へ引き戻す。
            const math::Vector3 offset   = center - shadowBounds.center;
            const float         distance = offset.Length();
            const float         limit    = (std::max)(shadowBounds.radius - radius, 0.0f);
            if (distance > limit && distance > 0.0001f)
                center = shadowBounds.center + offset * (limit / distance);
        } else {
            center = shadowBounds.center;
        }

        /// @note テクセルスナップ。中心がカメラに追従するとサブテクセルのずれで輪郭が波打つ
        /// @note (shadow swimming)。ライト空間で 1 テクセル単位へ量子化すると標本位置が固定される。
        const float texelWorldSize = (radius * 2.0f) / static_cast<float>(tileSize);
        {
            math::Vector4 lightSpace =
                snapView * math::Vector4{ center.x, center.y, center.z, 1.0f };
            lightSpace.x = std::floor(lightSpace.x / texelWorldSize) * texelWorldSize;
            lightSpace.y = std::floor(lightSpace.y / texelWorldSize) * texelWorldSize;
            const math::Vector4 snapped = snapViewInv * lightSpace;
            center = { snapped.x, snapped.y, snapped.z };
        }

        /// @note 深度レンジ。ボリュームの外にいる背の高い caster も影を落とせるよう、
        /// @note ライト方向の引きはシーン全体の広がりから取る (near/far を広げても塗る面積は増えない)。
        const float pullback   = (std::max)(shadowBounds.radius, radius) + 20.0f;
        const float depthRange = pullback + radius + 20.0f;

        const math::Vector3 eye  = center - lightDir * pullback;
        const math::Matrix4 view = math::Matrix4::LookAt(eye, center, up);
        const math::Matrix4 proj = math::Matrix4::Orthographic(-radius, radius,
                                                               -radius, radius,
                                                               1.0f, depthRange);

        ShadowCascade& cascade = cascades[i];
        cascade.viewProjection = proj * view;
        cascade.view           = view;
        cascade.eyePos         = eye;
        cascade.frustum        = math::Frustum::FromViewProjection(cascade.viewProjection);
        cascade.texelWorldSize = texelWorldSize;
        /// @note ワールド空間で約 5mm 相当の一定バイアスになるよう深度レンジで正規化する。
        /// @note カスケードごとにレンジが違うので、値もカスケードごとに持つ。
        cascade.biasNDC = (0.005f * dirShadowBias) / (std::max)(depthRange - 1.0f, 1.0f);

        /// @note アトラス内のタイル位置 (2x2 を左上から Z 字順に埋める)。
        const uint32_t tileX = static_cast<uint32_t>(i) % tilesPerSide;
        const uint32_t tileY = static_cast<uint32_t>(i) / tilesPerSide;
        cascade.viewportX    = tileX * tileSize;
        cascade.viewportY    = tileY * tileSize;
        cascade.viewportSize = tileSize;

        const float uvScale = static_cast<float>(tileSize) / static_cast<float>(atlasResolution);
        cascade.atlasRect = {
            static_cast<float>(cascade.viewportX) / static_cast<float>(atlasResolution),
            static_cast<float>(cascade.viewportY) / static_cast<float>(atlasResolution),
            uvScale, uvScale
        };

        cascadeSliceNear = sliceFar;
    }

    /// @note 単一のライト行列で足りるパス (パーティクル自己影など) 向けの代表値。
    /// @note 到達範囲を最も広く覆う最遠カスケードを渡す。ビルボードの正対に view の内訳が要るので、
    /// @note view と viewProjection は必ず同じカスケードから対で渡す。
    const ShadowCascade& widestCascade = cascades[cascadeCount - 1];
    const math::Matrix4  lightVP   = widestCascade.viewProjection;
    const math::Matrix4  lightView = widestCascade.view;
    const math::Vector3  lightPos  = widestCascade.eyePos;

    /// @note 不透明物は可能な限り共通の GBuffer → AO → DeferredLighting 経路を通す。
    /// @note Forward 直描きでは SSAO/GTAO/ContactShadows/SSR/IBL が Terrain に乗らないため。
    /// @note 必須リソースが欠けるときだけ従来 Forward へ落ちる。
    /// @note 「GBuffer を作るパイプラインか」の定義は RenderSettings::UsesGBuffer() が唯一で、
    /// @note UI の警告 (PipelineDiagnostics) も同じ関数を見る。
    const bool wantsDeferredPipeline = rs.UsesGBuffer();
    /// @note GBuffer (法線 + 深度 + roughness) を描けるか。Deferred の本経路と、
    /// @note Forward のプリパスの両方がこれを土台にする。
    const bool gbufferAvailable = gbufferRT.IsValid() && gbufferShader.IsValid();
    const bool useGBufferOpaquePipeline =
        wantsDeferredPipeline &&
        gbufferAvailable &&
        deferredLightingShader.IsValid() &&
        depthCopyShader.IsValid();

    /// @name Forward の GBuffer プリパス
    /// @note SSAO / GTAO / SSR / 接触影 はどれも GBuffer の法線と深度から作る。Forward には
    /// @note 書く場所が無く、同じ設定でも効果が丸ごと消えていた。
    /// @note 不透明ジオメトリをもう 1 回描くので、画面空間系を要求されたときだけ走らせる。
    /// @note 副産物として深度プリパスにもなり、本描画で early-Z が効く。
    const bool needsScreenSpaceInputs =
        rs.postProcess.ambientOcclusion.enabled || rs.IsGtaoActive()
        || rs.contactShadow.enabled || rs.ssr.enabled;
    const bool forwardGBufferPrepass =
        !useGBufferOpaquePipeline && gbufferAvailable
        && needsScreenSpaceInputs && !rs.IsUnlit();

    /// @note 画面空間系を走らせられるか。Deferred の本経路でも Forward のプリパスでも成立する。
    const bool screenSpaceReady = useGBufferOpaquePipeline || forwardGBufferPrepass;

    const bool ssaoEnabled =
        screenSpaceReady &&
        rs.postProcess.ambientOcclusion.enabled &&
        ssaoShader.IsValid() &&
        ssaoBlurShader.IsValid() &&
        ssaoRaw.IsValid() &&
        ssaoBlur.IsValid();

    const bool selectionOutlineEnabled =
        rs.showSelectionOutline && !rs.selectedObjects.empty() &&
        selectionMaskRT.IsValid() && selectionMaskPso.IsValid() &&
        selectionOutlineShader.IsValid();

    /// @note ランタイムのオブジェクトマスク。showSelectionOutline (エディタの表示切り替え) には
    /// @note 従わない ─ あちらは「編集中の選択を出すか」の設定で、ゲームの見た目を消す権限は持たない。
    /// @note 生きた申告が 1 件も無いフレームまで描くと、全画面クリアと 1 パスぶんの帯域を毎フレーム
    /// @note 捨てるので切る。4 条件のどれで落ちても症状は «輪郭が出ない» の 1 種類で黙って落ちるため、
    /// @note 欠けている条件を objectMaskOffReason で 1 行名指しできるようにする。
    const char* objectMaskOffReason = nullptr;
    const bool objectMaskEnabled = [&]() {
        if (!objectMaskRT.IsValid())      { objectMaskOffReason = "objectMaskRT 未作成"; return false; }
        if (!selectionMaskPso.IsValid())  { objectMaskOffReason = "selectionMaskPso 未作成"; return false; }
        if (!objectMaskShader.IsValid() && !objectMaskSkinnedShader.IsValid()) {
            objectMaskOffReason = "Pipeline/Mask のシェーダーが両方とも読めない";
            return false;
        }
        for (const renderer::RenderObjectMaskRequest& request : rs.objectMaskRequests)
            if (renderer::IsObjectMaskRequestLive(request, Time::frameCount)) return true;
        objectMaskOffReason = rs.objectMaskRequests.empty()
            ? "申告が 1 件も届いていない (RenderSettings の実体違い)"
            : "申告はあるが全部フレーム落ち (提出が描画より後)";
        return false;
    }();
    {
        static const char* sLastReason = "";
        const char* reason = objectMaskEnabled ? "(有効)" : objectMaskOffReason;
        if (reason && reason != sLastReason) {
            sLastReason = reason;
            /// @note 申告先と読み手が同じ実体かを直接見る。RenderSystem は settings を «複製» して
            /// @note 使うので、複製元のアドレスが Application の active と一致しているかが要点。
            const void* readFrom = static_cast<const void*>(settings);
            const void* active   = static_cast<const void*>(
                core::Application::Get().GetActiveRenderSettings());
            FBZZ_LOG_INFO("ObjectMask: %s (requests=%zu / 読み手=%p 申告先=%p %s)",
                          reason, rs.objectMaskRequests.size(), readFrom, active,
                          readFrom == active ? "一致" : "不一致");
        }
    }

    /// @note 「有効なのに現在のパイプラインでは無視される設定」をログへ出す。
    /// @note エディタを開かずにビルドする経路でも同じ落とし穴を踏むので、警告表示だけでは足りない。
    /// @note 変化時だけ出す。毎フレーム出すとログが埋まって本当のエラーが見えなくなる。
    /// @note settings が無いフレーム (起動時の Warmup 等) は黙る: 診断はプロジェクトの設定を指すが、
    /// @note 既定値は Forward + Clustered 有効なので、Deferred+ のプロジェクトでも既定値のままだと
    /// @note «Clustered Lights は無視される» が身に覚えのないまま必ず 1 度出てしまう。
    if (settings != nullptr) {
        static std::string sLastInertReport;
        std::string report;
        for (const renderer::InertSetting& issue : renderer::CollectInertSettings(rs)) {
            report += issue.label;
            report += " / ";
        }
        if (report != sLastInertReport) {
            sLastInertReport = report;
            if (!report.empty()) {
                FBZZ_LOG_WARN("Pipeline: 有効ですが現在のパイプラインでは無視される設定があります "
                              "-> %s(Project Settings > Rendering > Pipeline を確認してください)",
                              report.c_str());
            }
        }
    }
    profiler::Profiler::EndSample();

    /// @note RenderPassHandles を組み立て
    RenderPassHandles passHandles{};
    /// @note selectionMaskRT / outlineRT はハンドルを配らない。
    /// @note 名前 ("SelectionMask" / "Outline") から res.Target() で引く (登録簿を参照)。
    passHandles.customPostProcessRT[0] = customPostProcessRT[0];
    passHandles.customPostProcessRT[1] = customPostProcessRT[1];
    for (uint32_t i = 0; i < kBloomMipCount; ++i) {
        passHandles.bloomChain[i]     = viewTargets.bloomChain[i];
        passHandles.bloomUpChain[i]   = viewTargets.bloomUpChain[i];
        passHandles.bloomChainWidth[i]  = (std::max)(1u, sHdrW / (2u << i));
        passHandles.bloomChainHeight[i] = (std::max)(1u, sHdrH / (2u << i));
    }
    passHandles.bloomHalf         = bloomHalf;
    passHandles.ssaoRaw           = ssaoRaw;
    passHandles.ssaoShader        = ssaoShader;
    passHandles.ssaoBlurShader    = ssaoBlurShader;
    passHandles.bloomDownShader   = bloomDownShader;
    passHandles.bloomUpShader     = bloomUpShader;
    passHandles.compositeShader   = compositeShader;
    passHandles.causticsShader    = causticsShader;
    passHandles.volumetricCloudShader = volumetricCloudShader;
    passHandles.cloudUpscaleShader    = cloudUpscaleShader;
    passHandles.selectionMaskShader       = selectionMaskShader;
    passHandles.selectionMaskSkinnedShader = selectionMaskSkinnedShader;
    passHandles.selectionMaskParticleShader = selectionMaskParticleShader;
    passHandles.selectionMaskParticleGpuShader = selectionMaskParticleGpuShader;
    passHandles.selectionOutlineShader    = selectionOutlineShader;
    passHandles.objectMaskShader         = objectMaskShader;
    passHandles.objectMaskInstancedShader = objectMaskInstancedShader;
    passHandles.objectMaskSkinnedShader  = objectMaskSkinnedShader;
    passHandles.copyColorShader           = copyColorShader;
    passHandles.upscaleShader             = upscaleShader;
    passHandles.downscaleShader           = downscaleShader;
    passHandles.customComposeShader       = customComposeShader;
    passHandles.fxaaShader        = fxaaShader;
    passHandles.customPostProcessShaders.resize(rs.postProcess.customEffects.size());
    /// @note 走る段でリストを分ける。登録順が RenderGraph のタイブレークなので、
    /// @note 同じ段の中では customEffects に並べた順がそのまま適用順になる。
    std::vector<uint32_t> customAfterOpaqueIndices;
    std::vector<uint32_t> customSceneHdrIndices;
    std::vector<uint32_t> customPostProcessIndices;
    customPostProcessIndices.reserve(rs.postProcess.customEffects.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(rs.postProcess.customEffects.size()); ++i) {
        const auto& custom = rs.postProcess.customEffects[i];
        if (!custom.enabled) continue;
        /// @note shaderPath は .mat を持たないパスの受け皿。materialPath 側の解決は
        /// @note パス本体が毎フレーム行う (シェーダーもテクスチャも .mat が決めるため)。
        if (!custom.shaderPath.empty())
            passHandles.customPostProcessShaders[i] = resources.LoadShader(custom.shaderPath);
        if (!passHandles.customPostProcessShaders[i].IsValid() && custom.materialPath.empty())
            continue;
        switch (custom.stage) {
        case renderer::CustomPassStage::AfterOpaque: customAfterOpaqueIndices.push_back(i); break;
        case renderer::CustomPassStage::SceneHDR:    customSceneHdrIndices.push_back(i);    break;
        case renderer::CustomPassStage::PostProcess: customPostProcessIndices.push_back(i); break;
        }
    }
    /// @note マスクを «読む» パスが 1 本も無いと、RenderGraph は ObjectMask パスごと刈る
    /// @note (BuildExecutionOrder)。輪郭が出ない症状は «申告が届いていない» と
    /// @note «読み手が居なくて刈られた» で同じに見えるので、ここでも出す。
    {
        static size_t sAfterOpaque = SIZE_MAX;
        static size_t sSceneHdr    = SIZE_MAX;
        static size_t sPostProcess = SIZE_MAX;
        if (customAfterOpaqueIndices.size() != sAfterOpaque
            || customSceneHdrIndices.size() != sSceneHdr
            || customPostProcessIndices.size() != sPostProcess) {
            sAfterOpaque = customAfterOpaqueIndices.size();
            sSceneHdr    = customSceneHdrIndices.size();
            sPostProcess = customPostProcessIndices.size();
            std::string names;
            for (const auto& custom : rs.postProcess.customEffects) {
                names += custom.name;
                names += custom.enabled ? "(on) " : "(off) ";
            }
            FBZZ_LOG_INFO("CustomPass: afterOpaque=%zu sceneHdr=%zu postProcess=%zu / %s",
                          sAfterOpaque, sSceneHdr, sPostProcess,
                          names.empty() ? "(効果なし)" : names.c_str());
        }
    }

    passHandles.selectionMaskPSO  = selectionMaskPso;
    passHandles.postprocPSO       = postprocPSO;
    /// @note Cookie 焼き。全画面三角形を不透明で塗るだけなので postproc と同じ状態でよい。
    passHandles.cookieBlitShader  = cookieBlitShader;
    passHandles.cookieBlitCB      = cookieBlitCB;
    passHandles.cookieBlitPSO     = postprocPSO;

    passHandles.exposureHistogram   = viewTargets.exposureHistogram;
    passHandles.exposureResult      = viewTargets.exposureResult;
    passHandles.exposureResetGeneration = viewTargets.exposureResetGeneration;
    passHandles.cloudRT             = &viewTargets.cloudRT;
    passHandles.waterSceneColorRT   = &viewTargets.waterSceneColorRT;
    passHandles.waterSceneDepthRT   = &viewTargets.waterSceneDepthRT;
    passHandles.cloudDepthRT        = &viewTargets.cloudDepthRT;
    passHandles.particleSceneColorRT = &viewTargets.particleSceneColorRT;
    passHandles.particleOverdrawRT   = &viewTargets.particleOverdrawRT;
    passHandles.particleReactiveRT   = &viewTargets.particleReactiveRT;
    passHandles.causticsDepthRT      = &viewTargets.causticsDepthRT;
    passHandles.exposureHistogramCS = exposureHistogramCS;
    passHandles.exposureAverageCS   = exposureAverageCS;
    passHandles.exposureCB          = exposureCB;

    /// @note 散乱ボリュームは 2 枚をフレームごとに入れ替える。Inject が scatter へ書きながら
    /// @note history を読むので、同じテクスチャを UAV と SRV に同時に張れない。
    /// @note 向きもビュー別。static だと 1 フレームに 2 回反転し、互いの履歴を読み合う。
    viewTargets.froxelState.ping = !viewTargets.froxelState.ping;
    passHandles.froxelScatter        = viewTargets.froxelState.ping
                                     ? viewTargets.froxelScatter : viewTargets.froxelScatterHistory;
    passHandles.froxelScatterHistory = viewTargets.froxelState.ping
                                     ? viewTargets.froxelScatterHistory : viewTargets.froxelScatter;
    passHandles.froxelIntegrated  = viewTargets.froxelIntegrated;
    passHandles.froxelInjectCS    = froxelInjectCS;
    passHandles.froxelIntegrateCS = froxelIntegrateCS;
    passHandles.froxelFogCB       = froxelFogCB;
    passHandles.causticsPSO       = causticsPSO;
    passHandles.volumetricCloudPSO = volumetricCloudPSO;
    passHandles.volumetricCloudPremultipliedPSO = volumetricCloudPremultipliedPSO;
    passHandles.frameCB           = frameCB;
    passHandles.objectCB          = objectCB;
    passHandles.lightCB           = lightCB;
    passHandles.bindPoseSkinningCB = bindPoseSkinningCB;

    /// @name スキンドモデルのリファレンスポーズ CB を遅延生成
    /// @note AnimatorComponent を持たない SkinnedMeshRenderer の既定パレット。スケルトン単位に
    /// @note 1 本で全インスタンスが共有する。無いと単位行列へ落ち、バインド変換を持つアセットが倒れる。
    {
        struct RefPoseData { math::Matrix4 bones[asset::MAX_SKINNING_BONES]; };
        for (auto& go : scene.GameObjects()) {
            auto* smr = go.GetComponent<SkinnedMeshRenderer>();
            if (!smr || !smr->model || !smr->model->skeleton) continue;
            asset::Model& model = *smr->model;
            if (model.referencePoseCB.IsValid()) continue;

            const std::vector<math::Matrix4>& refPose = model.skeleton->referencePose;
            if (refPose.empty()) continue;

            RefPoseData rp{};
            for (auto& m : rp.bones) m = math::Matrix4::Identity();
            const size_t maxBones = static_cast<size_t>(asset::MAX_SKINNING_BONES);
            const size_t count = refPose.size() < maxBones ? refPose.size() : maxBones;
            for (size_t i = 0; i < count; ++i) rp.bones[i] = refPose[i];

            model.referencePoseCB = resources.CreateConstantBuffer(sizeof(RefPoseData));
            if (model.referencePoseCB.IsValid())
                resources.Update(model.referencePoseCB, &rp, sizeof(RefPoseData));
        }
    }
    passHandles.postprocCB        = postprocCB;
    passHandles.outlineCB         = outlineCB;
    passHandles.objectMaskCB     = objectMaskCB;
    passHandles.volumetricCloudCB = volumetricCloudCB;
    passHandles.cloudShapeTex     = cloudShapeTex;
    passHandles.cloudDetailTex    = cloudDetailTex;
    passHandles.decalMaskRT       = decalMaskRT;
    passHandles.decalShader       = decalShader;
    passHandles.decalMaskShader   = decalMaskShader;
    passHandles.decalMaskSkinnedShader = decalMaskSkinnedShader;
    passHandles.decalPSO          = decalPSO;
    passHandles.decalMaskPSO      = decalMaskPso;
    passHandles.decalCB           = decalCB;
    passHandles.decalMaterialCB   = decalMaterialCB;
    passHandles.decalReceiverCB   = decalReceiverCB;
    /// @name ジオメトリ用ハンドル
    passHandles.shadowShader         = shadowShader;
    passHandles.shadowInstancedShader = shadowInstancedShader;
    passHandles.shadowSkinnedShader  = skinnedShadowShader;
    passHandles.shadowCB             = shadowCB;
    passHandles.velocityShader        = velocityShader;
    passHandles.velocityInstancedShader = velocityInstancedShader;
    passHandles.velocitySkinnedShader = velocitySkinnedShader;
    passHandles.punctualShadowCB     = punctualShadowCB;
    passHandles.skinningComputeCS    = skinningComputeCS;
    passHandles.skinningCB           = skinningCB;
    passHandles.defaultPSO           = defaultPSO;
    passHandles.wireframePSO         = wireframePSO;
    passHandles.skyShader            = skydomeShader;
    passHandles.sunMoonShader        = sunMoonShader;
    passHandles.skyPSO               = skydomePSO;
    passHandles.sunMoonPSO           = sunMoonPSO;
    if (skydomeMesh) {
        passHandles.skyVB         = skydomeMesh->vertexBuffer;
        passHandles.skyIB         = skydomeMesh->indexBuffer;
        passHandles.skyIndexCount = skydomeMesh->indexCount;
    }
    passHandles.atmosphereCB         = atmCB;
    passHandles.skyEnvCubeRT         = skyEnvCubeRT;
    passHandles.skyCaptureFrameCB    = skyCaptureFrameCB;
    passHandles.lightProbeProjectCS         = lightProbeProjectCS;
    passHandles.lightProbeProjectCB         = lightProbeProjectCB;
    passHandles.lightProbeCaptureFrameCB    = lightProbeCaptureFrameCB;
    passHandles.lightProbeCaptureAdvancedCB = lightProbeCaptureAdvancedCB;
    passHandles.lightProbeDilateCS          = lightProbeDilateCS;
    passHandles.lightProbeDilateCB          = lightProbeDilateCB;
    passHandles.lightProbeFacingShader      = lightProbeFacingShader;
    passHandles.lightProbeSH[0]             = {};
    passHandles.lightProbeSH[1]             = {};
    passHandles.particleShader       = particleShader;
    passHandles.particlePSO          = particlePSO;
    passHandles.particleAlphaPSO     = particleAlphaPSO;
    passHandles.particlePremultipliedPSO = particlePremultipliedPSO;
    passHandles.particleIB           = particleIB;
    passHandles.particleGpuSimCS     = particleGpuSimCS;
    passHandles.particleGpuSortKeysCS  = particleGpuSortKeysCS;
    passHandles.particleGpuSortStepCS  = particleGpuSortStepCS;
    passHandles.particleGpuSortLocalCS = particleGpuSortLocalCS;
    passHandles.particleGpuMeshShader  = particleGpuMeshShader;
    passHandles.particleSelfShadowShader = particleSelfShadowShader;
    passHandles.particleSelfShadowRT      = particleSelfShadowRT;
    passHandles.particleSelfShadowFrameCB = particleSelfShadowFrameCB;
    passHandles.particleGpuShader    = particleGpuShader;
    passHandles.particleGpuPSO       = particleGpuPSO;
    passHandles.particleGpuAlphaPSO  = particleGpuAlphaPSO;
    passHandles.particleGpuPremultipliedPSO = particleGpuPremultipliedPSO;
    passHandles.trailShader          = trailShader;
    passHandles.trailPSO             = trailPSO;
    passHandles.meshTrailShader      = meshTrailShader;
    passHandles.skinnedMeshTrailShader = skinnedMeshTrailShader;
    passHandles.meshTrailPSO         = meshTrailPSO;
    passHandles.meshTrailDoubleSidedPSO = meshTrailDoubleSidedPSO;
    passHandles.gbufferShader        = gbufferShader;
    passHandles.gbufferInstancedShader = gbufferInstancedShader;
    passHandles.gbufferSkinnedShader   = gbufferSkinnedShader;
    passHandles.deferredLightingShader = deferredLightingShader;
    passHandles.depthCopyShader      = depthCopyShader;
    passHandles.clusterCullCS        = clusterCullCS;
    passHandles.punctualLightBuffer  = punctualLightPool.Acquire(
        resources, kMaxPunctualLights, static_cast<uint32_t>(sizeof(PunctualLightGPU)));
    passHandles.clusterIndexBuffer   = clusterIndexBuffer;
    passHandles.clusterCB            = clusterCB;
    passHandles.clusterLinearCB      = clusterLinearCB;

    /// @name Advanced Graphics ハンドルを passHandles に束縛
    passHandles.advancedGraphicsCB   = advancedGraphicsCB;
    if (rs.lutColorGrading.enabled) {
        const uint64_t lutHash = HashLutSettings(rs.lutColorGrading);
        const bool lutResourceAlive = resources.Get(proceduralColorLut) != nullptr;
        if (!lutResourceAlive || proceduralColorLutHash != lutHash) {
            if (lutResourceAlive)
                resources.Release(proceduralColorLut);
            const std::vector<uint8_t> pixels = GenerateProceduralColorLut(rs.lutColorGrading);
            proceduralColorLut = resources.CreateTexture3D(
                pixels.data(), PROCEDURAL_LUT_SIZE, PROCEDURAL_LUT_SIZE, PROCEDURAL_LUT_SIZE);
            proceduralColorLutHash = lutHash;
        }
    }
    passHandles.proceduralColorLut = proceduralColorLut;
    /// @note IBL BRDF LUT: static ComputeTexture。IBLBakePass が初回フレームで書き込む。
    passHandles.iblBrdfLut           = iblBrdfLut;
    passHandles.iblBrdfBakeShader    = iblBrdfBakeShader;
    /// @note IBL キューブマップ: RenderSettings に指定されたパスを毎フレーム LoadTexture でキャッシュ参照する。
    /// @note LoadTexture は内部でキャッシュするため、毎フレーム呼んでも I/O は初回のみ。
    if (!rs.ibl.irradiancePath.empty())
        passHandles.iblIrradiance = resources.LoadTexture(rs.ibl.irradiancePath);
    if (!rs.ibl.prefilterPath.empty())
        passHandles.iblPrefilter  = resources.LoadTexture(rs.ibl.prefilterPath);
    /// @note SSR
    passHandles.ssrResult            = ssrResult;
    passHandles.ssrShader            = ssrShader;
    /// @note Volumetric Lighting
    passHandles.volumetricResult     = volumetricResult;
    passHandles.volumetricShader     = volumetricShader;
    /// @note TAA (ping-pong)
    passHandles.taaHistoryA          = taaHistoryA;
    passHandles.taaHistoryB          = taaHistoryB;
    passHandles.taaFlip              = viewTargets.taaFlip;
    passHandles.taaShader            = taaShader;
    passHandles.taaPSO               = taaPSO;
    /// @note Motion Blur
    passHandles.motionBlurResult     = motionBlurResult;
    passHandles.motionBlurShader     = motionBlurShader;
    /// @note GTAO
    passHandles.gtaoRaw              = gtaoRaw;
    passHandles.gtaoShader           = gtaoShader;
    passHandles.gtaoBlurShader       = gtaoBlurShader;
    /// @note Contact Shadows
    passHandles.contactShadowShader  = contactShadowShader;
    /// @note Lens Flare
    passHandles.lensFlareShader      = lensFlareShader;
    passHandles.lensFlarePSO         = lensFlarePSO;

    /// @note カメラ視錐台とライト視錐台。Gribb-Hartmann 法は VP 行列の行の和・差から
    /// @note 6 平面を直接出せるので逆行列が要らない。全ジオメトリパスで共有する。
    const math::Frustum cameraFrustum = math::Frustum::FromViewProjection(camera.GetViewProjection());
    const math::Frustum lightFrustum  = math::Frustum::FromViewProjection(lightVP);
    OcclusionCuller occlusionCuller;

    /// @note 参照メンバーまでは集成体初期化で埋めざるを得ないが、それ以降は名前付きで代入する。
    /// @note 位置指定のままだと RenderPassContext へフィールドを 1 つ挿すだけで以降が全部ずれる。
    RenderPassContext passCtx{
        scene, renderer, resources, camera, rs,
        outputRT, cullingMask, passHandles
    };
    passCtx.frustumCullingEnabled   = resolvedCulling.frustumCulling;
    passCtx.occlusionCullingEnabled = resolvedCulling.occlusionCulling;
    passCtx.cullingBoundsPadding    = resolvedCulling.cullingBoundsPadding;
    passCtx.cullMaxDistance         = resolvedCulling.maxDrawDistance;
    passCtx.cullDistanceSpherical   = resolvedCulling.cullDistanceSpherical;
    passCtx.smallObjectScreenHeight = resolvedCulling.smallObjectScreenHeight;
    for (int i = 0; i < kCullLayerCount; ++i) {
        passCtx.cullLayerDistances[i] = resolvedCulling.layerCullDistances[i];
        if (resolvedCulling.layerCullDistances[i] > 0.0f)
            passCtx.hasLayerCullDistances = true;
    }
    /// @note 極小オブジェクト判定用の射影スケール = 1/tan(fovY/2)。
    /// @note GetProjectionMatrix() は毎回行列を組み直すので、オブジェクトごとに呼ぶと判定より高い。
    passCtx.cullProjScaleY    = camera.GetProjectionMatrix().m[1][1];
    passCtx.cullOrthographic  =
        camera.m_projection == renderer::ProjectionMode::Orthographic;
    passCtx.cullCameraForward = camera.GetForward();
    passCtx.width                   = sHdrW;
    passCtx.height                  = sHdrH;
    /// @note 名前 → 実ハンドルの登録簿は、下の DeclareTarget / DeclareTexture から
    /// @note RenderPipeline が組み立てる。ここに 2 つ目の一覧は置かない。

    passCtx.uiOptions               = uiOptions;
    passCtx.outputWidth             = nativeW;
    passCtx.outputHeight            = nativeH;
    /// @note ポストプロセスチェーンの終着点。等倍なら従来どおり outputRT へ直接書き切る。
    passCtx.chainOutputRT           = (needsUpscale && upscaleSrcRT.IsValid()) ? upscaleSrcRT : outputRT;
    /// @note TAA サブピクセルジッター。8 フレーム周期の Halton(2,3) をピクセル内 ±0.5 に写す。
    /// @note 周期 8 は収束の速さと品質の標準的な折衷。TAA が無効なフレームは 0 のまま
    /// @note (ジッターだけ残すと画面全体が揺れて見える)。
    if (rs.IsTaaActive() && sHdrW > 0u && sHdrH > 0u) {
        constexpr uint32_t kTaaJitterPeriod = 8u;
        const uint32_t sampleIndex = viewTargets.taaFrameIndex % kTaaJitterPeriod + 1u;
        const float offsetPxX = HaltonRadicalInverse(sampleIndex, 2u) - 0.5f;
        const float offsetPxY = HaltonRadicalInverse(sampleIndex, 3u) - 0.5f;
        /// @note ピクセル → NDC。NDC の Y は上向きなので符号を反転する。
        passCtx.taaJitterNdcX =  2.0f * offsetPxX / static_cast<float>(sHdrW);
        passCtx.taaJitterNdcY = -2.0f * offsetPxY / static_cast<float>(sHdrH);
        ++viewTargets.taaFrameIndex;
    } else {
        viewTargets.taaFrameIndex = 0u;
        viewTargets.taaHistoryValid = false;
    }
    passCtx.selectionOutlineEnabled = selectionOutlineEnabled;
    passCtx.objectMaskEnabled      = objectMaskEnabled;
    /// @note 登録条件 (Forward: forwardGBufferPrepass / Deferred: useGBufferOpaquePipeline) と
    /// @note ExecuteSSRPass の早期 return を合わせた «本当に走るか»。
    passCtx.ssrPassActive =
        rs.ssr.enabled && screenSpaceReady &&
        ssrShader.IsValid() && ssrResult.IsValid() && gbufferRT.IsValid();
    /// @note UI 要素の矩形も 3D と同じ選択マスクへ乗せ、輪郭の描き方を 1 か所に保つ。
    /// @note 寸法が nativeW/H なのは、UI がポストプロセス後の outputRT へ実寸で描かれ、
    /// @note Canvas Scaler の解釈も出力実寸で決まるため (渡すのはクリップ空間の行列)。
    if (selectionOutlineEnabled && uiOptions && uiOptions->enabled && uiOptions->context) {
        passCtx.appendUISelectionMask = [&]() {
            const float uiWidth = uiOptions->viewportWidth > 0.0f
                ? uiOptions->viewportWidth
                : static_cast<float>(nativeW);
            const float uiHeight = uiOptions->viewportHeight > 0.0f
                ? uiOptions->viewportHeight
                : static_cast<float>(nativeH);
            UISelectionMaskSystem(
                scene, renderer, resources, *uiOptions->context,
                uiWidth, uiHeight,
                [&](GameObject& go) { return IsSelectedForOutline(go, passCtx.settings); },
                camera.m_position, camera.m_rotation, camera.GetViewProjection(),
                uiOptions->targetView);
        };
    }
    passCtx.lightData               = lightData;
    passCtx.lightVP                 = lightVP;
    passCtx.lightView               = lightView;
    passCtx.lightEyePos             = lightPos;
    passCtx.isDeferred              = useGBufferOpaquePipeline;
    passCtx.ssaoEnabled             = ssaoEnabled;
    passCtx.gbufferDepthReady       = screenSpaceReady;

    /// @name 前方描画のマテリアルへ渡す画面空間の遮蔽
    /// @note GTAO と SSAO は排他 (IsGtaoActive が解決済み)。走った方を 1 つのスロットへ入れる。
    /// @note Deferred でも渡すのは、半透明・エフェクト・スキンドが Forward で描かれ
    /// @note DeferredLighting を通らないため。本体は共有ヘッダーを外しているので二重適用にならない。
    /// @note kHalfResScale は半解像度で焼いた AO / 接触影へ svPosition.xy を落とす係数。
    /// @note 強度は b8 が運ぶが、あれを組むのは IBL 解決後なのでここでは値だけ決める。
    constexpr float kHalfResScale = 0.5f;
    float screenAoStrength = 0.0f;
    float screenContactShadowStrength = 0.0f;

    if (screenSpaceReady && rs.IsGtaoActive() && gtaoBlur.IsValid()) {
        passCtx.screenAoTexture = gtaoBlur;
        /// @note GTAO の出力は gtaoIntensity を織り込み済み。ここで再度掛けると二重になる。
        screenAoStrength = 1.0f;
    } else if (ssaoEnabled) {
        passCtx.screenAoTexture = ssaoBlur;
        screenAoStrength = rs.postProcess.ambientOcclusion.intensity;
    }
    if (screenSpaceReady && rs.contactShadow.enabled && contactShadowResult.IsValid()) {
        passCtx.screenContactShadowTexture = contactShadowResult;
        /// @note 強度はマスク生成 CS 側で織り込み済み。ここは「適用するか」だけを決める。
        screenContactShadowStrength = 1.0f;
    }

    /// @name ライト供給モードの決定と定数の組み立て
    /// @note 4 つのパイプラインで「どのライトが効くか」を揃える。
    /// @note Forward/Deferred → LINEAR (全数走査)、Forward+/Deferred+ → CLUSTERED (クラスタで絞る)。
    /// @note どちらも同じ StructuredBuffer を読むので "+" は性能の選択であって絵の選択ではない。
    /// @note b3 のレガシー経路を既定から外したのは、点 8 / スポット 4 で打ち切るため 9 個目の
    /// @note 電球が黙って消えていたから。LEGACY はリソース確保失敗時とエディタのプレビュー
    /// @note 経路 (b9 / t29 を束縛しない) のフォールバックとして残る。
    /// @note useGBufferOpaquePipeline はライトの供給方法と直交する軸なので触らない。
    const bool punctualBufferReady =
        passHandles.punctualLightBuffer.IsValid() && clusterCB.IsValid();
    /// @note クラスタで絞れるか。カリング CS とインデックスバッファが揃って初めて成立する。
    const bool canCullClusters = rs.UsesClusteredLighting()
        && clusterIndexBuffer.IsValid() && clusterCullCS.IsValid()
        && !rs.clustered.forceAllLights;

    ClusterLightMode clusterMode = ClusterLightMode::Legacy;
    if (punctualBufferReady && !rs.IsUnlit()) {
        clusterMode = canCullClusters ? ClusterLightMode::Clustered
                                      : ClusterLightMode::Linear;
    }
    const bool clusteredEnabled = (clusterMode == ClusterLightMode::Clustered);

    passCtx.punctualLights    = std::move(punctualLights);
    passCtx.clusterLightMode  = clusterMode;
    /// @note 霧のフレーム間状態は描画中のビューが持つ (SceneView / GameView で混ざらないように)。
    passCtx.froxelFogState    = &viewTargets.froxelState;
    passCtx.clusterDebugHeatmap = clusteredEnabled && rs.clustered.debugHeatmap;

    if (punctualBufferReady) {
        /// @note ライト配列は毎フレーム転送する。上限 256 本 × 96B = 24KB で、部分更新の価値はない。
        if (!passCtx.punctualLights.empty()) {
            resources.Update(passHandles.punctualLightBuffer, passCtx.punctualLights.data(),
                             passCtx.punctualLights.size() * sizeof(PunctualLightGPU));
        }

        /// @note 指数分割の係数。slice = log(viewZ) * scale + bias が [0, GridZ) に収まるよう決める。
        /// @note slice(nearZ) = 0 / slice(clusterFar) = GridZ
        /// @note 対数なのは、等間隔だと手前の 1 スライスが広くなりすぎて何も落とせないため。
        const float nearZ = (std::max)(camera.m_near, 0.01f);
        const float farZ  = (std::max)((std::min)(camera.m_far, rs.clustered.maxDistance), nearZ * 2.0f);
        const float logRatio = std::log(farZ / nearZ);

        ClusterConstantsCB clusterData{};
        clusterData.clusterTilePx[0] = static_cast<float>(sHdrW) / static_cast<float>(kClusterGridX);
        clusterData.clusterTilePx[1] = static_cast<float>(sHdrH) / static_cast<float>(kClusterGridY);
        clusterData.clusterSliceScale = static_cast<float>(kClusterGridZ) / logRatio;
        clusterData.clusterSliceBias  = -static_cast<float>(kClusterGridZ) * std::log(nearZ) / logRatio;
        clusterData.clusterLightMode  = static_cast<uint32_t>(clusterMode);
        clusterData.punctualLightCount = static_cast<uint32_t>(passCtx.punctualLights.size());
        clusterData.clusterDebugMode  = passCtx.clusterDebugHeatmap ? 1u : 0u;
        resources.Update(clusterCB, &clusterData, sizeof(ClusterConstantsCB));

        /// @note 別視点パス用。Legacy を Linear へ持ち上げると空のバッファを全数走査するので落とす。
        /// @note ヒートマップも切る (メインカメラのタイル分布を別視点で塗っても意味がない)。
        ClusterConstantsCB linearData = clusterData;
        if (clusterMode != ClusterLightMode::Legacy)
            linearData.clusterLightMode = static_cast<uint32_t>(ClusterLightMode::Linear);
        linearData.clusterDebugMode = 0u;
        resources.Update(clusterLinearCB, &linearData, sizeof(ClusterConstantsCB));
    }
    passCtx.cameraFrustum           = &cameraFrustum;
    passCtx.lightFrustum            = &lightFrustum;
    passCtx.occlusionCuller         = &occlusionCuller;
    passCtx.physicsWorld  = physicsWorld;
    /// @note 空連動 IBL の永続状態 (フレームをまたぐ)
    passCtx.environmentResources = &sEnvironmentResources;
    /// @note 雲シャドウ (Phase C): SkyRenderer から読んだ params + 現在時刻を影パスへ渡す。
    passCtx.cloudShadowStrength = skyCloudShadowStrength;
    passCtx.cloudShadowCoverage = skyCloudShadowCoverage;
    passCtx.cloudShadowScale    = skyCloudShadowScale;
    passCtx.cloudShadowSpeed    = skyCloudShadowSpeed;
    passCtx.cloudShadowTime     = Time::time;
    /// @note Shadow トグルを CB まで伝える。描画を止めるだけだと、シェーダーは影を切っても
    /// @note PCF ループ (既定 7x7 = 49 タップ) を回し続け、「切っても速くならない」状態になる。
    passCtx.shadowStrength =
        (rs.shadowEnabled && dirCastShadows) ? dirShadowStrength : 0.0f;
    /// @note 単一カスケード相当のバイアス。カスケードごとの値は ShadowCascade::biasNDC が持つ。
    passCtx.shadowBiasNDC  = cascades[0].biasNDC;
    passCtx.shadowCascadeCount = cascadeCount;
    for (int i = 0; i < cascadeCount; ++i)
        passCtx.shadowCascades[i] = cascades[i];

    passCtx.punctualShadowViewCount  = punctualViewCount;
    passCtx.punctualShadowResolution = punctualShadowRes;
    for (int i = 0; i < punctualViewCount; ++i)
        passCtx.punctualShadowViews[i] = punctualViews[i];
    for (int i = 0; i < kMaxLegacyPunctualLights; ++i) {
        passCtx.legacyShadowSlots[i] = legacyShadowSlots[i];
        passCtx.legacyCookieSlots[i] = legacyCookieSlots[i];
    }

    passCtx.lightCookieViewCount = cookieViewCount;
    for (int i = 0; i < cookieViewCount; ++i)
        passCtx.lightCookieViews[i] = cookieViews[i];

    /// @note レガシー経路向けに「大きさを持つ光源」を先頭から数本だけ写す。
    /// @note 1 部屋に数個のものなので上限に当たること自体が稀。並び順が変わらない方が追いやすい。
    passCtx.legacyShapedLightCount = 0;
    for (const PunctualLightGPU& light : passCtx.punctualLights) {
        if (passCtx.legacyShapedLightCount >= kMaxLegacyShapedLights) break;
        const bool shaped = light.type == static_cast<uint32_t>(PunctualLightType::Area)
                         || light.type == static_cast<uint32_t>(PunctualLightType::Sphere)
                         || light.type == static_cast<uint32_t>(PunctualLightType::Tube);
        if (!shaped) continue;
        passCtx.legacyShapedLights[passCtx.legacyShapedLightCount++] = light;
    }
    for (int i = 0; i < kMaxLegacyPunctualLights; ++i)
        passCtx.legacySourceRadius[i] = legacySourceRadius[i];

    /// @name 空連動 IBL: source=DynamicSky のとき空→動的 IBL を用意する
    /// @note AdvancedGraphicsCB / 各 Lit パスより前に焼くことで同フレームで消費できる。
    /// @note キャプチャ先と畳み込み出力は RenderGraph 管理外なのでグラフ実行前に直接呼ぶ。
    /// @note SkyCapture / SkyLightBake は dirty を内部判定し、不要フレームは即 return する。
    bool dynamicIblReady = false;
    float reflectionProbeIntensity = 1.0f;
    int dynamicIblMipCount = 0;
    if (activeIblSource == IblSource::DynamicSky) {
        ExecuteSkyCapturePass(passCtx);
        ExecuteSkyLightBakePass(passCtx);
        if (sEnvironmentResources.HasBakedTextures()) {
            passHandles.iblIrradiance = sEnvironmentResources.skyIrradiance;
            passHandles.iblPrefilter  = sEnvironmentResources.skyPrefilter;
            dynamicIblReady = true;
            dynamicIblMipCount = static_cast<int>(sEnvironmentResources.prefilteredMipCount);
        }
    }

    /// @note 局所 Reflection Probe はカメラが影響範囲内にいるとき、グローバル IBL より優先する。
    /// @note IBL スロットを共有するので、マテリアル側に専用分岐も追加テクスチャも要らない。
    if (auto* localProbe = ExecuteReflectionProbeCapturePass(passCtx)) {
        passHandles.iblIrradiance = localProbe->runtimeIrradiance;
        passHandles.iblPrefilter  = localProbe->runtimePrefilter;
        dynamicIblReady = true;
        reflectionProbeIntensity = localProbe->intensity;
        dynamicIblMipCount = static_cast<int>(localProbe->runtimePrefilterMipCount);
    }

    /// @name AdvancedGraphicsCB (b8) を毎フレーム更新
    /// @note 各パスはここで書いたデータを読むだけなので、更新はこの 1 か所に集中させる。
    if (advancedGraphicsCB.IsValid()) {
        AdvancedGraphicsCB agData{};
        /// @note 未バインド SRV をサンプルさせず、確実に ambient へフォールバックさせるための判定。
        /// @note 動的 IBL は .dds を持たないので dynamicIblReady を別経路として許可する。
        const bool iblResourcesReady =
            (dynamicIblReady || (rs.ibl.enabled && rs.HasValidIblAssets())) &&
            passHandles.iblIrradiance.IsValid() && passHandles.iblPrefilter.IsValid();
        agData.iblIntensity          = iblResourcesReady ? rs.ibl.intensity * reflectionProbeIntensity : 0.0f;
        agData.iblDiffuseScale       = rs.ibl.diffuseScale;
        agData.iblSpecularScale      = rs.ibl.specularScale;
        /// @note 動的 IBL は SkyLightBake が焼いた prefilter mip 数に合わせる (maxMip = mip 数 - 1)。
        agData.iblMaxMipLevel        = dynamicIblReady
            ? dynamicIblMipCount - 1
            : rs.ibl.maxMipLevel;
        agData.ssrMaxDistance        = rs.ssr.maxDistance;
        agData.ssrThickness          = rs.ssr.thickness;
        agData.ssrSteps              = rs.ssr.steps;
        agData.ssrIntensity          = rs.ssr.enabled ? rs.ssr.intensity : 0.0f;
        agData.volLightIntensity     = rs.volumetricLight.enabled ? rs.volumetricLight.intensity : 0.0f;
        agData.volScattering         = rs.volumetricLight.scattering;
        agData.volSteps              = rs.volumetricLight.steps;
        agData.volMaxDist            = rs.volumetricLight.maxDist;
        agData.volMinDist            = rs.volumetricLight.minDist;
        agData.volDensity            = rs.volumetricLight.density;
        agData.volHeightFalloff      = rs.volumetricLight.heightFalloff;
        agData.volHeightStart        = rs.volumetricLight.heightStart;
        agData.volTintR              = rs.volumetricLight.tint[0];
        agData.volTintG              = rs.volumetricLight.tint[1];
        agData.volTintB              = rs.volumetricLight.tint[2];
        agData.volEdgeFade           = rs.volumetricLight.edgeFade;

    /// @note 前方描画のマテリアルが画面空間 AO / 接触影をどれだけ受けるか。
    /// @note 値そのものは上の「前方描画のマテリアルへ渡す画面空間の遮蔽」ブロックで決めてある。
    agData.screenAoStrength            = screenAoStrength;
    agData.screenContactShadowStrength = screenContactShadowStrength;
    agData.screenAoScale               = kHalfResScale;
    agData.screenContactShadowScale    = kHalfResScale;

    /// @note 自動露出。key <= 0 が「無効」の印なので、切ってあるときは 0 のまま渡す。
    /// @note 0.18 は反射率 18% のグレーカード = 写真の露出計が基準にしている明るさ。
    /// @note 結果バッファが無いと Composite は t29 を束縛しない。key を立てたままだと 0 を平均輝度として読み、
    ///       露出が上限へ張り付いて白飛びするので、同じ条件で無効にする (CompositePass の束縛条件と対)。
    agData.autoExposureKey          = (rs.autoExposure.enabled && viewTargets.exposureResult.IsValid()) ? 0.18f : 0.0f;
    agData.autoExposureCompensation = rs.autoExposure.compensation;
    agData.autoExposureMinEV        = rs.autoExposure.minExposureEV;
    agData.autoExposureMaxEV        = (std::max)(rs.autoExposure.maxExposureEV,
                                                 rs.autoExposure.minExposureEV);
        agData.taaFeedback           = viewTargets.taaHistoryValid ? rs.taa.feedback : 0.0f;
        agData.taaJitterX            = passCtx.taaJitterNdcX;
        agData.taaJitterY            = passCtx.taaJitterNdcY;
        agData.motionBlurStrength    = rs.motionBlur.enabled ? rs.motionBlur.strength : 0.0f;
        agData.motionBlurSamples     = rs.motionBlur.samples;
        agData.screenWidth           = static_cast<float>(sHdrW);
        agData.screenHeight          = static_cast<float>(sHdrH);
        agData.gtaoIntensity         = rs.IsGtaoActive()   ? rs.gtao.intensity : 0.0f;
        agData.gtaoRadius            = rs.gtao.radius;
        agData.gtaoSlices            = rs.gtao.slices;
        agData.gtaoStepsPerSlice     = rs.gtao.stepsPerSlice;
        agData.contactShadowStrength = rs.contactShadow.enabled ? rs.contactShadow.strength  : 0.0f;
        agData.contactShadowRayLen   = rs.contactShadow.rayLength;
        agData.contactShadowSteps    = rs.contactShadow.steps;
        agData.contactShadowThick    = rs.contactShadow.thickness;
        agData.lensFlareIntensity    = rs.lensFlare.enabled ? rs.lensFlare.intensity : 0.0f;
        agData.lensFlareGhostCount   = rs.lensFlare.ghostCount;
        agData.lensFlareHaloWidth    = rs.lensFlare.haloWidth;
        agData.lensFlareDistort      = rs.lensFlare.distortion;
        agData.pcssLightRadius       = rs.shadow.pcssLightRadius;
        agData.pcssEnabled           = rs.shadow.pcssEnabled ? 1 : 0;
        agData.lutBlend              = (rs.lutColorGrading.enabled && resources.Get(proceduralColorLut) != nullptr)
            ? rs.lutColorGrading.blend : 0.0f;
        /// @note 天候 — シーンに置かれた WeatherComponent 1 個ぶん。無ければ 0 で素通りする。
        /// @note 地形 (b8 を読めない) と同じ FindActiveWeather を使う。別々に探すと無効化の扱いが食い違う。
        {
            const ActiveWeather weather = FindActiveWeather(scene);
            agData.weatherWetness   = weather.wetness;
            agData.weatherDarkening = weather.darkening;
            agData.weatherPuddle    = weather.puddleAmount;
        }
        /// @note 前フレームの VP 行列 — TAA / Motion Blur が深度再投影で使う。ビュー別に持つ。
        agData.prevViewProjection    = viewTargets.prevViewProjection;
        agData.invPrevViewProjection = viewTargets.invPrevViewProjection;
        /// @note Light Probe Volume: 焼きを進め、焼き上がったボリュームの拡散 GI を IBL キューブに差し替える。
        /// @note 拡散 GI は IBL の拡散項の置き換えなので、IBL が引けないフレームでは効かせない (マテリアルが IBL 分岐に入らない)。
        if (iblResourcesReady) {
            const LightProbeVolumeSelection gi = ExecuteLightProbeBakePass(passCtx, agData);
            const LightProbeVolumeSelection::Entry* slots[2] = { &gi.inner, &gi.outer };
            for (int slot = 0; slot < 2; ++slot) {
                if (!slots[slot]->volume) continue;
                FillLightProbeVolumeConstants(*slots[slot]->owner, *slots[slot]->volume, agData.probeVolumes[slot]);
                passHandles.lightProbeSH[slot] = slots[slot]->volume->runtimeVolume;
            }
            /// @note 鏡面遮蔽の強さは 1 画面に 1 つ。内側 (画面の主役になりやすい方) の設定を使う。
            if (gi.inner.volume)
                agData.probeSpecularOcclusion = std::clamp(gi.inner.volume->specularOcclusion, 0.0f, 1.0f);
        }
        resources.Update(advancedGraphicsCB, &agData, sizeof(AdvancedGraphicsCB));
        /// @note ここはジッターを載せない。両方に載せるとジッター差分がそのまま「動き」として
        /// @note 現れ、履歴が毎フレームずれて収束しない。履歴はピクセル中心で収束した絵なので、
        /// @note 引く座標もピクセル中心でなければならない。
        /// @note シェーダーが今フレームの深度 (Reversed-Z) と並べて使うので GPU 用の行列で持つ。
        viewTargets.prevViewProjection    = camera.GetGpuViewProjection();
        viewTargets.invPrevViewProjection = math::Matrix4::Inverse(viewTargets.prevViewProjection);
    }

    /// @note RenderPipeline にパスを登録
    RenderPipeline& pipeline = viewTargets.pipeline;
    pipeline.BeginBuild();
    /// @note エディターが編集した «このパスは載せない / これを待つ» をこのフレームへ効かせる。
    pipeline.SetPassOverrides(rs.passOverrides);
    pipeline.SetSchedulePolicy(rs.schedulePolicy);
    /// @note 申告を条件で組み立てるパス用。initializer_list には if を書けないので、
    /// @note 読むものが構成で変わるパスは vector を渡す。
    using RA = renderer::RenderGraph::ResourceAccess;
    using RU = renderer::RenderGraph::ResourceUsage;
    profiler::Profiler::BeginSample(
        profiler::ProfilerMarker("RenderSystem::BuildPipeline", "Rendering"));
    /// @name 論理リソースの宣言
    /// @note 申告 (依存解析に使う «形») と実体 (名前 → ハンドル) を同じ 1 行で渡す。宣言と登録を
    /// @note 分けると、片方だけ足しても Plan が通り «申告したのに実体が無い» が静かに成立してしまう。
    /// @note 登録簿は RenderPipeline がここから組み立てる。
    using RK = renderer::RenderGraph::ResourceKind;
    constexpr auto kResFormat = renderer::Format::RGBA16F;

    /// @note 内部解像度・フレーム内だけ生きる RT。違うのは MRT 枚数と深度の有無だけ。
    const auto declareViewTarget = [&](const char* name,
                                       renderer::ResourceHandle<renderer::RenderTargetTag> handle,
                                       uint32_t colorCount, bool withDepth) {
        pipeline.DeclareTarget(name, handle,
            { RK::RenderTarget, sHdrW, sHdrH, kResFormat, colorCount, withDepth, false, true });
    };
    /// @note CS 出力のテクスチャ。解像度以外の «形» は全部同じ。
    const auto declareViewTexture = [&](const char* name,
                                        renderer::ResourceHandle<renderer::TextureTag> handle,
                                        uint32_t width, uint32_t height) {
        pipeline.DeclareTexture(name, handle,
            { RK::Texture, width, height, kResFormat, 1, false, false, true });
    };

    /// @note Output だけは出力先そのものなので実寸で申告する (中間 RT は内部解像度)。
    pipeline.DeclareTarget("Output", outputRT,
        { RK::RenderTarget, nativeW, nativeH, kResFormat, 1, true, true, false });
    pipeline.DeclareTarget("ShadowMap", shadowMapRT,
        { RK::RenderTarget, rs.shadow.mapResolution, rs.shadow.mapResolution, kResFormat, 0, true, false, false });
    pipeline.DeclareTarget("PunctualShadowMap", punctualShadowRT,
        { RK::RenderTarget, punctualShadowRes, punctualShadowRes, kResFormat, 0, true, false, false });
    pipeline.DeclareTarget("LightCookieAtlas", lightCookieRT,
        { RK::RenderTarget, kLightCookieAtlasWidth, kLightCookieAtlasHeight, kResFormat, 1, true, false, false });

    declareViewTarget("HDR",                hdrRT,                  1, true);
    declareViewTarget("LDR",                ldrRT,                  1, false);
    declareViewTarget("SelectionMask",      selectionMaskRT,        1, true);
    declareViewTarget("Outline",            outlineRT,              1, false);
    declareViewTarget("ObjectMask",         objectMaskRT,           1, true);
    declareViewTarget("Velocity",           velocityRT,             1, true);
    declareViewTarget("CustomPostProcess0", customPostProcessRT[0], 1, false);
    declareViewTarget("CustomPostProcess1", customPostProcessRT[1], 1, false);

    /// @note 実体は upscaleSrcRT。等倍のフレームは誰も触らないので申告もしない。
    const bool upscaleActive = needsUpscale && upscaleSrcRT.IsValid();
    if (upscaleActive)
        declareViewTarget("UpscaleSrc", upscaleSrcRT, 1, false);

    declareViewTexture("Bloom",            bloomFull,        sHdrW, sHdrH);
    declareViewTexture("SSRResult",        ssrResult,        sHdrW, sHdrH);
    declareViewTexture("MotionBlurResult", motionBlurResult, sHdrW, sHdrH);
    declareViewTexture("VolumetricResult", volumetricResult, sHdrW, sHdrH);

    /// @note Forward もプリパスで GBuffer へ描くので、ここを Deferred 限定にすると
    /// @note 「宣言されていないリソース」への書き込みになり RenderGraph の検証が落ちる。
    if (screenSpaceReady)
        pipeline.DeclareTarget("GBuffer", gbufferRT,
            { RK::RenderTarget, sHdrW, sHdrH, kResFormat, 2, true, false, false });

    /// @note AO と接触影は半解像度で持つ (実体は curW/2 x curH/2)。ここをフル解像度で
    /// @note 申告していると、エイリアシングが全画面 RT と同じ枠を貸してしまう。
    const uint32_t halfW = (std::max)(1u, sHdrW / 2);
    const uint32_t halfH = (std::max)(1u, sHdrH / 2);
    declareViewTexture("LensFlareSource", bloomHalf, halfW, halfH);
    if (ssaoEnabled)
        declareViewTexture("SSAO", ssaoBlur, halfW, halfH);
    /// @note GTAO / ContactShadows は GBuffer を読んで独自の UAV へ書く。専用名で宣言しないと
    /// @note GBuffer への偽書き込みとみなされ、DeferredLighting との依存順が崩れる。
    if (screenSpaceReady && rs.IsGtaoActive())
        declareViewTexture("GTAOResult", gtaoBlur, halfW, halfH);
    if (screenSpaceReady && rs.contactShadow.enabled)
        declareViewTexture("ContactShadowResult", contactShadowResult, halfW, halfH);
    pipeline.SetOutputs({ "Output" });

    /// @note IBL BRDF LUT 焼き付け。512x512 の積分テーブルはシーンにも設定にも依存しない定数なので、
    /// @note 毎フレーム呼ぶが実処理は世代追跡で初回のみ走る。
    pipeline.AddPass<IBLBrdfBakePass>();

    scene.ClearUserRenderPasses();

    auto appendQueuedUserPasses = [&](UserRenderPassInjectionPoint injectionPoint) {
        for (const auto& desc : scene.GetUserRenderPasses()) {
            if (desc.injectionPoint != injectionPoint)
                continue;
            assert(!desc.name.empty() && "UserRenderPassDesc.name is required");
            auto execute = desc.execute;
            pipeline.AddRawPass(desc.name, desc.accesses, [&, execute]() {
                if (execute)
                    execute(passCtx);
            }, desc.allowCulling);
        }
    };

    /// @note HDR の段 (AfterOpaque / SceneHDR) のカスタムパスを積む。どちらも hdrRT を
    /// @note 読んで書くので、違うのは «パイプラインのどこへ挿すか» だけ。
    /// @note マスクを reads に入れないと、グラフは «マスクを描く前に» このパスを走らせてよいことになる。
    /// @note 読む宣言をしていない効果でも同じ段に読む効果が混ざれば順序は共有されるので、
    /// @note 有無で分けずまとめて宣言する。
    auto appendCustomHdrPasses = [&](const char* label, const std::vector<uint32_t>& indices) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(indices.size()); ++i) {
            const uint32_t customIndex = indices[i];
            const auto body = [&, customIndex]() { ExecuteCustomHdrPass(passCtx, customIndex); };
            const std::string name = label + std::to_string(i);
            if (objectMaskEnabled)
                pipeline.AddRawPass(name, { "HDR", "ObjectMask" }, { "HDR" }, body);
            else
                pipeline.AddRawPass(name, { "HDR" }, { "HDR" }, body);
        }
    };

    /// @name Skinning (コンピュート)
    /// @note Shadow より前。変形結果をシャドウ・GBuffer・Forward が共有するので 1 回で済む。
    /// @note 出力は論理リソースではなく SkinnedMeshRenderer の頂点バッファなので依存には乗せない。
    pipeline.AddPass<SkinningComputePass>();

    /// @name クラスタライトカリング
    /// @note Shadow より前。どちらの経路も同じ結果を読むので 1 回で済む。
    /// @note 出力は StructuredBuffer で論理リソースではないため reads/writes は空。
    if (clusteredEnabled) {
        pipeline.AddPass<ClusterLightCullPass>();
    }

    /// @name Light Cookie
    /// @note Cookie の顔ぶれが変わったフレームだけアトラスを焼き直す。
    pipeline.AddPass<LightCookiePass>();

    /// @name Shadow
    /// @note Directional の CSM と Spot / Point のアトラスを 1 パスで描く。caster の収集と
    /// @note ソートを両者で共有するため、パスを分けるとシーン走査が丸ごと 2 回になる。
    pipeline.AddPass<ShadowPass>();

    /// @name Forward or Deferred
    if (!useGBufferOpaquePipeline) {
        /// @note 画面空間系のための GBuffer プリパス。ライティングはせず法線・深度・roughness だけ書く。
        /// @note 以降の SSAO / GTAO / SSR / 接触影は Deferred と同じ入力を読む。
        if (forwardGBufferPrepass) {
            pipeline.AddPass<GBufferPass>(GBufferPassMode::DepthNormalPrepass);

            /// @note 地形も GBuffer へ入れる。飛ばすと地形が AO の遮蔽者にも受け手にもならず、
            /// @note 「Deferred では地形に AO が乗るのに Forward では乗らない」差が残る。
            /// @note 独立したパスにするのは Setup を呼ばせるため。登録順を直後に置けば実行順は変わらない。
            pipeline.AddPass<TerrainRenderPass>(TerrainDrawMode::GBuffer);

            /// @note AO と接触影は ForwardOpaque より前。Forward には合流点が無く各マテリアルが
            /// @note 自分の画素で読むので、本描画の時点で結果が揃っていないと何も掛からない。
            /// @note 有効条件と申告はパス側が持つ (PostProcessPasses.hpp)。ここが決めるのは位置だけ。
            pipeline.AddPass<GTAOPass>();
            pipeline.AddPass<ContactShadowsPass>();
            pipeline.AddPass<SSAOPass>();
        }

        pipeline.AddPass<ForwardOpaquePass>();
    }

    if (useGBufferOpaquePipeline) {
        pipeline.AddPass<GBufferPass>(GBufferPassMode::Deferred);

        /// @note Deferred Terrain — GBuffer へ書く。DepthCopy / AO / Lighting より前に置くことで
        /// @note GTAO/SSAO/ContactShadows/SSR/IBL が地形へも効く。
        pipeline.AddPass<TerrainRenderPass>(TerrainDrawMode::GBuffer);

        pipeline.AddPass<DeferredDepthCopyPass>();
    }

    /// @name Terrain (Forward フォールバック用)
    /// @note ForwardOpaque / Sky の間に HDR RT (depth 共有) へ描く。Sky より前なので空が被らない。
    /// @note 通常は上の GBuffer フェーズで描画済みなのでここは通らない。
    if (!useGBufferOpaquePipeline) {
        pipeline.AddPass<TerrainRenderPass>(TerrainDrawMode::Forward);
    }

    /// @note Sky / SunMoon — Forward フォールバックではここ (不透明描画後・雲前)。
    /// @note GBuffer 経路では Terrain が HDR を書かないので Sky と DeferredDepthCopy の順序保証が
    /// @note 失われ、DepthCopy のクリアで空が消える。だから Lighting 後 (下のブロック) に描く。
    if (!useGBufferOpaquePipeline) {
        pipeline.AddPass<SkyPass>();
        pipeline.AddPass<SunMoonPass>();

        /// @note VolumetricCloud — GBuffer フォールバックの Forward では Sky 後・透明物前に HDR へ合成する。
        /// @note 空を背景にしつつ、後続の水面・透明エフェクトで上書きできる順序にする。
        pipeline.AddPass<VolumetricCloudPass>();

        /// @note SSR — Forward でもプリパスの GBuffer から反射を計算する。
        /// @note 映すのはライティング済みのシーンなので HDR が出揃った後に置く。
        /// @note 設定のトグルは SSRPass::IsEnabled。ここで見るのは GBuffer があるかだけ。
        if (forwardGBufferPrepass)
            pipeline.AddPass<SSRPass>();
    }

    /// @name SSAO + Deferred Lighting
    if (useGBufferOpaquePipeline) {
        /// @note AO と接触影は DeferredLighting より前。専用名で書くので、Lighting は
        /// @note "GTAOResult" / "ContactShadowResult" を正確な依存で待てる。
        /// @note 申告と有効条件はパス側 (PostProcessPasses.hpp)。Forward 側と同じ 3 行になる。
        pipeline.AddPass<GTAOPass>();
        pipeline.AddPass<ContactShadowsPass>();
        pipeline.AddPass<SSAOPass>();
        pipeline.AddPass<DeferredLightingPass>();

        /// @note Sky / SunMoon — GBuffer ライティング後に HDR へ描く。最遠 (Reversed-Z で 0) の画素だけを埋め、
        /// @note HDR 依存チェーンで DeferredDepthCopy のクリアより確実に後段になる。
        pipeline.AddPass<SkyPass>();
        pipeline.AddPass<SunMoonPass>();

        /// @note VolumetricCloud — GBuffer Lighting / Sky 後・透明物前に HDR へ合成する。
        /// @note Lighting・空に上書きされず、透明物や水面を雲の手前に描ける順序にする。
        pipeline.AddPass<VolumetricCloudPass>();

        /// @note Deferred の中で «前方描画される» 2 パス。どちらも BindForwardShadingResources を
        /// @note 通るので、Forward パスと同じく Spot/Point の影 (t28) と Cookie (t31) を引く。
        /// @note 申告しないと依存辺が張られず、Shadow / LightCookie より先に走ってよいことになる。
        pipeline.AddPass<DeferredSkinnedForwardPass>();
        pipeline.AddPass<FiberRenderPass>();
        pipeline.AddPass<DeferredForwardTransparentPass>();

        /// @note SSR — 透明オブジェクト通過後の深度を使うので DeferredForwardTransparent の後。
        /// @note 実行条件はパイプラインの選択ではなく GBuffer の有無 (SSRPass::IsEnabled)。
        pipeline.AddPass<SSRPass>();
    }

    /// @note VolumetricLight — ゴッドレイ・光柱を HDR バッファへ加算合成する。半透明より前に置く:
    /// @note レイは不透明深度で止まるが、水や半透明は深度を書かないためレイの終端が水底になる。
    /// @note 水面描画の後に足すと水底までの光芒が水面手前に描かれ水が光の靄で塗り潰されるので、
    /// @note 不透明深度が確定したここで先に足す。WaterCaustics が Water の前必須なのと同じ理由。
    pipeline.AddPass<VolumetricLightPass>();

    /// @note WaterCaustics — 水面下の不透明ジオメトリへコースティクスを投影してから、水面本体を透明描画する。
    /// @note Water の後に加算すると水面そのものへ模様が乗りやすいため、深度が不透明物だけを指す段階で実行する。
    pipeline.AddPass<WaterCausticsPass>();

    pipeline.AddPass<WaterRenderPass>();

    for (EntityID id : scene.GetEntities<ScriptComponent>()) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        if (!sc || !go || !go->activeInHierarchy())
            continue;
        for (auto& entry : sc->scripts) {
            if (!entry.script || !entry.script->enabled)
                continue;
            entry.script->SetContext(&scene, go);
            entry.script->ExecuteCallback(&Script::OnSetupRenderPasses, pipeline, passCtx);
        }
    }

    appendQueuedUserPasses(UserRenderPassInjectionPoint::AfterOpaque);

    /// @note オブジェクトマスク。描くのはジオメトリなので不透明が出揃ったここで済ませる。
    /// @note 半透明より前でよい: マスクの中身は «不透明の形と、その時点の深度» で決まり、
    /// @note 深度を書かない半透明を待っても結果は変わらない。ここより後ろへ置くと、
    /// @note AfterOpaque 段のカスタムパスがマスクを読めなくなる (順序が閉じない)。
    pipeline.AddPass<ObjectMaskPass>();

    /// @name AfterOpaque 段のユーザーシェーダー
    /// @note 背景だけが描かれていて、デカール・トレイル・パーティクル・半透明はまだ乗っていない。
    /// @note 画面を歪める効果をこの後 (SceneHDR) に置くと、既に描かれたパーティクルごと曲がって
    /// @note «エフェクトだけ別の場所に居る» 絵になる。
    appendCustomHdrPasses("CustomAfterOpaque", customAfterOpaqueIndices);

    /// @name デカール用深度スナップショット
    /// @note 深度専用 RT (colorCount = 0)。カラーを持つ RT と貸し回してはいけない。
    declareViewTarget("DecalDepth", decalDepthRT, 0, true);
    pipeline.AddPass<DecalDepthCopyPass>();

    /// @name Decal + Trail + Particle
    pipeline.AddPass<DecalPass>();

    pipeline.AddPass<MeshTrailRenderPass>();
    pipeline.AddPass<TrailRenderPass>();

    /// @note ShadowMap は粒子の自己影が読む (t8)。申告していないと影より前に走れてしまう。
    /// @note 粒子より前に登録するのは、粒子が同じフレームの霧を読んで «自分の奥行きの霧» を逆算する
    /// @note (ParticleLighting.hlsli の ApplyParticleFog) ため。依存を申告しあわない 2 パスは登録順に並ぶ。
    pipeline.AddPass<FroxelFogPass>();

    /// @note PunctualShadowMap / LightCookieAtlas は «点光源を受ける» .mat の粒子が読む (ParticleLighting.hlsli)。
    pipeline.AddPass<ParticlePass>();

    /// @note Overdraw 可視化は診断表示。有効なときだけ Particle の直後に HDR を上書きする。
    /// @note GPU 時間を Particle パスの実測値と混ぜないよう、別パスとして計測させる。
    pipeline.AddPass<ParticleOverdrawPass>();

    /// @note TAA の反応マスク。HDR へは書かないが、Particle の後・Composite (→ TAA) の前に並べるために
    /// @note HDR の書き手として申告する (Overdraw と同じ申告の仕方)。
    pipeline.AddPass<ParticleReactivePass>();

    appendQueuedUserPasses(UserRenderPassInjectionPoint::AfterTransparent);

    /// @name Selection / Debug
    pipeline.AddPass<SelectionMaskPass>();

    /// @name SceneHDR 段のユーザーシェーダー
    /// @note 絵が出揃っていて、まだブルームにも露出にも触れていない唯一の場所。
    /// @note Bloom より後ろへ置くと «光っているのに滲まない»、AutoExposure より後ろへ
    /// @note 置くと «明るくしたのに露出が反応しない» という、段を選べる意味が消える並びになる。
    /// @note デバッグ描画より前なのは、ギズモを効果で歪ませないため。
    appendCustomHdrPasses("CustomSceneHDR", customSceneHdrIndices);

    /// @note 自動露出はデバッグ描画より前。測るのは «シーンの明るさ» で、グリッド・ギズモ・
    /// @note コライダー・NavMesh は Scene View にしか無い。後ろへ置くと Scene View だけ露出が
    /// @note Game View と食い違う (RenderGraph は登録順より前の書き手を読み手の世代とする)。
    /// @note MotionBlur / LensFlare より前なのは、フレアを測光へ入れると «明るい→露出が下がる→
    /// @note フレアが弱る» の輪ができるため。Bloom は HDR を書かないので位置に関わらず同じ。
    /// @note 出力は StructuredBuffer (Composite が t29 で読む) で論理リソースに乗らないため、
    /// @note FroxelFog と同じ理由でカリング対象から外す。
    pipeline.AddPass<AutoExposurePass>();

    /// @note debug.Draw* の duration はビューの数や表示の入り切りに関係なくフレーム 1 回減らす
    /// @note (Scene 側でフレーム番号ガード済み)。表示を切ったビューでもキューが溜まらない。
    scene.TickScriptDebugDrawCommands(Time::deltaTime);
    /// @note OnDrawGizmos はここで 1 ビュー 1 回だけ記録し、深度あり (HDR) と深度なし (LDR 終端) の
    /// @note 2 パスへ流す。showScriptGizmos が false のビューでは呼ばない。
    renderer::DebugDrawCapture scriptGizmoCapture;
    CaptureScriptGizmos(passCtx, scriptGizmoCapture);

    /// @note ここに残すのはシーン深度で遮蔽させる線だけ (グリッド・NavMesh の面・深度ありのギズモ)。
    /// @note 深度なしの診断線はトーンマップ後へ回す (下の «デバッグ線» 参照)。
    pipeline.AddPass<GridDebugPass>();
    pipeline.AddPass<NavMeshDebugPass>();
    pipeline.AddPass<ScriptGizmoPass>("HDR", "ScriptGizmosDepth",
                                      renderer::DebugDrawLayer::DepthTested, scriptGizmoCapture);

    appendQueuedUserPasses(UserRenderPassInjectionPoint::BeforePostProcess);

    /// @name モーションベクター
    /// @note TAA とモーションブラーは深度再投影だけでは「カメラの動き」しか復元できない。
    /// @note 不透明ジオメトリの実際の移動量を専用 RT へ描いて両者へ供給する。
    /// @note 消費側が 1 つも無いフレームは丸ごと省く (不透明をもう一度ラスタライズするため)。
    const bool velocityNeeded =
        velocityRT.IsValid() && (rs.motionBlur.enabled || rs.IsTaaActive());
    if (velocityNeeded) {
        pipeline.AddPass<VelocityPass>();
    }

    /// @name PostProcess チェーン
    /// @note MotionBlur CS — HDR 空間で計算し motionBlurResult へ書く (Composite が hdrRT の代わりに読む)。
    /// @note Bloom の前に走らせるので blur 後の輝度が Bloom に乗る。
    if (rs.motionBlur.enabled) {
        /// @note Velocity は誰も書かないフレームがある。書かれないものを読むと申告した瞬間に
        /// @note «producer が居ない» で Plan が落ちるので、要るときだけ足す。
        std::vector<RA> motionBlurAccesses = { { "MotionBlurResult", RU::Write }, { "HDR", RU::ReadWrite } };
        if (velocityNeeded) motionBlurAccesses.push_back({ "Velocity", RU::Read });
        pipeline.AddRawPass("MotionBlur", std::move(motionBlurAccesses), [&]() {
            ExecuteMotionBlurPass(passCtx);
        });
    }
    /// @note LensFlare PS — 輝度抽出した光源を ADDITIVE で HDR へ合成する。
    /// @note Bloom の前に置くのでフレアも Bloom に乗るが、その順序では bloomHalf に今フレームの
    /// @note 輝点がまだ無い。パス自身が bloomHalf へ焼いてから読む (LensFlareSource がこの出力)。
    pipeline.AddPass<LensFlarePass>();
    pipeline.AddPass<BloomPass>();

    /// @note フロクセル霧。シャドウマップを読むので Shadow より後、Composite より前。
    /// @note 霧はライトとシャドウだけから作るので HDR の完成を待つ必要はない。
    /// @note 無効でも積むのは、パス側が b13 へ「無効」を書き戻さないと前フレームの定数が残り
    /// @note 画面が真っ黒になるため (FroxelFogPass 参照)。
    /// @note 出力先の 3D ボリュームは論理リソースに乗らないので、writes が空でもカリングさせない。

    const bool customPostProcessEnabled =
        !customPostProcessIndices.empty() &&
        customPostProcessRT[0].IsValid() &&
        customPostProcessRT[1].IsValid();
    /// @note hasPostCompositeEffects: Composite の出力先が "LDR" かチェーン終端かを決める。
    /// @note このフラグが true なら Composite は ldrRT に書き、後続エフェクトがチェーンを形成する。
    const bool hasPostCompositeEffects =
        rs.IsTaaActive() || customPostProcessEnabled || selectionOutlineEnabled || rs.postProcess.fxaaEnabled;

    /// @note ここが «Composite がどこへ書くか» の唯一の正本。グラフへの申告 (下の writes) と
    /// @note パスが実際に束縛するハンドルを、同じ 1 つの判定から配る。
    passCtx.compositeOutputRT = hasPostCompositeEffects ? ldrRT : passCtx.chainOutputRT;

    /// @note LDR チェーンの終端リソース。passCtx.chainOutputRT の «グラフ側の名前» で、
    /// @note 実寸へ引き伸ばすのは UpscalePass だけという対応を保つ。
    const char* const chainOutRes = upscaleActive ? "UpscaleSrc" : "Output";

    {
        /// @note Bloom を読むのは bloom.enabled のときだけ (CompositePass の bloomWritten と同条件)。
        std::vector<RA> compositeAccesses = {
            { "HDR", RU::Read },
            { hasPostCompositeEffects ? "LDR" : chainOutRes, RU::Write },
        };
        if (rs.postProcess.bloom.enabled) compositeAccesses.push_back({ "Bloom", RU::Read });
        pipeline.AddRawPass("Composite", std::move(compositeAccesses), [&]() {
            ExecuteCompositePass(passCtx);
        });
    }

    /// @name Post-composite チェーン
    /// @note ppCurrent は「LDR 空間の最新フレームを持つリソース名」。これを進めるだけで
    /// @note TAA/CustomPP/SelectionOutline/FXAA の任意の組み合わせが 1 本の直列チェーンになる。
    std::string ppCurrent  = hasPostCompositeEffects ? "LDR" : chainOutRes;
    /// @note customPostProcessRT の ping-pong インデックス
    int         ppPingPong = 0;

    /// @note TAA — 最初に適用することで後続の CustomPP/SelectionOutline が TAA 済み映像に乗る。
    /// @note 登録順が RenderGraph のタイブレークになる (Kahn's algorithm)。
    if (rs.IsTaaActive()) {
        const auto taaBody = [&]() {
            ExecuteTAAPass(passCtx);
            viewTargets.taaHistoryValid = true;
            /// @note taaFlip は ExecuteTAAPass 内で反転済み — 反転後のフラグで「書いた方」を特定する。
            auto& taaOut = passHandles.taaFlip ? passHandles.taaHistoryB : passHandles.taaHistoryA;
            passHandles.fxaaInput = resources.GetColorTexture(taaOut, 0);
            /// @note TAA 後は履歴バッファが最新フレーム。更新しないと後続が TAA 前の ldrRT を読む。
            passHandles.postProcessInput = passHandles.fxaaInput;
            /// @note LDR の論理的な最新世代は履歴 RT に移る。診断も後続パスも同じ実体を引く。
            passCtx.resourceRegistry.BindTarget("LDR", taaOut);
        };
        /// @note MotionBlur と同じ理由で Velocity は要るときだけ足す。
        /// @note HDR は深度 (t7) を読むための申告。速度の無い画素の再投影に使う。
        std::vector<RA> taaAccesses = { { ppCurrent, RU::ReadWrite }, { "HDR", RU::Read } };
        if (velocityNeeded) taaAccesses.push_back({ "Velocity", RU::Read });
        pipeline.AddRawPass("TAA", std::move(taaAccesses), taaBody);
    }

    /// @note Custom PostProcess チェーン
    for (uint32_t i = 0; i < static_cast<uint32_t>(customPostProcessIndices.size()); ++i) {
        const uint32_t customIndex  = customPostProcessIndices[i];
        const bool     isLastEffect = (i + 1 == static_cast<uint32_t>(customPostProcessIndices.size()))
                                       && !selectionOutlineEnabled
                                       && !rs.postProcess.fxaaEnabled;
        /// @note outputIndex == 2 → ExecuteCustomPostProcessPass が ctx.outputRT に直書きする規約
        const uint32_t    outputIndex = isLastEffect ? 2u : static_cast<uint32_t>(ppPingPong % 2);
        const std::string outRes      = isLastEffect
            ? chainOutRes
            : ("CustomPostProcess" + std::to_string(outputIndex));
        const auto customBody = [&, customIndex, outputIndex]() {
            ExecuteCustomPostProcessPass(passCtx, customIndex, outputIndex);
        };
        /// @note 輪郭マスクはユーザーシェーダーの入力になりうる。読むと宣言しないと、
        /// @note グラフはマスクを描く前にこのパスを走らせてよいことになる (reads が
        /// @note initializer_list なので MotionBlur と同じく 2 通りに分ける)。
        if (objectMaskEnabled) {
            pipeline.AddRawPass(
                "CustomPostProcess" + std::to_string(i),
                { ppCurrent, "ObjectMask" }, { outRes }, customBody);
        } else {
            pipeline.AddRawPass(
                "CustomPostProcess" + std::to_string(i),
                { ppCurrent }, { outRes }, customBody);
        }
        ppCurrent = outRes;
        ++ppPingPong;
    }

    /// @note SelectionOutline
    if (selectionOutlineEnabled) {
        const bool        isLastEffect = !rs.postProcess.fxaaEnabled;
        const std::string outRes       = isLastEffect ? chainOutRes : "Outline";
        /// @note HDR は輪郭の深度比較が読む (t7)。
        pipeline.AddRawPass("SelectionOutline",
            { ppCurrent, "SelectionMask", "HDR" },
            { outRes },
            [&](PassResources& res) { ExecuteSelectionOutlinePass(res, passCtx); });
        ppCurrent = outRes;
    }

    /// @note FXAA
    if (rs.postProcess.fxaaEnabled) {
        pipeline.AddRawPass("FXAA", { ppCurrent }, { chainOutRes }, [&]() { ExecuteFxaaPass(passCtx); });
        ppCurrent = chainOutRes;
    }

    /// @note TAA_Blit — TAA は ping-pong 履歴にしか書かないので、後続エフェクトが 1 つも無いときは
    /// @note ppCurrent が "LDR" のまま残る。ここでチェーン終端へ届ける。
    if (ppCurrent != chainOutRes) {
        pipeline.AddRawPass("TAA_Blit", { ppCurrent }, { chainOutRes }, [&]() {
            ExecuteTAABlitPass(passCtx);
        });
        ppCurrent = chainOutRes;
    }

    /// @note デバッグ線 (深度なし) は LDR チェーンの終端へ、Upscale より前に内部解像度で重ねる。
    /// @note HDR に描くと MotionBlur / Bloom / LensFlare / 露出 / トーンマップ / TAA で線の色と形が変わる。
    /// @note 終端 RT はシーン深度を持たないので、深度テストが要る線は上の HDR 段に残してある。
    /// @note 同じ描き先を ReadWrite するので登録順がそのまま描画順。コライダーは破線で最後に描き、
    /// @note 同じ形のスクリプト Gizmo (実線) と重なっても両方読めるようにする。
    pipeline.AddPass<ConstraintDebugPass>(chainOutRes);
    pipeline.AddPass<RagdollDebugPass>(chainOutRes);
    pipeline.AddPass<RigidBodyDebugPass>(chainOutRes);
    pipeline.AddPass<AnimatorDebugPass>(chainOutRes);
    pipeline.AddPass<IKDebugPass>(chainOutRes);
    pipeline.AddPass<SpringBoneDebugPass>(chainOutRes);
    pipeline.AddPass<AttachmentDebugPass>(chainOutRes);
    pipeline.AddPass<LightRangeDebugPass>(chainOutRes);
    pipeline.AddPass<VFXGizmoDebugPass>(chainOutRes);
    pipeline.AddPass<FlowFieldDebugPass>(chainOutRes);
    pipeline.AddPass<FlowSampleDebugPass>(chainOutRes);
    pipeline.AddPass<PhysicsVolumeDebugPass>(chainOutRes);
    pipeline.AddPass<WaterFlowDebugPass>(chainOutRes);
    pipeline.AddPass<VFXPathDebugPass>(chainOutRes);
    pipeline.AddPass<BoundsDebugPass>(chainOutRes);
    pipeline.AddPass<TerrainCollisionDebugPass>(chainOutRes);
    pipeline.AddPass<DecalDebugPass>(chainOutRes);
    pipeline.AddPass<ScriptGizmoPass>(chainOutRes, "ScriptGizmos",
                                      renderer::DebugDrawLayer::Overlay, scriptGizmoCapture);
    pipeline.AddPass<DebugCollidersPass>(chainOutRes);

    /// @note Upscale — 内部解像度で仕上がった絵を出力先の実寸へ解像する。
    /// @note UI より «前» に置くのが要点。後ろに回すと UI まで引き伸ばされて滲む。
    if (upscaleActive) {
        pipeline.AddRawPass("Upscale", { ppCurrent }, { "Output" }, [&]() {
            ExecuteUpscalePass(passCtx);
        });
    }

    if (uiOptions && uiOptions->enabled && uiOptions->context) {
        pipeline.AddRawPass(
            "UIPass",
            { { "Output", renderer::RenderGraph::ResourceUsage::ReadWrite } },
            [&]() { ExecuteUIPass(passCtx); });
    }
    profiler::Profiler::EndSample();

    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ScriptPreRender");
        for (EntityID id : scene.GetEntities<ScriptComponent>()) {
            auto* sc = scene.GetComponent<ScriptComponent>(id);
            auto* go = scene.GetGameObject(id);
            if (!sc || !go || !go->activeInHierarchy())
                continue;
            for (auto& entry : sc->scripts) {
                if (!entry.script || !entry.script->enabled)
                    continue;
                entry.script->SetContext(&scene, go);
                entry.script->ExecuteCallback(&Script::OnPreRender, "OnPreRender");
            }
        }
    }

    /// @note RenderPipeline 実行 + デバッグスナップショット更新

    /// @note GPU Timestamp Query の前フレーム結果を収集してからフレームを開始する。
    /// @note GpuProfCollect を先に呼ぶことで前フレームの非同期クエリが確定している可能性を最大化する。
    {
        FBZZ_PROFILE_SCOPE("RenderSystem::GpuProfilerSetup");
        renderer.GpuProfCollect();
        renderer.GpuProfBeginFrame();

        /// @note GPU フックを RenderPipeline に設定する。CPU フックとは独立しているため、
        /// @note Profiler の CPU スコープ計測と干渉しない。
        pipeline.SetGpuProfilerHooks(
            [&](std::string_view name) { renderer.GpuProfBeginPass(name.data()); },
            [&](std::string_view name) { renderer.GpuProfEndPass(name.data()); }
        );
    }

    const bool graphExecuted = pipeline.Execute(passCtx, capture);

    renderer.GpuProfEndFrame();
    if (capture)
        capture->Finish(pipeline.LastReport(), renderer.GpuProfGetResults());
    assert(graphExecuted);
    (void)graphExecuted;
    /// @note パスが書き換えたフレームをまたぐ状態をビューへ戻す。
    /// @note TAA は反転させた向き (次フレームは «書いた方» を履歴として読む)、
    /// @note 自動露出は消費したリセット世代。
    viewTargets.taaFlip                 = passHandles.taaFlip;
    viewTargets.exposureResetGeneration = passHandles.exposureResetGeneration;

    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ScriptPostRender");
        for (EntityID id : scene.GetEntities<ScriptComponent>()) {
            auto* sc = scene.GetComponent<ScriptComponent>(id);
            auto* go = scene.GetGameObject(id);
            if (!sc || !go || !go->activeInHierarchy())
                continue;
            for (auto& entry : sc->scripts) {
                if (!entry.script || !entry.script->enabled)
                    continue;
                entry.script->SetContext(&scene, go);
                entry.script->ExecuteCallback(&Script::OnPostRender, "OnPostRender");
            }
        }
    }

    {
        FBZZ_PROFILE_SCOPE("RenderSystem::DebugSnapshot");
        renderer::RenderDebugOverlay::Snapshot dbgSnap;
        dbgSnap.hdrRT           = hdrRT;
        dbgSnap.ldrRT           = ldrRT;
        dbgSnap.selectionMaskRT = selectionMaskRT;
        dbgSnap.outlineRT       = outlineRT;
        dbgSnap.gbufferRT       = gbufferRT;
        dbgSnap.width           = sHdrW;
        dbgSnap.height          = sHdrH;
        dbgSnap.planDescription = pipeline.LastPlanDescription();
        for (const auto& profile : pipeline.LastReport().profiles)
            dbgSnap.passTimings.push_back({ profile.name, profile.cpuMilliseconds });

        /// @note GPU 計測結果を Snapshot に詰める。QUERY_LATENCY フレーム以内は空になる。
        for (const auto& gp : renderer.GpuProfGetResults())
            dbgSnap.gpuPassTimings.push_back({ gp.name, gp.gpuMs });

        /// @note カリング統計を Snapshot に詰める
        dbgSnap.renderStats.totalObjects    = passCtx.statsTotalObjects;
        dbgSnap.renderStats.frustumCulled   = passCtx.statsFrustumCulled;
        dbgSnap.renderStats.occlusionCulled = passCtx.statsOcclusionCulled;
        dbgSnap.renderStats.distanceCulled    = passCtx.statsDistanceCulled;
        dbgSnap.renderStats.smallObjectCulled = passCtx.statsSmallObjectCulled;
        dbgSnap.renderStats.drawCalls       = passCtx.statsDrawCalls;
        dbgSnap.renderStats.vertexCount     = passCtx.statsVertexCount;
        dbgSnap.renderStats.triangleCount   = passCtx.statsTriangleCount;
        dbgSnap.renderStats.skinningVertexCount = passCtx.statsSkinningVertexCount;
        dbgSnap.renderStats.skinningDispatchCount = passCtx.statsSkinningDispatchCount;
        dbgSnap.renderStats.shadowDrawCalls     = passCtx.statsShadowDrawCalls;
        dbgSnap.renderStats.shadowTriangleCount = passCtx.statsShadowTriangleCount;
        dbgSnap.renderStats.instancedBatches    = passCtx.statsInstancedBatches;
        dbgSnap.renderStats.instancedDrawsSaved = passCtx.statsInstancedDrawsSaved;

        renderer::RenderDebugOverlay::UpdateSnapshot(dbgSnap, rs.passViewerEnabled);
    }
}

} // namespace fbzz::scene
