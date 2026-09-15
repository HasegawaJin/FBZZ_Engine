/// @file    RenderSystem.cpp
/// @brief   Scene から DrawCall を生成するオーケストレーター。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// 各描画パスの実装は RenderPasses/ 以下の Execute*Pass 関数に委譲する。
#include "Engine/Scene/Systems/RenderSystem.hpp"
// ResolveGameCullingSettings — 呼び出し側が明示しなかったときのフォールバック解決に使う。
#include "Engine/Scene/SceneUtils.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/TerrainRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/WaterRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/MeshTrailRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/TrailRenderPass.hpp"
#include "Engine/Renderer/RenderSettings.hpp"
#include "Engine/Renderer/RenderDebugOverlay.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassCapture.hpp>
#include "Engine/Renderer/DebugDraw.hpp"
#include "RenderPasses/Debug/DebugPasses.hpp"
#include <Physics/World.hpp>
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include "RenderPasses/Geometry/ParticleEmitterSpace.hpp"
#include "Engine/Scene/Components/ParticleEmitter.hpp"
#include "Engine/Scene/Components/ParticleGpuSimulation.hpp"
#include "Engine/Scene/Components/ParticleLightSelection.hpp"
#include "RenderPasses/PostProcess/PostProcessPasses.hpp"
#include "RenderPasses/PostProcess/CloudNoiseBake.hpp"
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

// RendererがサンプルするLDR RGB座標と同じ順序で32^3 RGBA8 LUTを生成する。
// WHAT: x=R, y=G, z=B、xが最速で並ぶD3D11 Texture3Dのメモリ配置にする。
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

// Viewport ごとに解像度依存の中間リソースを保持する。
// static で共有すると SceneView と GameView が 1 フレーム内でリサイズし合う。
struct ViewRenderTargets {
    // View ごとの RenderGraph 計画と transient RT をフレーム間で保持する。
    // スタック生成だと DX12 の descriptor heap を毎フレーム作り直し、CPU が詰まる。
    RenderPipeline pipeline;
    renderer::ResourceHandle<renderer::RenderTargetTag> hdr;
    renderer::ResourceHandle<renderer::RenderTargetTag> ldr;
    renderer::ResourceHandle<renderer::RenderTargetTag> selectionMask;
    renderer::ResourceHandle<renderer::RenderTargetTag> outline;
    // ランタイム輪郭のシルエット (RGB=色 / A=太さ)。エディタ選択のマスクとは別物。
    renderer::ResourceHandle<renderer::RenderTargetTag> objectMask;
    renderer::ResourceHandle<renderer::RenderTargetTag> customPostProcess[2];
    // ポストプロセスチェーンの終着点。描画スケールが等倍でないフレームだけ持ち、
    // UpscalePass がここから出力先の実寸へ解像する。等倍なら確保しない。
    renderer::ResourceHandle<renderer::RenderTargetTag> upscaleSrc;
    renderer::ResourceHandle<renderer::RenderTargetTag> gbuffer;
    // モーションベクター (RG=速度, B=書き込み済みフラグ)。TAA / MotionBlur が有効な
    // フレームだけ描く。深度は自前で持つので、本描画の深度バッファとは共有しない。
    renderer::ResourceHandle<renderer::RenderTargetTag> velocity;
    renderer::ResourceHandle<renderer::RenderTargetTag> decalDepth;
    renderer::ResourceHandle<renderer::RenderTargetTag> decalMask;
    // Bloom のミップ連鎖。bloomChain[0] が半解像度で、以降 1/2 ずつ。
    // bloomHalf は bloomChain[0] の別名 (Composite 側の参照名)。
    // 1 枚のミップ付きにしないのは CreateComputeTexture がミップを持たないため。
    renderer::ResourceHandle<renderer::TextureTag> bloomChain[kBloomMipCount];
    renderer::ResourceHandle<renderer::TextureTag> bloomUpChain[kBloomMipCount];
    renderer::ResourceHandle<renderer::TextureTag> bloomHalf;
    renderer::ResourceHandle<renderer::TextureTag> bloomFull;
    renderer::ResourceHandle<renderer::TextureTag> ssaoRaw;
    renderer::ResourceHandle<renderer::TextureTag> ssaoBlur;
    // ---- Advanced Graphics (解像度依存・ビュー単位) ----
    // 解像度非依存な static リソース (BRDF LUT 等) は別途 static 変数が持つ。
    renderer::ResourceHandle<renderer::TextureTag>        ssrResult;           // SSR CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        volumetricResult;    // Volumetric CS 出力
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryA;         // TAA ping-pong A
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryB;         // TAA ping-pong B
    // TAA の ping-pong の向き。RenderPassHandles はフレームごとに作り直すので、
    // ここに持たないと毎フレーム false から始まり «A を読んで B に書く» しか起きない。
    // A は一度も書かれず、履歴は最初の中身のまま固定される。
    bool taaFlip = false;
    renderer::ResourceHandle<renderer::TextureTag>        motionBlurResult;    // Motion Blur CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        gtaoRaw;             // GTAO RAW CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        gtaoBlur;            // GTAO Blur CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        contactShadowResult; // Contact Shadow CS 出力
    // ---- ビュー別定数バッファ / 再投影行列 ----
    // static で共有すると SceneView と GameView が互いのカメラ行列を引き、
    // MotionBlur / TAA の再投影が常に壊れる。
    renderer::ResourceHandle<renderer::ConstantBufferTag> advancedGraphicsCB;
    // ---- 自動露出 (ビュー単位・解像度非依存) ----
    // exposureResult は「順応済みの平均輝度」でフレームをまたぐ状態。static で共有すると
    // SceneView と GameView が交互に順応を進め、互いの明るさへ引きずられて露出が振れる。
    renderer::ResourceHandle<renderer::StructuredBufferTag> exposureHistogram;
    renderer::ResourceHandle<renderer::StructuredBufferTag> exposureResult;
    uint32_t exposureResetGeneration = 0;
    // ---- 体積雲の作業 RT (ビュー単位・解像度依存) ----
    renderer::SizedRenderTarget cloudRT;
    renderer::SizedRenderTarget cloudDepthRT;
    // ---- 水面の屈折用コピー (ビュー単位・解像度依存) ----
    renderer::SizedRenderTarget waterSceneColorRT;
    renderer::SizedRenderTarget waterSceneDepthRT;
    // ---- 歪みパーティクルの背景退避 / 重なり計数 / コースティクスの深度コピー ----
    // 水面と同じくビュー単位。共有すると 2 ビューで寸法を取り合い、毎フレーム作り直す。
    renderer::SizedRenderTarget particleSceneColorRT;
    renderer::SizedRenderTarget particleOverdrawRT;
    renderer::SizedRenderTarget particleReactiveRT;
    renderer::SizedRenderTarget causticsDepthRT;
    // ---- フロクセル霧 (ビュー単位・解像度非依存) ----
    // グリッドは視錐台に貼り付くので、共有すると互いの履歴を上書きして霧が明滅する。
    // 寸法は設定値 (既定 160x90x64) で画面サイズと無関係なので、リサイズでは作り直さない。
    renderer::ResourceHandle<renderer::TextureTag> froxelScatter;
    renderer::ResourceHandle<renderer::TextureTag> froxelScatterHistory;
    renderer::ResourceHandle<renderer::TextureTag> froxelIntegrated;
    uint32_t                     froxelGrid[3] = { 0u, 0u, 0u };
    FroxelFogViewState           froxelState;
    math::Matrix4 prevViewProjection    = math::Matrix4::Identity();
    math::Matrix4 invPrevViewProjection = math::Matrix4::Identity();
    // TAA ジッター列の現在位置。ビュー別に持たないと SceneView と GameView が
    // 同じ番号を取り合って、どちらもサンプル点が飛び飛びになる。
    uint32_t taaFrameIndex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Halton 列 (基数 base) の index 番目。
// TAA には少ない枚数でも偏らない散り方が要る。乱数だと数フレームでは固まる。
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

// Resize 前のネイティブリソースを ResourceManager から確実に解放する。
void ReleaseViewRenderTargets(ViewRenderTargets& targets, renderer::ResourceManager& resources)
{
    // transient RT も同じ Viewport 寿命に属するため、固定 RT より先に明示解放する。
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
    // bloomHalf は bloomChain[0] の別名なので、ここでは解放しない (二重解放になる)。
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
    // どちらも解像度非依存。作り直すと prevVP が Identity へ戻り、
    // MotionBlur / TAA が 1 フレーム乱れるので、保存して復元する。
    auto savedCB         = targets.advancedGraphicsCB;
    auto savedPrevVP     = targets.prevViewProjection;
    auto savedPrevInvVP  = targets.invPrevViewProjection;
    auto savedTaaIndex   = targets.taaFrameIndex;
    // フロクセル霧のボリュームは解像度非依存なので、リサイズで作り直さない。
    // 作り直すと 14MB の確保が走ってフレームが飛ぶうえ、履歴が切れて霧が 1 度暗転する。
    auto savedFroxelA     = targets.froxelScatter;
    auto savedFroxelB     = targets.froxelScatterHistory;
    auto savedFroxelInt   = targets.froxelIntegrated;
    const uint32_t savedFroxelGrid[3] = { targets.froxelGrid[0], targets.froxelGrid[1],
                                          targets.froxelGrid[2] };
    auto savedFroxelState = targets.froxelState;
    // 露出の順応も解像度非依存。リサイズで捨てると画面が一瞬白飛び / 黒潰れする。
    auto savedExposureHistogram = targets.exposureHistogram;
    auto savedExposureResult    = targets.exposureResult;
    const uint32_t savedExposureGeneration = targets.exposureResetGeneration;
    // 雲の作業 RT は解像度依存。`targets = {}` で握ったまま忘れると漏れるので先に返す。
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

// 視錐台スライス [nearZ, farZ] の外接球。centerDistance はカメラ前方への距離。
// 8 頂点へ合わせるとカメラの回転で箱の大きさが変わり、影の精細度が脈動する。
// 外接球の半径は向きに依存しない。
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

    // 平行投影は錐台ではなく直方体なので、対角の傾きという概念が無い。
    // スライスの外接球は「中央 + 半対角」でそのまま求まる。
    if (camera.m_projection == renderer::ProjectionMode::Orthographic) {
        const float halfH = (std::max)(camera.m_orthoHeight, 0.01f) * 0.5f;
        const float halfW = halfH * camera.m_aspect;
        const float halfD = (farZ - nearZ) * 0.5f;
        FrustumSliceSphere box;
        box.centerDistance = (nearZ + farZ) * 0.5f;
        box.radius = std::sqrt(halfW * halfW + halfH * halfH + halfD * halfD);
        return box;
    }

    // 視錐台の対角方向の傾き。k = |(±aspect*t, ±t, 1)| の xy 成分の長さ。
    const float tanHalfFov = std::tan(camera.m_fovY * 0.5f * DEG_TO_RAD);
    const float k  = tanHalfFov * std::sqrt(1.0f + camera.m_aspect * camera.m_aspect);
    const float k2 = k * k;

    FrustumSliceSphere sphere;
    // near 面が far 面より広いほど中心は手前へ寄る。k² が十分大きいときは
    // far 面の外接円がスライス全体を包むので、中心は far 面上に載る。
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

// practical split scheme。対数分割 (手前を細かく) と等分割 (遠方を細かく) を lambda で補間する。
// 対数だけだと遠景の影が溶け、等分だけだと足元が粗くなる。
// outSplits[i] は「カスケード i が担当する far 距離」。outSplits[count-1] == shadowDistance。
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
    // 丸め誤差で最遠が shadowDistance を下回ると、影の到達距離が設定より短くなる。
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

    // カリング挙動は「明示指定 > シーンのメインカメラ > 既定値」の順で解決する。
    // シーンから引くのは、RenderSystem を素で呼ぶ Standalone でも CameraComponent の設定を効かせるため。
    const CameraCullingSettings resolvedCulling =
        cullingSettings ? *cullingSettings : ResolveGameCullingSettings(scene);

    // VFXCameraShake — VFX グラフの Camera Shake ノードによる揺れ。
    // カメラ本体を書き換えると DebugCamera の yaw/pitch と乖離して操作が壊れる。
    // 描画用のコピーだけをずらす。カリングも揺れた視点で行うので画面端の不整合も出ない。
    renderer::Camera shakenCamera = inputCamera;
    {
        math::Vector3 offset = math::Vector3::ZERO;
        float rollDegrees = 0.0f;
        // 方向性のキックだけはワールド空間で積む。«どちらから押されたか» が本体なので、
        // カメラのローカル軸へ畳むと向きの情報が消える。
        math::Vector3 worldKick = math::Vector3::ZERO;
        for (EntityID id : scene.GetEntities<VFXCameraShake>()) {
            GameObject* go       = scene.GetGameObject(id);
            auto*       shakePtr = scene.GetComponent<VFXCameraShake>(id);
            if (!go || !shakePtr || !go->activeInHierarchy()) continue;
            const VFXCameraShake& shake = *shakePtr;
            const float weight = shake.enabled ? std::clamp(shake.weight, 0.0f, 1.0f) : 0.0f;
            if (weight <= 0.0f) continue;
            // 発生源から遠いほど弱める。radius <= 0 は距離減衰なし。
            float distanceScale = 1.0f;
            if (shake.radius > 0.0f) {
                const float distance = (go->transform.worldPosition - inputCamera.m_position).Length();
                distanceScale = std::clamp(1.0f - distance / shake.radius, 0.0f, 1.0f);
            }
            const float amount = weight * distanceScale;
            if (amount <= 0.0f) continue;
            // 軸ごとに位相をずらした正弦の合成。決定論的で、フレームレートに依存しない。
            const float phase = shake.elapsed * shake.frequency;
            offset.x += std::sin(phase * 1.00f) * shake.amplitude * amount;
            offset.y += std::sin(phase * 1.37f + 1.7f) * shake.amplitude * amount;
            offset.z += std::sin(phase * 0.83f + 3.1f) * shake.amplitude * amount * 0.5f;
            rollDegrees += std::sin(phase * 1.11f + 0.6f) * shake.rotationAmplitude * amount;

            // 方向性のキック。«発生源から見て押しのけられる» 向きへ 1 回だけ動かす。
            if (shake.kick != 0.0f) {
                math::Vector3 direction = shake.kickDirection;
                if (direction.LengthSq() <= 0.0001f)
                    direction = inputCamera.m_position - go->transform.worldPosition;
                // 発生源とカメラが同じ点にあるときは «押す向き» が決まらない。
                // 揺れだけを残し、キックは捨てる (前方へ倒すと爆心で毎回同じ癖が出る)。
                worldKick += direction.NormalizedOr(math::Vector3::ZERO) * (shake.kick * amount);
            }
        }
        if (offset.LengthSq() > 0.0f || worldKick.LengthSq() > 0.0f || rollDegrees != 0.0f) {
            constexpr float DEG_TO_RAD = 0.01745329251994329577f;
            // オフセットはカメラのローカル軸で与え、向きに依らず自然に揺れるようにする。
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

    // ── ルック設定の解決 ───────────────────────────────────────────
    // ベースは VolumeSettings の既定値で、ボリュームが唯一の供給源
    // (ProjectSettings からポストプロセスと高度グラフィクスの公開は撤去済み)。
    // 隠れた全体ベースを持つと、同じプロファイルが別プロジェクトで違う絵になる。
    renderer::VolumeSettings volumeSettings;
    if (const auto* runtimePostProcess = scene.TryGetRuntimePostProcessSettings())
        volumeSettings.post = *runtimePostProcess;

    // ── コンポーネントによる設定上書き (ProjectSettings < runtimePostProcess < Component) ───
    // View<> を使わないのは GameObject が取れず activeInHierarchy() を見られないため。
    // enabled (コンポーネントを切る) と activeInHierarchy() (オブジェクトごと切る) の
    // どちらでも絵から消える必要がある。
    //
    // EnvironmentLightComponent — シーン Inspector から IBL を上書きする。先着優先。
    IblSource activeIblSource = IblSource::StaticDDS; // 空連動 IBL: 採用された EnvironmentLight の source
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
    // AtmosphericScatteringComponent — シーン Inspector から霧設定を上書きする。
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
    // ── PostProcessVolumeComponent の合成 ────────────────────────────────────
    // ベースの上に、有効なボリュームを priority 昇順で重み付きブレンドする。
    // 走査順に任せると GameObject を作り直しただけで重なり順が変わる。
    {
        struct VolumeEntry {
            const PostProcessVolumeComponent* volume  = nullptr;
            const asset::PostProcessProfile*  profile = nullptr;
            float weight = 0.0f;
        };
        std::vector<VolumeEntry> entries;

        // 距離判定はシェイク適用後のカメラ位置で行う。
        // シェイク量は influenceRadius に対して無視できるので、視点を使い分けない。
        const math::Vector3 viewPosition = camera.m_position;

        for (EntityID id : scene.GetEntities<PostProcessVolumeComponent>()) {
            GameObject* go     = scene.GetGameObject(id);
            auto*       ppvPtr = scene.GetComponent<PostProcessVolumeComponent>(id);
            if (!go || !ppvPtr || !go->activeInHierarchy() || !ppvPtr->enabled) continue;
            const PostProcessVolumeComponent& ppv = *ppvPtr;

            // プロファイル未アサイン / 参照切れのボリュームは何も適用しない。
            // 既定値へ倒すと「素のルック」を主張して priority 次第で他を打ち消す。
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

        // priority 昇順。同値は安定ソートで走査順を保つ (再現性のため)。
        std::stable_sort(entries.begin(), entries.end(),
            [](const VolumeEntry& lhs, const VolumeEntry& rhs) {
                return lhs.volume->priority < rhs.volume->priority;
            });

        for (const VolumeEntry& entry : entries) {
            // 持っているオーバーライドだけが混ざり、載っていない効果は素通し。
            // 「洞窟プロファイルは Fog と Color Grading だけ持つ」差分オーサリングが成立する。
            entry.profile->ApplyTo(volumeSettings, entry.weight);
        }

        // 以降のコードは effectiveSettings.postProcess や .ssr をフラットに読む。
        renderer::ApplyVolumeSettings(volumeSettings, effectiveSettings);
    }
    // VFXScreenEffect — VFX グラフの ScreenEffect ノードが出す一時的な画面演出。
    // PostProcessVolume は「設定の差し替え」なので一瞬の上乗せや同時発生を表現できない。
    // 解決済み設定へ後段で加算する。フラッシュだけは飽和するので最大値を採る。
    {
        renderer::PostProcessSettings& pp = effectiveSettings.postProcess;
        float strongestFlash = 0.0f;
        // 輪だけは «一番強い 1 枚» を採る。中心と半径を足すと、2 つの爆発が
        // «画面のどこにも無い中心を持つ 1 つの輪» に化ける。
        float strongestRing = 0.0f;
        // 露出・色は «押し» の合計。掛け算ではなく加算なので、同時に走った演出は
        // それぞれのぶんだけ深くなる (0 が «素» になる設計)。
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
            // 放射ブラーは «有効フラグ» を持たない。0 のときシェーダー側が
            // 早期 return するので、加算するだけで «掛かっていない» が成立する。
            if (effect.radialBlur > 0.0f)
                pp.lens.radialBlur += effect.radialBlur * weight;
            // 輪は «進捗» で外へ走る。progress=0 (窓の頭 / 配り手が居ない) では
            // 半径 0 の点になってしまうので、そのフレームは掛けない。
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
            // 切ってあったグレーディングを «押し» のために点けるときは、必ず素の値から
            // 始める。プロファイルが書いた値がぶら下がったまま有効になると、
            // 止めの一瞬だけ «誰も指示していない色» へ飛ぶ。
            if (!pp.colorGrading.enabled) {
                pp.colorGrading = renderer::ColorGradingSettings{};
                pp.colorGrading.enabled = true;
            }
            pp.colorGrading.saturation = (std::max)(pp.colorGrading.saturation + saturationOffset, 0.0f);
            pp.colorGrading.contrast += contrastOffset;
        }
    }
    // NOTE: ReflectionProbeComponent は将来の局所反射ブレンド実装で使用予定。
    //       現時点は Inspector / Serializer のみ対応し、RenderSystem での適用は未実装。

    const renderer::RenderSettings& rs = effectiveSettings;

    // 静的ハンドルの検証・デバイスリセット復旧・初回バッファ生成をまとめて計測する。
    profiler::Profiler::BeginSample(
        profiler::ProfilerMarker("RenderSystem::StaticResourceSetup", "Rendering"));

    // =========================================================================
    // 静的リソースの遅延初期化
    // =========================================================================
    static uint64_t sResourceResetVersion = resources.GetResetVersion();
    // シャドウアトラスは «解像度が動く» リソース (画質プリセットとエディタの Play/Stop)。
    // 作り直しと解放は SizedRenderTarget に任せる ─ 自前で書くと、返し忘れた 1 か所が
    // そのまま «ShadowPass だけ突然重い» になる。
    static renderer::SizedRenderTarget shadowMapRT;
    // Spot / Point 用のシャドウアトラス。Directional の CSM とは面積を共有しない。
    static renderer::SizedRenderTarget punctualShadowRT;
    // ライト Cookie を敷き詰めるアトラス。寸法は固定なので作り直しは起きない。
    static renderer::SizedRenderTarget lightCookieRT;
    static auto cookieBlitShader =
        resources.LoadShader("Assets/Shaders/Pipeline/Lighting/CookieBlit.hlsl");
    static auto shadowShader         = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMap.hlsl");
    static auto skinnedShadowShader  = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/SkinnedShadowMap.hlsl");
    static auto velocityShader        = resources.LoadShader("Assets/Shaders/Motion/Velocity.hlsl");
    static auto velocitySkinnedShader = resources.LoadShader("Assets/Shaders/Motion/VelocitySkinned.hlsl");
    // コンピュートスキニング。無効ならスキンド描画は従来の VS スキニング経路へ落ちる。
    static auto skinningComputeCS    = resources.LoadShader("Assets/Shaders/Pipeline/Skinning/SkinningCompute.cs.hlsl");

    // スキンドメッシュに AnimatorComponent がない場合のアイデンティティボーンパレット
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
    static auto objectMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Pipeline/Mask/ObjectMaskSkinned.hlsl");
    static auto copyColorShader         = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
    static auto customComposeShader     = resources.LoadShader("Assets/Shaders/PostProcess/Custom/CustomCompose.hlsl");
    static auto fxaaShader              = resources.LoadShader("Assets/Shaders/PostProcess/AntiAliasing/FXAA.hlsl");
    static auto upscaleShader           = resources.LoadShader("Assets/Shaders/PostProcess/Upscale/Upscale.hlsl");

    // ---- Advanced Graphics シェーダー (static で初回ロード、Reset 後に再ロード) ----
    static auto iblBrdfBakeShader   = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/BRDFIntegration.cs.hlsl");
    static auto gtaoShader          = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/GTAO.cs.hlsl");
    static auto gtaoBlurShader      = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/GTAOBlur.cs.hlsl");
    static auto ssrShader           = resources.LoadShader("Assets/Shaders/PostProcess/Reflections/SSR.cs.hlsl");
    static auto volumetricShader    = resources.LoadShader("Assets/Shaders/PostProcess/Lighting/VolumetricLight.cs.hlsl");
    static auto contactShadowShader = resources.LoadShader("Assets/Shaders/PostProcess/Shadow/ContactShadows.cs.hlsl");
    static auto taaShader           = resources.LoadShader("Assets/Shaders/PostProcess/AntiAliasing/TAA.hlsl");
    static auto motionBlurShader    = resources.LoadShader("Assets/Shaders/PostProcess/Motion/MotionBlur.cs.hlsl");
    static auto lensFlareShader     = resources.LoadShader("Assets/Shaders/PostProcess/Flare/LensFlare.hlsl");
    // WHY: BRDF LUT は 512×512 の定数テーブルで、解像度・シーンが変わっても内容は変わらない。
    //      毎フレーム再生成するコストを避けるため static で一度だけ生成し、IBLBakePass でのみ書き込む。
    static auto iblBrdfLut          = resources.CreateComputeTexture(512, 512);
    static renderer::ResourceHandle<renderer::TextureTag> proceduralColorLut;
    static uint64_t proceduralColorLutHash = 0u;

    static auto skydomeShader = resources.LoadShader("Assets/Shaders/Material/Sky/Skydome.hlsl");
    static auto sunMoonShader = resources.LoadShader("Assets/Shaders/Material/Sky/SunMoon.hlsl");
    static auto skydomeMesh   = renderer::PrimitiveMesh::Sphere(resources, 32);

    // 空連動 IBL (環境システム Phase A): 空を焼くキューブマップ RT と、面ごとの view/proj 用 CB。
    // EnvironmentResources はフレームをまたいで保持し、SkyRenderer が変化した時だけ再キャプチャする。
    static constexpr uint32_t kSkyEnvCubeSize = 128;
    static EnvironmentResources sEnvironmentResources;
    static uint64_t sEnvResetVersion = resources.GetResetVersion();
    static renderer::ResourceHandle<renderer::RenderTargetTag>   skyEnvCubeRT;
    static renderer::ResourceHandle<renderer::ConstantBufferTag> skyCaptureFrameCB;
    if (!skyEnvCubeRT.IsValid() || sEnvResetVersion != resources.GetResetVersion()) {
        sEnvResetVersion  = resources.GetResetVersion();
        skyEnvCubeRT      = resources.CreateCubemapRenderTarget(kSkyEnvCubeSize, 1);
        skyCaptureFrameCB = resources.CreateConstantBuffer(sizeof(PerFrameCB));
        // リソース再生成後 (デバイスリセット等) は古い動的 IBL ハンドルが無効。クリアして焼き直す。
        sEnvironmentResources.skyEnvCube    = {};
        sEnvironmentResources.skyIrradiance = {};
        sEnvironmentResources.skyPrefilter  = {};
        sEnvironmentResources.needsConvolution = false;
        sEnvironmentResources.MarkDirty();
    }

    // ボリューメトリック雲の 3D ノイズ (Shape 128³ + Detail 32³) を起動時に 1 回だけ CPU 焼きする。
    // タイラブルなので WRAP サンプルで無限に並べられる。デバイスリセット後は SRV が無効になるため焼き直す。
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
    static auto deferredLightingShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DeferredLighting.hlsl");
    static auto depthCopyShader        = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");

    // クラスタライトカリング (Forward+ / Deferred+)。
    static auto clusterCullCS = resources.LoadShader("Assets/Shaders/Pipeline/Clustered/ClusterLightCull.cs.hlsl");
    // clusterIndexBuffer は解像度非依存の固定長なので確保は初回の 1 回だけ。
    // CS が u2 へ書き PS が t30 から読むので RW。
    // punctualLightBuffer (CPU が書いて GPU が読むだけ) は 1 枚を共有せず、描くたびに借りる。
    // WHY: DX12 の読み取り専用 StructuredBuffer は Upload ヒープへの直 memcpy。1 枚だと
    //      Scene View の Draw が Game View の書いた配列を読み、GPU がまだ読んでいる
    //      前フレームの配列も上書きする (ライトが増減したフレームにだけ幽霊が出る)。
    static renderer::DynamicStructuredBufferPool punctualLightPool;
    static auto clusterIndexBuffer = resources.CreateRWStructuredBuffer(
        nullptr, kClusterCount * kClusterStride, static_cast<uint32_t>(sizeof(uint32_t)));

    // 自動露出のバッファはビュー単位 (ViewRenderTargets) で確保する。シェーダーだけ共有。
    static auto exposureHistogramCS =
        resources.LoadShader("Assets/Shaders/PostProcess/Color/ExposureHistogram.cs.hlsl");
    static auto exposureAverageCS =
        resources.LoadShader("Assets/Shaders/PostProcess/Color/ExposureAverage.cs.hlsl");

    // フロクセル霧の CS。ボリューム本体はビュー単位なので、下の per-view ブロックで確保する。
    static auto froxelInjectCS =
        resources.LoadShader("Assets/Shaders/PostProcess/Cloud/FroxelInject.cs.hlsl");
    static auto froxelIntegrateCS =
        resources.LoadShader("Assets/Shaders/PostProcess/Cloud/FroxelIntegrate.cs.hlsl");
    static auto clusterCB = resources.CreateConstantBuffer(sizeof(ClusterConstantsCB));
    // 別視点から描くパス用に、供給モードだけ Linear へ落とした同内容の CB。
    static auto clusterLinearCB = resources.CreateConstantBuffer(sizeof(ClusterConstantsCB));

    static auto decalShader     = resources.LoadShader("Assets/Shaders/Material/Decal/Decal.hlsl");
    static auto decalMaskShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMask.hlsl");
    static auto decalMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMaskSkinned.hlsl");

    static auto particleShader      = resources.LoadShader("Assets/Shaders/Material/Effects/Particle.hlsl");
    static auto particleGpuSimCS   = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSim.cs.hlsl");
    static auto particleGpuShader  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGPU.hlsl");
    // GPU ソート 3 段 (キー生成 / グローバル段 / LDS 段)。sortMode != None のときだけ走る。
    static auto particleGpuSortKeysCS  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortKeys.cs.hlsl");
    static auto particleGpuSortStepCS  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortStep.cs.hlsl");
    static auto particleGpuSortLocalCS = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortLocal.cs.hlsl");
    static auto particleGpuMeshShader  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuMesh.hlsl");
    // 自己影: 光源から見た密度を積む。selfShadowStrength > 0 のエミッターがあるときだけ走る。
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
    // コンピュートスキニングの b0 (頂点数のみ)。16 バイト境界へ切り上げられる。
    static auto skinningCB = resources.CreateConstantBuffer(16);
    static auto postprocCB = resources.CreateConstantBuffer(sizeof(PostProcCB));
    static auto outlineCB  = resources.CreateConstantBuffer(sizeof(OutlineCB));
    static auto objectMaskCB = resources.CreateConstantBuffer(sizeof(ObjectMaskCB));
    static auto atmCB      = resources.CreateConstantBuffer(sizeof(AtmosphereCB));
    static auto decalCB    = resources.CreateConstantBuffer(sizeof(DecalCB));
    static auto decalMaterialCB = resources.CreateConstantBuffer(sizeof(DecalMaterialCB));
    static auto decalReceiverCB = resources.CreateConstantBuffer(sizeof(DecalReceiverCB));
    static auto volumetricCloudCB = resources.CreateConstantBuffer(176);
    // パーティクル自己影: 光源側の密度 RT と、光源行列を入れる専用 frame CB。
    // RenderPassHandles は毎フレーム作り直される値型なので、パス側で遅延生成すると RT を漏らす。
    // 解像度が固定なのは、拾うのが「煙の内部で光がどれだけ減るか」という低周波の情報だから。
    static renderer::SizedRenderTarget particleSelfShadowRT;
    static auto particleSelfShadowFrameCB = resources.CreateConstantBuffer(sizeof(PerFrameCB));

    // WHY: static handle は通常フレームでは再利用し、ResourceManager::Reset() 後だけ世代差分で再生成する。
    //      これによりデバイスロスト復帰時も旧ネイティブリソースへ触らない。
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
    // VolumetricCloud.hlsl は scatter.rgb に既に透過率を積分した premultiplied 値を返す。
    // Overdraw 可視化も volumetricCloudPSO を共有するため、雲の合成だけ専用 PSO に分離する。
    static auto volumetricCloudPremultipliedPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::PREMULTIPLIED,
        renderer::DepthMode::DEPTH_OFF
    });
    // ---- Advanced Graphics PSO / 定数バッファ ----
    // taaPSO: OPAQUE — TAA は ping-pong バッファへ上書きするため α ブレンドは不要
    static auto taaPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    // lensFlarePSO: ADDITIVE — ゴーストはフレアを HDR バッファに加算合成する
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

    // GPU リソースが «増え続けていないか» をフレーム単位で見張る。
    // WHY ここか: 描画は 1 フレームに 1 度だけ通り、resources を持っている。
    resources.TickLeakWatchdog();

    // シャドウアトラス。解像度は «動く» ─ 画質プリセットの適用 (GameSettings が各シーンの
    // OnStart で行う) と、エディタの Play / Stop による RenderSettings の差し替えで、
    // 1 回の試遊につき数回作り直される。1 枚で数十 MB あるので、返し忘れると数回の Play で
    // 数百 MB 漏れ «ShadowPass だけ突然重い / エディタ再起動で直る» として出る (2026-09-01)。
    // 解像度は CSM と punctual で別設定なので、それぞれ独立に作り直す。
    const uint32_t punctualShadowRes = (std::max)(rs.shadow.punctualMapResolution, 64u);
    (void)punctualShadowRT.Ensure(resources, punctualShadowRes, punctualShadowRes, 0);

    if (lightCookieRT.Ensure(resources, kLightCookieAtlasWidth, kLightCookieAtlasHeight, 1)) {
        cookieBlitShader =
            resources.LoadShader("Assets/Shaders/Pipeline/Lighting/CookieBlit.hlsl");
        // アトラスの中身は作り直しで失われる。焼き直し済みの記録も捨てる。
        ReleaseLightCookieCache();
    }

    // WHY 下のシェーダー再ロードと分けるか (2026-09-01 の修正):
    //   以前はアトラスとシェーダーが 1 つの if に同居していて、影の解像度が 1 段変わるだけで
    //   シェーダー約 40 本・定数バッファ十数個・PSO・BRDF LUT まで作り直していた。
    //   しかもどれも古いハンドルを返していないので、そのぶんが丸ごと漏れる。
    (void)shadowMapRT.Ensure(resources, rs.shadow.mapResolution, rs.shadow.mapResolution, 0);

    (void)particleSelfShadowRT.Ensure(resources, RenderPassHandles::kSelfShadowResolution,
                                      RenderPassHandles::kSelfShadowResolution, 1);

    // シェーダー / 定数バッファ / PSO の作り直し。
    //
    // WHY «リセットされたときと初回» だけか: どれもデバイスリセットで実体ごと失われる
    //   もので、返す相手はもう居ない (だから Release を書いていない)。逆に言えば、
    //   実体が生きているうちにここを通してはいけない ─ 通ったぶんがそのまま漏れる。
    static bool sStaticsLoaded = false;
    if (resourcesWereReset || !sStaticsLoaded) {
        sStaticsLoaded        = true;
        sResourceResetVersion = resources.GetResetVersion();

        shadowShader        = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMap.hlsl");
        skinnedShadowShader = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/SkinnedShadowMap.hlsl");
        velocityShader        = resources.LoadShader("Assets/Shaders/Motion/Velocity.hlsl");
        velocitySkinnedShader = resources.LoadShader("Assets/Shaders/Motion/VelocitySkinned.hlsl");
        skinningComputeCS   = resources.LoadShader("Assets/Shaders/Pipeline/Skinning/SkinningCompute.cs.hlsl");
        // Mesh* / AnimatorComponent* をキーにしたキャッシュはリソースリセットで無効になる。
        ReleaseSkinningComputeCaches();
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
        objectMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Pipeline/Mask/ObjectMaskSkinned.hlsl");
        copyColorShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
        customComposeShader = resources.LoadShader("Assets/Shaders/PostProcess/Custom/CustomCompose.hlsl");
        fxaaShader = resources.LoadShader("Assets/Shaders/PostProcess/AntiAliasing/FXAA.hlsl");
        upscaleShader = resources.LoadShader("Assets/Shaders/PostProcess/Upscale/Upscale.hlsl");
        skydomeShader = resources.LoadShader("Assets/Shaders/Material/Sky/Skydome.hlsl");
        sunMoonShader = resources.LoadShader("Assets/Shaders/Material/Sky/SunMoon.hlsl");
        skydomeMesh   = renderer::PrimitiveMesh::Sphere(resources, 32);
        gbufferShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBuffer.hlsl");
        deferredLightingShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DeferredLighting.hlsl");
        depthCopyShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
        clusterCullCS = resources.LoadShader("Assets/Shaders/Pipeline/Clustered/ClusterLightCull.cs.hlsl");
        decalShader = resources.LoadShader("Assets/Shaders/Material/Decal/Decal.hlsl");
        decalMaskShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMask.hlsl");
        decalMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMaskSkinned.hlsl");
        // .mat の解決結果はシェーダー・テクスチャ・cbuffer のハンドルを持つ。
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

        // ---- Advanced Graphics: デバイスリセット後に再ロード ----
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
        // WHY: デバイスリセット後は iblBrdfLut の内容が失われるため再生成する。
        //      IBLBakePass は焼き済み対象を世代付きハンドルで追跡し、新ハンドルを次フレームで再生成する。
        iblBrdfLut          = resources.CreateComputeTexture(512, 512);

        bindPoseSkinningCB = {};
        {
            struct BindPoseData { math::Matrix4 bones[asset::MAX_SKINNING_BONES]; };
            BindPoseData bp{};
            for (auto& m : bp.bones) m = math::Matrix4::Identity();
            bindPoseSkinningCB = resources.CreateConstantBuffer(sizeof(BindPoseData));
            resources.Update(bindPoseSkinningCB, &bp, sizeof(BindPoseData));
        }
        // モデル側のリファレンスポーズ CB もデバイスリセットで失われる。
        // ハンドルを落としておけば下の遅延生成が次フレームで作り直す。
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

    // パーティクルのクワッド用インデックスバッファ (最大描画数分を事前確保)。
    // 頂点側は共有せず、描画時に DynamicVertexBufferPool から 1 エミッターぶんずつ借りる。
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

    // UI の描画先種別を安定キーにして、Scene / Game の中間リソースを分離する。
    // uiOptions がない通常ゲーム描画は key=0 の単一コンテキストを使う。
    const uint32_t viewKey = uiOptions
        ? static_cast<uint32_t>(uiOptions->targetView) + 1u
        : 0u;
    static std::unordered_map<uint32_t, ViewRenderTargets> s_viewTargets;
    static uint64_t sRenderTargetResetVersion = 0;
    if (sRenderTargetResetVersion != resources.GetResetVersion()) {
        // ResourceManager::Reset() 後は旧ハンドルが無効なので Release せずキャッシュだけ破棄する。
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
    // advancedGraphicsCB はビュー別に生成する。
    // s_viewTargets.clear() によるデバイスリセット後は無効になるため、ここで lazily 再生成する。
    if (!viewTargets.advancedGraphicsCB.IsValid())
        viewTargets.advancedGraphicsCB = resources.CreateConstantBuffer(sizeof(AdvancedGraphicsCB));
    auto& advancedGraphicsCB = viewTargets.advancedGraphicsCB;
    // 自動露出。ヒストグラムは「読んだ後に自分でクリアする」設計なので初回だけ 0 が要る
    // (DEFAULT ヒープの初期内容は未定義)。
    if (!viewTargets.exposureHistogram.IsValid()) {
        const std::vector<uint32_t> zeros(kExposureHistogramBins, 0u);
        viewTargets.exposureHistogram = resources.CreateRWStructuredBuffer(
            zeros.data(), kExposureHistogramBins, static_cast<uint32_t>(sizeof(uint32_t)));
    }
    if (!viewTargets.exposureResult.IsValid()) {
        // 負値は「まだ順応していない」の印。CS 側が reset と同じ扱いで拾う。
        const float initial = -1.0f;
        viewTargets.exposureResult =
            resources.CreateRWStructuredBuffer(&initial, 1, static_cast<uint32_t>(sizeof(float)));
    }

    // 出力先の実寸。UI はこの寸法で描く (描画スケールの影響を受けない)。
    uint32_t nativeW = 0;
    uint32_t nativeH = 0;
    // 内部解像度と出力先の実寸が食い違うフレームか。UpscalePass の要否そのもの。
    bool needsUpscale = false;
    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ResizeRenderTargets");
        const auto* output = resources.Get(outputRT);
        nativeW = output ? output->GetWidth()  : renderer.GetWidth();
        nativeH = output ? output->GetHeight() : renderer.GetHeight();
        if (nativeW == 0 || nativeH == 0) return;

        // 内部描画解像度。ここで倍率を掛ければ中間 RT もビューポートも texelSize も追従する。
        // 実寸へ戻すのは UpscalePass ただ 1 つ。ポストの各段が outputRT へ直接書くと、
        // その段だけが実寸で走り、描画スケールで浮かせたはずのコストが最後に戻ってくる。
        uint32_t curW = 0;
        uint32_t curH = 0;
        renderer::ResolveRenderResolution(nativeW, nativeH, rs.renderScale, curW, curH);
        if (curW == 0 || curH == 0) return;
        if (!hdrRT.IsValid() || sHdrW != curW || sHdrH != curH)
        {
            // 全画面ポストの中継先。深度テストも深度書き込みもしないので深度を持たない。
            // WHY ここだけ落とせるか: 深度が要るのは «ジオメトリを描く RT» と
            //     «深度を SRV で読まれる RT» の 2 つだけ。中継先はどちらでもない
            //     (GetDepthTexture の引数を全部当たって確認済み)。
            //     1080p で 1 枚 8MB、ビューごとに 7 枚ぶん浮く。
            constexpr renderer::RenderTargetDesc kPostChainRT{
                /*colorCount=*/1, renderer::Format::RGBA16F, /*withDepth=*/false };

            ReleaseViewRenderTargets(viewTargets, resources);
            hdrRT           = resources.CreateRenderTarget(curW, curH, 1);
            ldrRT           = resources.CreateRenderTarget(curW, curH, kPostChainRT);
            // 選択マスクと輪郭マスクはジオメトリを描き、深度も読まれる。
            selectionMaskRT = resources.CreateRenderTarget(curW, curH, 1);
            outlineRT       = resources.CreateRenderTarget(curW, curH, kPostChainRT);
            objectMaskRT   = resources.CreateRenderTarget(curW, curH, 1);
            customPostProcessRT[0] = resources.CreateRenderTarget(curW, curH, kPostChainRT);
            customPostProcessRT[1] = resources.CreateRenderTarget(curW, curH, kPostChainRT);
            gbufferRT       = resources.CreateRenderTarget(curW, curH, 2);
            velocityRT      = resources.CreateRenderTarget(curW, curH, 1);
            decalDepthRT    = resources.CreateRenderTarget(curW, curH, 0);
            decalMaskRT     = resources.CreateRenderTarget(curW, curH, 1);
            // Bloom のミップ連鎖。段ごとに 1/2、1 まで来たら以降は同寸法のまま確保する。
            // ダウンサンプル用と足し戻し用の 2 系統。足し戻しは「1 段小さいぼけ + 自分の段の元」
            // を読んで書くので、読みながら書けない以上は書き先を分ける必要がある。
            for (uint32_t i = 0; i < kBloomMipCount; ++i) {
                const uint32_t div = 2u << i;   // 2, 4, 8, 16, 32
                const uint32_t mw = (std::max)(1u, curW / div);
                const uint32_t mh = (std::max)(1u, curH / div);
                viewTargets.bloomChain[i]   = resources.CreateComputeTexture(mw, mh);
                viewTargets.bloomUpChain[i] = resources.CreateComputeTexture(mw, mh);
            }
            bloomHalf       = viewTargets.bloomChain[0];
            bloomFull       = resources.CreateComputeTexture(curW, curH);
            // AO / 接触影は低周波なので半解像度 (コスト約 1/4)。消費側が linear サンプルで戻す。
            // SSR は鏡面が崩れるためフル解像度のまま。
            ssaoRaw         = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2));
            ssaoBlur        = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2));
            // ---- Advanced Graphics per-view テクスチャ ----
            ssrResult           = resources.CreateComputeTexture(curW, curH);
            volumetricResult    = resources.CreateComputeTexture(curW, curH);
            taaHistoryA         = resources.CreateRenderTarget(curW, curH, kPostChainRT);
            taaHistoryB         = resources.CreateRenderTarget(curW, curH, kPostChainRT);
            motionBlurResult    = resources.CreateComputeTexture(curW, curH);
            gtaoRaw             = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2)); // 半解像度 AO
            gtaoBlur            = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2)); // 半解像度 AO
            contactShadowResult = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2)); // 半解像度 接触影
            sHdrW = curW;
            sHdrH = curH;
        }

        // アップスケール元。等倍のときは 1 枚まるごと無駄なので持たない。
        // WHY 上のリサイズ判定に混ぜないか: 内部解像度は kMinRenderWidth で床に張り付くので、
        //     出力先だけが動いて curW/curH が変わらないフレームがある。そこで倍率が等倍を
        //     またぐと、必要な RT が無いまま UpscalePass だけが登録される。
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

        // フロクセル霧のボリューム。解像度ではなくグリッド寸法で作り直す。
        // リサイズのたびに 14MB を作り直すとフレームが飛び、履歴が切れて霧が暗転する。
        // 無効化しても解放しないのは、切り戻した瞬間に確保が走るのを避けるため。
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
                // 中身が未初期化の 2 枚を「前フレーム」として読ませない。
                viewTargets.froxelState.grid[0] = 0u;
                viewTargets.froxelState.grid[1] = 0u;
                viewTargets.froxelState.grid[2] = 0u;
            }
        }
    }

    // =========================================================================
    // ライト収集とシャドウ範囲計算はシーン全体を走査するため、独立して計測する。
    profiler::Profiler::BeginSample(
        profiler::ProfilerMarker("RenderSystem::LightingSetup", "Rendering"));

    // ライト定数バッファを構築
    // =========================================================================
    renderer::LightConstantsCB lightData{};
    lightData.lightDir       = { 0.0f, -1.0f, 0.5f };
    lightData.lightColor     = { 1.0f,  1.0f, 1.0f };
    lightData.lightIntensity = 1.0f;

    // Directional Light のシャドウ設定 (LightComponent から取得)
    bool  dirCastShadows    = true;
    float dirShadowBias     = 1.0f;
    float dirShadowStrength = 1.0f;
    float dirShadowDistance = 0.0f;

    // クラスタライティング用の統合ライト配列。b3 の固定長配列と並行して構築する。
    // b3 は点 8 / スポット 4 で打ち切るが、こちらは 256 本まで拾う。
    // b3 の詰め方は変えないので、クラスタ未対応のパスの見た目は据え置き。
    std::vector<PunctualLightGPU> punctualLights;
    punctualLights.reserve(32);

    // 影を落とせるライトの候補 (Directional 以外の全型)。
    // アトラスは 16 タイルしかなく、走査順に配ると「シーンのどこに置いたか」で
    // 影の有無が決まる。全部集めてから捨てる相手を選ぶ。
    struct PunctualShadowCandidate {
        size_t        punctualIndex;   // punctualLights 内の位置
        int           legacySlot;      // b3 側の位置 (点 0-7 / スポット 8-11)。-1 = b3 に入らない
        // 全方位のライトはキューブ 6 面 = 6 タイルを使う。Spot / Area は 1 タイル。
        bool          needsCube;
        math::Vector3 position;
        math::Vector3 direction;       // 1 タイル側の照射方向 (キューブでは未使用)
        float         range;
        float         outerCone;       // [degrees] 1 タイル側の半画角
        float         nearPlane;
        float         bias;
        float         strength;
        float         sourceRadius;  // 半影の広がりを決める光源半径 [m]
        float         cameraDistSq;
    };
    std::vector<PunctualShadowCandidate> shadowCandidates;

    // Cookie を持つスポットの候補。割り当ての考え方は影と同じで、タイル数が
    // 有限 (8 枚) なのでカメラから近い順に配る。
    struct LightCookieCandidate {
        size_t        punctualIndex;
        int           legacySlot;
        math::Vector3 position;
        math::Vector3 direction;
        float         range;
        float         outerCone;   // [degrees]
        float         nearPlane;
        float         rotationRad;
        std::string   path;
        float         cameraDistSq;
    };
    std::vector<LightCookieCandidate> cookieCandidates;

    // b3 経路の点光源 / スポットの光源半径。添字は legacyShadowSlots と同じ。
    float legacySourceRadius[kMaxLegacyPunctualLights] = {};

    constexpr float kDegToRad = 3.14159265f / 180.0f;
    // キューブ 1 面ぶんの半画角 (= 90 度の半分)。Point シャドウの 6 面で使う。
    constexpr float kQuarterPi = 3.14159265f / 4.0f;
    // Area の影を焼く錐台の半画角 [degrees]。Spot の outerCone に相当する値として渡す。
    // 面光源は法線側の半球 (= 90 度) を照らすが、透視投影は 90 度で無限に広がるため
    // 張れない。75 度は「パネルの正面に置いた物の影は出る / 真横は諦める」の線。
    constexpr float kAreaShadowOuterConeDeg = 75.0f;
    // View<> だと GameObject が取れず activeInHierarchy() を見られないので、GO を切っても
    // 光だけが残る。GetEntities<> は View<> と同じ基底 span なので走査順は変わらない。
    for (EntityID id : scene.GetEntities<LightComponent>()) {
        GameObject*     go    = scene.GetGameObject(id);
        LightComponent* light = scene.GetComponent<LightComponent>(id);
        if (!go || !light || !go->activeInHierarchy() || !light->enabled) continue;
        const Transform&      tf = go->transform;
        const LightComponent& lc = *light;

        // 色温度モードでは color 欄ではなく colorTemperature が正本。
        // 毎フレーム引き直す。キャッシュは Inspector の反映漏れという見つけにくい種になる。
        const math::Vector3 lightColor =
            lc.useColorTemperature ? renderer::ColorFromTemperature(lc.colorTemperature)
                                   : lc.color;

        // 点光源 / スポット / 大きさを持つ光源は上限に達するまで統合配列へも積む。
        // b3 は「点を全部→スポットを全部」の 2 配列だがこちらは 1 本なので評価順が変わりうる。
        // 加算なので結果は同じ (順序による丸め差のみ)。
        if (lc.type != LightComponent::Type::Directional
            && punctualLights.size() < kMaxPunctualLights) {
            const size_t punctualIndex = punctualLights.size();
            PunctualLightGPU& gpu = punctualLights.emplace_back();
            // WHY worldPosition か: Transform::position は親基準のローカル座標。
            //     子 GameObject にライトを置くと (キャラクターの発光部・車のヘッドライト・
            //     ボーンに付けた松明)、親の姿勢が一切効かず原点付近に光が落ちる。
            //     向き (forward / right / up) は worldRotation から作られるので既に
            //     ワールド空間で、位置だけが取り残されていた。
            gpu.position  = tf.worldPosition;
            gpu.range     = lc.range;
            gpu.color     = lightColor;
            gpu.intensity = lc.intensity;
            // 既定は Point。他の型が以降で上書きする。
            gpu.direction   = { 0.0f, -1.0f, 0.0f };
            gpu.innerCos    = 0.0f;
            gpu.outerCos    = 0.0f;
            gpu.type        = static_cast<uint32_t>(PunctualLightType::Point);
            gpu.shadowIndex = -1;
            gpu.cookieIndex = -1;
            gpu.tangent     = { 1.0f, 0.0f, 0.0f };
            gpu.bitangent   = { 0.0f, 1.0f, 0.0f };
            // 点光源 / スポットでも halfWidth は光源半径として意味を持つ
            // (形状は点のまま、ハイライトの広がりと影のにじみ幅にだけ効く)。
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
                // 管の軸は Transform の Right。蛍光灯を横向きに置く姿勢が既定になる。
                gpu.tangent    = tf.right.NormalizedOr({ 1.0f, 0.0f, 0.0f });
                gpu.halfHeight = (std::max)(lc.sourceLength, 0.0f) * 0.5f;
                gpu.type       = static_cast<uint32_t>(PunctualLightType::Tube);
            } else if (lc.type == LightComponent::Type::Area) {
                gpu.direction  = tf.forward.NormalizedOr({ 0.0f, 0.0f, 1.0f });
                gpu.tangent    = tf.right.NormalizedOr({ 1.0f, 0.0f, 0.0f });
                gpu.bitangent  = tf.up.NormalizedOr({ 0.0f, 1.0f, 0.0f });
                gpu.halfWidth  = (std::max)(lc.areaWidth,  0.001f) * 0.5f;
                gpu.halfHeight = (std::max)(lc.areaHeight, 0.001f) * 0.5f;
                // Area では innerCos / outerCos が空くので、両面フラグの運搬に使う。
                // 専用フィールドを足すと 96 バイトの構造体がキャッシュライン 2 本に収まらない。
                gpu.outerCos   = lc.areaTwoSided ? 1.0f : 0.0f;
                gpu.type       = static_cast<uint32_t>(PunctualLightType::Area);
            }

            const math::Vector3 toCamera = tf.worldPosition - camera.m_position;
            const float cameraDistSq = math::Vector3::Dot(toCamera, toCamera);

            // b3 側でこのライトが取る添字。直後のブロックが末尾へ 1 つ足すだけなので、
            // 採番される番号は今のカウンタ値そのもの。
            int legacySlot = -1;
            if (lc.type == LightComponent::Type::Point && lightData.pointLightCount < 8)
                legacySlot = lightData.pointLightCount;
            else if (lc.type == LightComponent::Type::Spot && lightData.spotLightCount < 4)
                legacySlot = kLegacySpotSlotBase + lightData.spotLightCount;
            if (legacySlot >= 0)
                legacySourceRadius[legacySlot] = (std::max)(lc.sourceRadius, 0.0f);

            // Cookie の候補。Spot 専用 — Point はキューブマップ、Directional は
            // ワールド空間のタイリングという別の仕組みが要る。
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

            // 影の候補として控える。Directional 以外は全型が落とせる。
            //
            // WHY 形状を持つ光源も «点から焼いた影» でよいか: 影の形は遮蔽物と受光面の
            //     配置でほぼ決まり、光源の大きさは半影の広さにしか効かない。その広さは
            //     sourceRadius から作る penumbraTexels が受け持つので、深度そのものは
            //     中心 1 点から焼けば足りる。管が長いほど本当は半影が軸方向へ伸びるが、
            //     それを出すには軸に沿った複数枚が要り、16 タイルでは到底足りない。
            const bool canCastShadow =
                lc.castShadows && lc.shadowStrength > 0.0f &&
                lc.type != LightComponent::Type::Directional;
            if (canCastShadow) {
                // 遠すぎるライトへタイルを割り当てない。判定距離に range を足すのは、
                // range の大きいライトは離れていても画面を広く照らすため。
                const float limit = rs.shadow.punctualShadowDistance + lc.range;
                if (cameraDistSq <= limit * limit) {
                    PunctualShadowCandidate cand{};
                    cand.punctualIndex = punctualIndex;
                    cand.legacySlot    = legacySlot;
                    // Sphere / Tube は Point と同じ全方位。Area だけが向きを持つ。
                    cand.needsCube     = (lc.type == LightComponent::Type::Point
                                       || lc.type == LightComponent::Type::Sphere
                                       || lc.type == LightComponent::Type::Tube);
                    cand.position      = tf.worldPosition;
                    cand.direction     = cand.needsCube
                                       ? math::Vector3{ 0.0f, -1.0f, 0.0f }
                                       : tf.forward.NormalizedOr({ 0.0f, 0.0f, 1.0f });
                    cand.range         = (std::max)(lc.range, 0.05f);
                    // Area は法線側の半球を照らすが、1 枚の透視投影では 180 度を張れない。
                    // 実用上そこまでで、これ以上広げると端のテクセル密度が落ちるだけ。
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

    // 粒子を点光源にする (ParticleEmitter の Lights モジュール)。LightComponent の後に積むので、
    // 枠が足りないときに削られるのは粒子の光の方。Legacy (b3) には載せない。
    // NOTE: GPU シミュレーションの粒子は位置が GPU にしか無いので対象外 (Inspector に注記がある)。
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

    // ── Spot / Point シャドウのスロット割り当てと行列の組み立て ────────────────────
    // カメラから近い順。遠いライトの影は数ピクセルにしかならず落としても気づかれにくい。
    // 距離キーは連続に変化するので、あふれの切り替わりも端から 1 つずつ起きる。
    std::sort(shadowCandidates.begin(), shadowCandidates.end(),
              [](const PunctualShadowCandidate& a, const PunctualShadowCandidate& b) {
                  return a.cameraDistSq < b.cameraDistSq;
              });

    // アトラスは 4x4 = kMaxPunctualShadows タイル。Spot が 1 枚、Point が 6 枚を使う。
    constexpr uint32_t kPunctualTilesPerSide = 4u;
    static_assert(kPunctualTilesPerSide * kPunctualTilesPerSide
                      == static_cast<uint32_t>(kMaxPunctualShadows),
                  "punctual shadow atlas tiling must cover exactly kMaxPunctualShadows tiles");
    const uint32_t punctualTileSize =
        (std::max)(punctualShadowRes / kPunctualTilesPerSide, 1u);
    const float    punctualAtlasResF = static_cast<float>(punctualShadowRes);
    const float    punctualUvScale   =
        static_cast<float>(punctualTileSize) / punctualAtlasResF;

    // キューブ 6 面の向きと up。順序は PunctualShadow.hlsli の FBZZ_CubeFaceIndex と
    // 一致させること (+X, -X, +Y, -Y, +Z, -Z)。
    // up は描く行列と引く行列が同じなら何でもよい (両方ここで作った 1 本を使う)。
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
    // キューブ 6 面を使ったライトの本数 (Point / Sphere / Tube)。
    int shadowedCubeCount   = 0;
    // b3 経路 (既定の Forward) 向けのスロット番号。-1 = 影なし。
    int legacyShadowSlots[kMaxLegacyPunctualLights];
    for (int& slot : legacyShadowSlots) slot = -1;

    // タイル 1 枚を組み立てる。halfFovRad はそのタイルの投影半画角。
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

        // 基本バイアスは「1 テクセルが覆うワールド距離」。アクネはテクセルの幅の中で
        // 面の深度が変わることから出るので、補正量はテクセルの実寸そのものになる
        // (斜め面ぶんの tan(theta) はシェーダー側の FBZZ_PunctualSlopeBias が掛ける)。
        // 評価点が range の中ほどなのは、透視投影ではテクセル実寸が深度に比例するため。
        const float midZ       = (std::max)((nearZ + farZ) * 0.5f, nearZ * 2.0f);
        const float texelWorld =
            2.0f * midZ * std::tan(halfFovRad) / static_cast<float>(punctualTileSize);
        // 透視投影の NDC 深度は非線形なので、ワールド距離をそのまま渡せない。
        //   z_ndc = f/(f-n) * (1 - n/z)  →  dz_ndc/dz = f*n / ((f-n) * z^2)
        const float ndcPerWorld =
            (farZ * nearZ) / ((std::max)(farZ - nearZ, 0.001f) * midZ * midZ);
        view.biasNDC = texelWorld * ndcPerWorld * biasScale;

        // 1 テクセルが張る角度。ShadowPass の極小 caster カリングが使う。
        view.texelAngularSize =
            2.0f * std::tan(halfFovRad) / static_cast<float>(punctualTileSize);

        // 光源半径がシャドウマップ上で何テクセルぶんの半影になるか。
        // 本来は「光源の大きさ × 遮蔽物と受光面の距離比」だが、ブロッカー探索が無いので
        // 比を 1 とみなす。遮蔽物が遠いほど硬くなるが「大きな電球ほど柔らかい」は出る。
        view.penumbraTexels = (texelWorld > 0.0f) ? (sourceRadius / texelWorld) : 0.0f;
    };

    if (rs.shadowEnabled) {
        for (const PunctualShadowCandidate& cand : shadowCandidates) {
            const int needed = cand.needsCube ? 6 : 1;
            // break ではなく continue。全方位のライトが入らなかっただけで、後ろに続く
            // Spot / Area は 1 枚で収まる可能性がある。
            if (punctualViewCount + needed > kMaxPunctualShadows) continue;
            if (cand.needsCube && shadowedCubeCount >= rs.shadow.maxShadowedPointLights) continue;

            // Inspector で range より大きい shadowNearPlane を入れられるので、
            // ここで潰さないと Matrix4::Perspective の assert を踏む。
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
                // 錐台は円錐へ外接させる。outerCone は半角なので画角はその 2 倍。
                // 少し広げるのは、ぴったり切ると PCF が縁ではみ出して影が欠けるため。
                // Area はコーンを持たないので kAreaShadowOuterConeDeg が入っている。
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
            // クラスタ経路はライト構造体から、レガシー経路は b12 の対応表から番号を引く。
            // どちらの経路でも同じスロットを指すよう、ここで両方へ書く。
            punctualLights[cand.punctualIndex].shadowIndex = baseSlot;
            if (cand.legacySlot >= 0 && cand.legacySlot < kMaxLegacyPunctualLights)
                legacyShadowSlots[cand.legacySlot] = baseSlot;
        }
    }

    // ── Cookie のスロット割り当て ────────────────────────────────────────────────
    // 影と同じくカメラから近い順。タイルは 8 枚しかない。
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

        // 投影は影と同じ「スポットの円錐に外接する透視錐台」。
        // 影の行列を流用しないのは、Cookie が影を落とさないライトにも付くため。
        // 縁を広げないのは、近傍サンプルが無く広げると模様がコーンより内側で終わるため。
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

    // ── 昼夜の色・強度カーブ (Phase B) ─────────────────────────────────────────────
    // 太陽の向きは DirectionalLight の transform が唯一のソース (lightDir は上書きしない)。
    // dayNightEnabled のときは、その光源の太陽高度から色と強度の遷移だけを駆動する。
    // ライトを回すと 太陽ディスク・空・月・空連動 IBL・ライティングが一緒に動く。
    // 雲シャドウ params (Phase C) も SkyRenderer から読み、passCtx へ後で転送する。
    float skyCloudShadowStrength = 0.0f, skyCloudShadowCoverage = 0.5f,
          skyCloudShadowScale = 0.02f, skyCloudShadowSpeed = 1.0f;
    // 太陽の向きは DirectionalLight 側で決まるため SkyRenderer の Transform は使わない。
    for (EntityID id : scene.GetEntities<SkyRenderer>()) {
        GameObject* go     = scene.GetGameObject(id);
        auto*       skyPtr = scene.GetComponent<SkyRenderer>(id);
        if (!go || !skyPtr || !go->activeInHierarchy() || !skyPtr->enabled) continue;
        const SkyRenderer& sky = *skyPtr;

        // 雲シャドウは昼夜サイクルとは独立に常に反映する。
        skyCloudShadowStrength = sky.cloudShadowStrength;
        skyCloudShadowCoverage = sky.cloudShadowCoverage;
        // Component は「まだら 1 周期の大きさ [m]」。シェーダーは world→UV スケールを要る。
        skyCloudShadowScale    = 1.0f / (std::max)(sky.cloudShadowSize, 1.0f);
        skyCloudShadowSpeed    = sky.cloudShadowSpeed;

        if (sky.dayNightEnabled) {
            // 太陽方向 (toward sun) = -lightDir。その高度 [度] を軸に 夜 ↔ 夕方 ↔ 昼 を補間する。
            // 高度 0° を夕方のキーに置くと「ライトを水平 = 夕方」になり、昼側と夜側それぞれ
            // 独立した帯幅で抜けられる。旧実装は夕焼けの重みに昼の重みを掛けており、
            // 地平線上で重みが 0.17 まで落ちて夕方を作れなかった。
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
            t = t * t * (3.0f - 2.0f * t); // smoothstep: 帯の端で色・明るさが折れないようにする

            lightData.lightColor     = lerp3(sky.sunsetColor,
                                             above ? sky.dayColor : sky.nightColor, t);
            lightData.lightIntensity = lerp1(sky.sunsetIntensity,
                                             above ? sky.dayIntensity : sky.nightIntensity, t);
            // 空の明るさは太陽光の強さとは別軸。共用していた頃は太陽を強くすると空も白飛びした。
            // 詳細は SkyRenderer::skyDayBrightness。
            lightData.skyDimmer      = lerp1(sky.skySunsetBrightness,
                                             above ? sky.skyDayBrightness : sky.skyNightBrightness, t);
        }
        break;
    }

    // ambientColor: Lit モードでは AMBIENT_SCALE 相当値、Unlit 系では白に上書き
    lightData.ambientColor = { 0.08f, 0.08f, 0.08f };
    if (rs.IsUnlit()) {
        lightData.ambientColor    = { 1.0f, 1.0f, 1.0f };
        lightData.lightIntensity  = 0.0f;
        lightData.pointLightCount = 0;
        lightData.spotLightCount  = 0;
        // 空も消灯する。分離前は lightIntensity=0 が空系シェーダーにも効いていたので、
        // Unlit 表示で空が黒く落ちる従来の挙動を skyDimmer 側で維持する。
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

    // ── カスケードシャドウ (CSM) のフィッティング ───────────────────────────────
    // 精細さを決めるのは解像度ではなく「1 テクセルが覆うワールド距離」。単一マップでは
    // 到達距離を伸ばすと分母が伸びるだけで、近距離の精細さと両立しない。
    // 視錐台を距離で区切り、手前ほど狭い範囲へ 1 タイルを割り当てて足元の密度だけ上げる。
    // shadowBounds (シーン全体) は「これ以上大きくしない」上限としてだけ使う。
    const int cascadeCount =
        std::clamp(rs.shadow.cascadeCount, 1, fbzz::renderer::kMaxShadowCascades);

    // アトラス配置: 1 分割なら全面、2 分割以上なら 2x2 タイル。
    // 1 枚に収めれば影を読む 20 以上のシェーダーがバインドもサンプラーも変えずに済む。
    // 解像度とメモリは分割数によらず一定で、変わるのは面積の配分だけ。
    const uint32_t atlasResolution = (std::max)(rs.shadow.mapResolution, 1u);
    const uint32_t tilesPerSide    = (cascadeCount > 1) ? 2u : 1u;
    const uint32_t tileSize        = (std::max)(atlasResolution / tilesPerSide, 1u);

    // 影の最大到達距離。LightComponent::shadowDistance > 0 は従来どおり手動指定を優先する。
    const float shadowDistance = (dirShadowDistance > 0.0f)
        ? (std::max)(dirShadowDistance, 1.0f)
        : (std::max)(rs.shadow.autoFitDistance, 1.0f);

    float cascadeSplits[fbzz::renderer::kMaxShadowCascades] = {};
    ComputeCascadeSplits((std::max)(camera.m_near, 0.01f), shadowDistance,
                         cascadeCount, rs.shadow.cascadeSplitLambda, cascadeSplits);

    const math::Vector3 up = (std::abs(lightDir.y) > 0.99f)
                             ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                             : math::Vector3{ 0.0f, 1.0f, 0.0f };
    // 位置を持たない回転だけのライト空間。テクセルスナップの量子化格子として使う。
    const math::Matrix4 snapView     = math::Matrix4::LookAt(math::Vector3::ZERO, lightDir, up);
    const math::Matrix4 snapViewInv  = math::Matrix4::Inverse(snapView);

    ShadowCascade cascades[fbzz::renderer::kMaxShadowCascades] = {};
    float cascadeSliceNear = (std::max)(camera.m_near, 0.01f);

    for (int i = 0; i < cascadeCount; ++i) {
        const float sliceFar = cascadeSplits[i];

        // このカスケードが担当する視錐台スライスの外接球 (向きに依存しないので回転で脈動しない)。
        const FrustumSliceSphere slice =
            ComputeFrustumSliceSphere(camera, cascadeSliceNear, sliceFar);

        // シーン全体より大きい影ボリュームを作っても無駄なテクセルが増えるだけ。
        float radius = (std::max)((std::min)(slice.radius, shadowBounds.radius), 1.0f);

        math::Vector3 center = camera.m_position + camera.GetForward() * slice.centerDistance;
        if (radius < shadowBounds.radius) {
            // シーン球からはみ出さないよう、中心をシーン球内へ引き戻す。
            const math::Vector3 offset   = center - shadowBounds.center;
            const float         distance = offset.Length();
            const float         limit    = (std::max)(shadowBounds.radius - radius, 0.0f);
            if (distance > limit && distance > 0.0001f)
                center = shadowBounds.center + offset * (limit / distance);
        } else {
            center = shadowBounds.center;
        }

        // テクセルスナップ。中心がカメラに追従するとサブテクセルのずれで輪郭が波打つ
        // (shadow swimming)。ライト空間で 1 テクセル単位へ量子化すると標本位置が固定される。
        const float texelWorldSize = (radius * 2.0f) / static_cast<float>(tileSize);
        {
            math::Vector4 lightSpace =
                snapView * math::Vector4{ center.x, center.y, center.z, 1.0f };
            lightSpace.x = std::floor(lightSpace.x / texelWorldSize) * texelWorldSize;
            lightSpace.y = std::floor(lightSpace.y / texelWorldSize) * texelWorldSize;
            const math::Vector4 snapped = snapViewInv * lightSpace;
            center = { snapped.x, snapped.y, snapped.z };
        }

        // 深度レンジ。ボリュームの外にいる背の高い caster も影を落とせるよう、
        // ライト方向の引きはシーン全体の広がりから取る (near/far を広げても塗る面積は増えない)。
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
        // ワールド空間で約 5mm 相当の一定バイアスになるよう深度レンジで正規化する。
        // カスケードごとにレンジが違うので、値もカスケードごとに持つ。
        cascade.biasNDC = (0.005f * dirShadowBias) / (std::max)(depthRange - 1.0f, 1.0f);

        // アトラス内のタイル位置 (2x2 を左上から Z 字順に埋める)。
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

    // 単一のライト行列で足りるパス (パーティクル自己影など) 向けの代表値。
    // 到達範囲を最も広く覆う最遠カスケードを渡す。ビルボードの正対に view の内訳が要るので、
    // view と viewProjection は必ず同じカスケードから対で渡す。
    const ShadowCascade& widestCascade = cascades[cascadeCount - 1];
    const math::Matrix4  lightVP   = widestCascade.viewProjection;
    const math::Matrix4  lightView = widestCascade.view;
    const math::Vector3  lightPos  = widestCascade.eyePos;

    // 不透明物は可能な限り共通の GBuffer → AO → DeferredLighting 経路を通す。
    // Forward 直描きでは SSAO/GTAO/ContactShadows/SSR/IBL が Terrain に乗らないため。
    // 必須リソースが欠けるときだけ従来 Forward へ落ちる。
    // 「GBuffer を作るパイプラインか」の定義は RenderSettings::UsesGBuffer() が唯一で、
    // UI の警告 (PipelineDiagnostics) も同じ関数を見る。
    const bool wantsDeferredPipeline = rs.UsesGBuffer();
    // GBuffer (法線 + 深度 + roughness) を描けるか。Deferred の本経路と、
    // Forward のプリパスの両方がこれを土台にする。
    const bool gbufferAvailable = gbufferRT.IsValid() && gbufferShader.IsValid();
    const bool useGBufferOpaquePipeline =
        wantsDeferredPipeline &&
        gbufferAvailable &&
        deferredLightingShader.IsValid() &&
        depthCopyShader.IsValid();

    // ── Forward の GBuffer プリパス ────────────────────────────────────────────
    // SSAO / GTAO / SSR / 接触影 はどれも GBuffer の法線と深度から作る。Forward には
    // 書く場所が無く、同じ設定でも効果が丸ごと消えていた。
    // 不透明ジオメトリをもう 1 回描くので、画面空間系を要求されたときだけ走らせる。
    // 副産物として深度プリパスにもなり、本描画で early-Z が効く。
    const bool needsScreenSpaceInputs =
        rs.postProcess.ambientOcclusion.enabled || rs.IsGtaoActive()
        || rs.contactShadow.enabled || rs.ssr.enabled;
    const bool forwardGBufferPrepass =
        !useGBufferOpaquePipeline && gbufferAvailable
        && needsScreenSpaceInputs && !rs.IsUnlit();

    // 画面空間系を走らせられるか。Deferred の本経路でも Forward のプリパスでも成立する。
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

    // ランタイムのオブジェクトマスク。showSelectionOutline (エディタの表示切り替え) には
    // 従わない ─ あちらは「編集中の選択を出すか」の設定で、ゲームの見た目を消す権限は
    // 持たない。
    //
    // WHY 生きた申告の有無で切るか: 申告が 1 件も無いフレームまでマスクを描くと、
    //     何も印されていない盤面で全画面のクリアと 1 パスぶんの帯域を毎フレーム捨てる。
    // WHY 理由を出すか: 4 つの条件のどれで落ちても症状は «輪郭が出ない» の 1 種類で、
    //     しかも黙って落ちる。どれが欠けているかを 1 行で名指しできるようにしておく。
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
            // 申告先と読み手が同じ実体かを直接見る。RenderSystem は settings を «複製» して
            // 使うので、複製元のアドレスが Application の active と一致しているかが要点。
            const void* readFrom = static_cast<const void*>(settings);
            const void* active   = static_cast<const void*>(
                core::Application::Get().GetActiveRenderSettings());
            FBZZ_LOG_INFO("ObjectMask: %s (requests=%zu / 読み手=%p 申告先=%p %s)",
                          reason, rs.objectMaskRequests.size(), readFrom, active,
                          readFrom == active ? "一致" : "不一致");
        }
    }

    // 「有効なのに現在のパイプラインでは無視される設定」をログへ出す。
    // エディタを開かずにビルドする経路でも同じ落とし穴を踏むので、警告表示だけでは足りない。
    // 変化時だけ出す。毎フレーム出すとログが埋まって本当のエラーが見えなくなる。
    //
    // WHY settings が無いフレームは黙るか: 診断が指しているのは «プロジェクトの設定» で、
    //     設定を渡されなかった呼び出し (起動時の Warmup 等) が使う既定値は誰も書いていない。
    //     既定は Forward + Clustered 有効なので、Deferred+ のプロジェクトでも必ず
    //     «Clustered Lights は無視される» が 1 度出る。身に覚えのない警告になる。
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

    // =========================================================================
    // RenderPassHandles を組み立て
    // =========================================================================
    RenderPassHandles passHandles{};
    // selectionMaskRT / outlineRT はハンドルを配らない。
    // 名前 ("SelectionMask" / "Outline") から res.Target() で引く (登録簿を参照)。
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
    passHandles.objectMaskSkinnedShader  = objectMaskSkinnedShader;
    passHandles.copyColorShader           = copyColorShader;
    passHandles.upscaleShader             = upscaleShader;
    passHandles.customComposeShader       = customComposeShader;
    passHandles.fxaaShader        = fxaaShader;
    passHandles.customPostProcessShaders.resize(rs.postProcess.customEffects.size());
    // 走る段でリストを分ける。登録順が RenderGraph のタイブレークなので、
    // 同じ段の中では customEffects に並べた順がそのまま適用順になる。
    std::vector<uint32_t> customAfterOpaqueIndices;
    std::vector<uint32_t> customSceneHdrIndices;
    std::vector<uint32_t> customPostProcessIndices;
    customPostProcessIndices.reserve(rs.postProcess.customEffects.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(rs.postProcess.customEffects.size()); ++i) {
        const auto& custom = rs.postProcess.customEffects[i];
        if (!custom.enabled) continue;
        // shaderPath は .mat を持たないパスの受け皿。materialPath 側の解決は
        // パス本体が毎フレーム行う (シェーダーもテクスチャも .mat が決めるため)。
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
    // WHY ここも出すか: マスクを «読む» パスが 1 本も無いと、RenderGraph は
    //     ObjectMask パスごと刈る (BuildExecutionOrder)。輪郭が出ない症状は
    //     «申告が届いていない» と «読み手が居なくて刈られた» で同じに見える。
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
    // Cookie 焼き。全画面三角形を不透明で塗るだけなので postproc と同じ状態でよい。
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

    // 散乱ボリュームは 2 枚をフレームごとに入れ替える。Inject が scatter へ書きながら
    // history を読むので、同じテクスチャを UAV と SRV に同時に張れない。
    // 向きもビュー別。static だと 1 フレームに 2 回反転し、互いの履歴を読み合う。
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

    // ── スキンドモデルのリファレンスポーズ CB を遅延生成 ─────────────────
    // AnimatorComponent を持たない SkinnedMeshRenderer の既定パレット。スケルトン単位に
    // 1 本で全インスタンスが共有する。無いと単位行列へ落ち、バインド変換を持つアセットが倒れる。
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
    // ── ジオメトリ用ハンドル ──────────────────────────────────────────────────
    passHandles.shadowShader         = shadowShader;
    passHandles.shadowSkinnedShader  = skinnedShadowShader;
    passHandles.shadowCB             = shadowCB;
    passHandles.velocityShader        = velocityShader;
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
    passHandles.deferredLightingShader = deferredLightingShader;
    passHandles.depthCopyShader      = depthCopyShader;
    passHandles.clusterCullCS        = clusterCullCS;
    passHandles.punctualLightBuffer  = punctualLightPool.Acquire(
        resources, kMaxPunctualLights, static_cast<uint32_t>(sizeof(PunctualLightGPU)));
    passHandles.clusterIndexBuffer   = clusterIndexBuffer;
    passHandles.clusterCB            = clusterCB;
    passHandles.clusterLinearCB      = clusterLinearCB;

    // ---- Advanced Graphics ハンドルを passHandles に束縛 ----
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
    // IBL BRDF LUT: static ComputeTexture。IBLBakePass が初回フレームで書き込む。
    passHandles.iblBrdfLut           = iblBrdfLut;
    passHandles.iblBrdfBakeShader    = iblBrdfBakeShader;
    // IBL キューブマップ: RenderSettings に指定されたパスを毎フレーム LoadTexture でキャッシュ参照する。
    // WHY: LoadTexture は内部でキャッシュするため、毎フレーム呼んでも I/O は初回のみ。
    if (!rs.ibl.irradiancePath.empty())
        passHandles.iblIrradiance = resources.LoadTexture(rs.ibl.irradiancePath);
    if (!rs.ibl.prefilterPath.empty())
        passHandles.iblPrefilter  = resources.LoadTexture(rs.ibl.prefilterPath);
    // SSR
    passHandles.ssrResult            = ssrResult;
    passHandles.ssrShader            = ssrShader;
    // Volumetric Lighting
    passHandles.volumetricResult     = volumetricResult;
    passHandles.volumetricShader     = volumetricShader;
    // TAA (ping-pong)
    passHandles.taaHistoryA          = taaHistoryA;
    passHandles.taaHistoryB          = taaHistoryB;
    passHandles.taaFlip              = viewTargets.taaFlip;
    passHandles.taaShader            = taaShader;
    passHandles.taaPSO               = taaPSO;
    // Motion Blur
    passHandles.motionBlurResult     = motionBlurResult;
    passHandles.motionBlurShader     = motionBlurShader;
    // GTAO
    passHandles.gtaoRaw              = gtaoRaw;
    passHandles.gtaoShader           = gtaoShader;
    passHandles.gtaoBlurShader       = gtaoBlurShader;
    // Contact Shadows
    passHandles.contactShadowShader  = contactShadowShader;
    // Lens Flare
    passHandles.lensFlareShader      = lensFlareShader;
    passHandles.lensFlarePSO         = lensFlarePSO;

    // カメラ視錐台とライト視錐台。Gribb-Hartmann 法は VP 行列の行の和・差から
    // 6 平面を直接出せるので逆行列が要らない。全ジオメトリパスで共有する。
    const math::Frustum cameraFrustum = math::Frustum::FromViewProjection(camera.GetViewProjection());
    const math::Frustum lightFrustum  = math::Frustum::FromViewProjection(lightVP);
    OcclusionCuller occlusionCuller;

    // 参照メンバーまでは集成体初期化で埋めざるを得ないが、それ以降は名前付きで代入する。
    // 位置指定のままだと RenderPassContext へフィールドを 1 つ挿すだけで以降が全部ずれる。
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
    // 極小オブジェクト判定用の射影スケール = 1/tan(fovY/2)。
    // GetProjectionMatrix() は毎回行列を組み直すので、オブジェクトごとに呼ぶと判定より高い。
    passCtx.cullProjScaleY    = camera.GetProjectionMatrix().m[1][1];
    passCtx.cullOrthographic  =
        camera.m_projection == renderer::ProjectionMode::Orthographic;
    passCtx.cullCameraForward = camera.GetForward();
    passCtx.width                   = sHdrW;
    passCtx.height                  = sHdrH;
    // ---- 名前 → 実ハンドルの登録簿 ----
    // グラフへ申告するのも、パスが引くのも同じ名前。ここが唯一の対応表になる。
    // WHY 毎フレーム埋めるか: 中身 (TAA の ping-pong、UpscaleSrc の有無、GBuffer の
    //     使用可否) はフレームごとに変わる。作り直すのは数十件なので、
    //     «いつのものか分からない対応表» を持ち回るより素直。
    {
        auto& reg = passCtx.resourceRegistry;
        reg.Clear();
        reg.BindTarget("Output",             outputRT);
        reg.BindTarget("HDR",                hdrRT);
        reg.BindTarget("LDR",                ldrRT);
        reg.BindTarget("ShadowMap",          shadowMapRT);
        reg.BindTarget("PunctualShadowMap",  punctualShadowRT);
        reg.BindTarget("LightCookieAtlas",   lightCookieRT);
        reg.BindTarget("SelectionMask",      selectionMaskRT);
        reg.BindTarget("Outline",            outlineRT);
        reg.BindTarget("ObjectMask",         objectMaskRT);
        reg.BindTarget("Velocity",           velocityRT);
        reg.BindTarget("CustomPostProcess0", customPostProcessRT[0]);
        reg.BindTarget("CustomPostProcess1", customPostProcessRT[1]);
        reg.BindTarget("GBuffer",            gbufferRT);
        reg.BindTarget("DecalDepth",         decalDepthRT);
        reg.BindTarget("UpscaleSrc",         upscaleSrcRT);
        // Kind が Texture のもの (CS 出力)。RT ではないので別の口へ入れる。
        reg.BindTexture("Bloom",               bloomFull);
        reg.BindTexture("SSAO",                ssaoBlur);
        reg.BindTexture("GTAOResult",          gtaoBlur);
        reg.BindTexture("ContactShadowResult", contactShadowResult);
        reg.BindTexture("SSRResult",           ssrResult);
        reg.BindTexture("MotionBlurResult",    motionBlurResult);
        reg.BindTexture("VolumetricResult",    volumetricResult);
        reg.BindTexture("LensFlareSource",     bloomHalf);
    }

    passCtx.uiOptions               = uiOptions;
    passCtx.outputWidth             = nativeW;
    passCtx.outputHeight            = nativeH;
    // ポストプロセスチェーンの終着点。等倍なら従来どおり outputRT へ直接書き切る。
    passCtx.chainOutputRT           = (needsUpscale && upscaleSrcRT.IsValid()) ? upscaleSrcRT : outputRT;
    // TAA サブピクセルジッター。8 フレーム周期の Halton(2,3) をピクセル内 ±0.5 に写す。
    // 周期 8 は収束の速さと品質の標準的な折衷。TAA が無効なフレームは 0 のまま
    // (ジッターだけ残すと画面全体が揺れて見える)。
    if (rs.IsTaaActive() && sHdrW > 0u && sHdrH > 0u) {
        constexpr uint32_t kTaaJitterPeriod = 8u;
        const uint32_t sampleIndex = viewTargets.taaFrameIndex % kTaaJitterPeriod + 1u;
        const float offsetPxX = HaltonRadicalInverse(sampleIndex, 2u) - 0.5f;
        const float offsetPxY = HaltonRadicalInverse(sampleIndex, 3u) - 0.5f;
        // ピクセル → NDC。NDC の Y は上向きなので符号を反転する。
        passCtx.taaJitterNdcX =  2.0f * offsetPxX / static_cast<float>(sHdrW);
        passCtx.taaJitterNdcY = -2.0f * offsetPxY / static_cast<float>(sHdrH);
        ++viewTargets.taaFrameIndex;
    } else {
        viewTargets.taaFrameIndex = 0u;
    }
    passCtx.selectionOutlineEnabled = selectionOutlineEnabled;
    passCtx.objectMaskEnabled      = objectMaskEnabled;
    // 登録条件 (Forward: forwardGBufferPrepass / Deferred: useGBufferOpaquePipeline) と
    // ExecuteSSRPass の早期 return を合わせた «本当に走るか»。
    passCtx.ssrPassActive =
        rs.ssr.enabled && screenSpaceReady &&
        ssrShader.IsValid() && ssrResult.IsValid() && gbufferRT.IsValid();
    // UI 要素の矩形も 3D と同じ選択マスクへ乗せ、輪郭の描き方を 1 か所に保つ。
    // 寸法が nativeW/H なのは、UI がポストプロセス後の outputRT へ実寸で描かれ、
    // Canvas Scaler の解釈も出力実寸で決まるため (渡すのはクリップ空間の行列)。
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

    // ── 前方描画のマテリアルへ渡す画面空間の遮蔽 ──────────────────────────────
    // GTAO と SSAO は排他 (IsGtaoActive が解決済み)。走った方を 1 つのスロットへ入れる。
    // Deferred でも渡すのは、半透明・エフェクト・スキンドが Forward で描かれ
    // DeferredLighting を通らないため。本体は共有ヘッダーを外しているので二重適用にならない。
    // kHalfResScale は半解像度で焼いた AO / 接触影へ svPosition.xy を落とす係数。
    // 強度は b8 が運ぶが、あれを組むのは IBL 解決後なのでここでは値だけ決める。
    constexpr float kHalfResScale = 0.5f;
    float screenAoStrength = 0.0f;
    float screenContactShadowStrength = 0.0f;

    if (screenSpaceReady && rs.IsGtaoActive() && gtaoBlur.IsValid()) {
        passCtx.screenAoTexture = gtaoBlur;
        // GTAO の出力は gtaoIntensity を織り込み済み。ここで再度掛けると二重になる。
        screenAoStrength = 1.0f;
    } else if (ssaoEnabled) {
        passCtx.screenAoTexture = ssaoBlur;
        screenAoStrength = rs.postProcess.ambientOcclusion.intensity;
    }
    if (screenSpaceReady && rs.contactShadow.enabled && contactShadowResult.IsValid()) {
        passCtx.screenContactShadowTexture = contactShadowResult;
        // 強度はマスク生成 CS 側で織り込み済み。ここは「適用するか」だけを決める。
        screenContactShadowStrength = 1.0f;
    }

    // ── ライト供給モードの決定と定数の組み立て ──────────────────────────────────
    // 4 つのパイプラインで「どのライトが効くか」を揃える。
    //   Forward   / Deferred   → LINEAR    (統合配列を全数走査)
    //   Forward+  / Deferred+  → CLUSTERED (クラスタで絞ってから走査)
    // どちらも同じ StructuredBuffer を読むので、"+" は性能の選択であって絵の選択ではない。
    // b3 のレガシー経路を既定から外したのは、点 8 / スポット 4 で打ち切るため
    // Forward を選んだだけで 9 個目の電球が黙って消えていたから。
    // LEGACY はフォールバックとして残る (リソース確保の失敗と、b9 / t29 を束縛しない
    // エディタのプレビュー経路)。
    // useGBufferOpaquePipeline はライトの供給方法と直交する軸なので触らない。
    const bool punctualBufferReady =
        passHandles.punctualLightBuffer.IsValid() && clusterCB.IsValid();
    // クラスタで絞れるか。カリング CS とインデックスバッファが揃って初めて成立する。
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
    // 霧のフレーム間状態は描画中のビューが持つ (SceneView / GameView で混ざらないように)。
    passCtx.froxelFogState    = &viewTargets.froxelState;
    passCtx.clusterDebugHeatmap = clusteredEnabled && rs.clustered.debugHeatmap;

    if (punctualBufferReady) {
        // ライト配列は毎フレーム転送する。上限 256 本 × 96B = 24KB で、部分更新の価値はない。
        if (!passCtx.punctualLights.empty()) {
            resources.Update(passHandles.punctualLightBuffer, passCtx.punctualLights.data(),
                             passCtx.punctualLights.size() * sizeof(PunctualLightGPU));
        }

        // 指数分割の係数。slice = log(viewZ) * scale + bias が [0, GridZ) に収まるよう決める。
        //   slice(nearZ) = 0 / slice(clusterFar) = GridZ
        // 対数なのは、等間隔だと手前の 1 スライスが広くなりすぎて何も落とせないため。
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

        // 別視点パス用。Legacy を Linear へ持ち上げると空のバッファを全数走査するので落とす。
        // ヒートマップも切る (メインカメラのタイル分布を別視点で塗っても意味がない)。
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
    passCtx.environmentResources = &sEnvironmentResources; // 空連動 IBL の永続状態 (フレームをまたぐ)
    // 雲シャドウ (Phase C): SkyRenderer から読んだ params + 現在時刻を影パスへ渡す。
    passCtx.cloudShadowStrength = skyCloudShadowStrength;
    passCtx.cloudShadowCoverage = skyCloudShadowCoverage;
    passCtx.cloudShadowScale    = skyCloudShadowScale;
    passCtx.cloudShadowSpeed    = skyCloudShadowSpeed;
    passCtx.cloudShadowTime     = Time::time;
    // Shadow トグルを CB まで伝える。描画を止めるだけだと、シェーダーは影を切っても
    // PCF ループ (既定 7x7 = 49 タップ) を回し続け、「切っても速くならない」状態になる。
    passCtx.shadowStrength =
        (rs.shadowEnabled && dirCastShadows) ? dirShadowStrength : 0.0f;
    // 単一カスケード相当のバイアス。カスケードごとの値は ShadowCascade::biasNDC が持つ。
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

    // レガシー経路向けに「大きさを持つ光源」を先頭から数本だけ写す。
    // 1 部屋に数個のものなので上限に当たること自体が稀。並び順が変わらない方が追いやすい。
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

    // ── 空連動 IBL: source=DynamicSky のとき空→動的 IBL を用意する ──
    // AdvancedGraphicsCB / 各 Lit パスより前に焼くことで同フレームで消費できる。
    // キャプチャ先と畳み込み出力は RenderGraph 管理外なのでグラフ実行前に直接呼ぶ。
    // SkyCapture / SkyLightBake は dirty を内部判定し、不要フレームは即 return する。
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

    // 局所 Reflection Probe はカメラが影響範囲内にいるとき、グローバル IBL より優先する。
    // IBL スロットを共有するので、マテリアル側に専用分岐も追加テクスチャも要らない。
    if (auto* localProbe = ExecuteReflectionProbeCapturePass(passCtx)) {
        passHandles.iblIrradiance = localProbe->runtimeIrradiance;
        passHandles.iblPrefilter  = localProbe->runtimePrefilter;
        dynamicIblReady = true;
        reflectionProbeIntensity = localProbe->intensity;
        dynamicIblMipCount = static_cast<int>(localProbe->runtimePrefilterMipCount);
    }

    // ---- AdvancedGraphicsCB (b8) を毎フレーム更新 ----
    // 各パスはここで書いたデータを読むだけなので、更新はこの 1 か所に集中させる。
    if (advancedGraphicsCB.IsValid()) {
        AdvancedGraphicsCB agData{};
        // 未バインド SRV をサンプルさせず、確実に ambient へフォールバックさせるための判定。
        // 動的 IBL は .dds を持たないので dynamicIblReady を別経路として許可する。
        const bool iblResourcesReady =
            (dynamicIblReady || (rs.ibl.enabled && rs.HasValidIblAssets())) &&
            passHandles.iblIrradiance.IsValid() && passHandles.iblPrefilter.IsValid();
        agData.iblIntensity          = iblResourcesReady ? rs.ibl.intensity * reflectionProbeIntensity : 0.0f;
        agData.iblDiffuseScale       = rs.ibl.diffuseScale;
        agData.iblSpecularScale      = rs.ibl.specularScale;
        // 動的 IBL は SkyLightBake が焼いた prefilter mip 数に合わせる (maxMip = mip 数 - 1)。
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

    // 前方描画のマテリアルが画面空間 AO / 接触影をどれだけ受けるか。
    // 値そのものは上の「前方描画のマテリアルへ渡す画面空間の遮蔽」ブロックで決めてある。
    agData.screenAoStrength            = screenAoStrength;
    agData.screenContactShadowStrength = screenContactShadowStrength;
    agData.screenAoScale               = kHalfResScale;
    agData.screenContactShadowScale    = kHalfResScale;

    // 自動露出。key <= 0 が「無効」の印なので、切ってあるときは 0 のまま渡す。
    // 0.18 は反射率 18% のグレーカード = 写真の露出計が基準にしている明るさ。
    agData.autoExposureKey          = rs.autoExposure.enabled ? 0.18f : 0.0f;
    agData.autoExposureCompensation = rs.autoExposure.compensation;
    agData.autoExposureMinEV        = rs.autoExposure.minExposureEV;
    agData.autoExposureMaxEV        = (std::max)(rs.autoExposure.maxExposureEV,
                                                 rs.autoExposure.minExposureEV);
        agData.taaFeedback           = rs.taa.feedback;
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
        // 天候 — シーンに置かれた WeatherComponent 1 個ぶん。無ければ 0 で素通りする。
        for (auto& weatherGo : scene.GameObjects()) {
            const auto* weather = weatherGo.GetComponent<WeatherComponent>();
            if (!weather || !weather->enabled) continue;
            agData.weatherWetness   = std::clamp(weather->wetness, 0.0f, 1.0f);
            agData.weatherDarkening = std::clamp(weather->darkening, 0.0f, 1.0f);
            agData.weatherPuddle    = std::clamp(weather->puddleAmount, 0.0f, 1.0f);
            break;
        }
        // 前フレームの VP 行列 — TAA / Motion Blur が深度再投影で使う。ビュー別に持つ。
        agData.prevViewProjection    = viewTargets.prevViewProjection;
        agData.invPrevViewProjection = viewTargets.invPrevViewProjection;
        resources.Update(advancedGraphicsCB, &agData, sizeof(AdvancedGraphicsCB));
        // ここはジッターを載せない。両方に載せるとジッター差分がそのまま「動き」として
        // 現れ、履歴が毎フレームずれて収束しない。履歴はピクセル中心で収束した絵なので、
        // 引く座標もピクセル中心でなければならない。
        viewTargets.prevViewProjection    = camera.GetViewProjection();
        viewTargets.invPrevViewProjection = math::Matrix4::Inverse(camera.GetViewProjection());
    } // end AdvancedGraphicsCB update

    // =========================================================================
    // RenderPipeline にパスを登録
    // =========================================================================
    RenderPipeline& pipeline = viewTargets.pipeline;
    pipeline.BeginBuild();
    // 申告を条件で組み立てるパス用。initializer_list には if を書けないので、
    // 読むものが構成で変わるパスは vector を渡す。
    using RA = renderer::RenderGraph::ResourceAccess;
    using RU = renderer::RenderGraph::ResourceUsage;
    // 申告を条件で組み立てるパス用。initializer_list には if を書けないので、
    // 読むものが構成で変わるパスは vector を渡す。
    profiler::Profiler::BeginSample(
        profiler::ProfilerMarker("RenderSystem::BuildPipeline", "Rendering"));
    // Output だけは出力先そのものなので実寸で申告する (中間 RT は内部解像度)。
    pipeline.DeclareResource("Output",     { renderer::RenderGraph::ResourceKind::RenderTarget, nativeW, nativeH, renderer::Format::RGBA16F, 1, true,  true,  false });
    pipeline.DeclareResource("ShadowMap",  { renderer::RenderGraph::ResourceKind::RenderTarget, rs.shadow.mapResolution, rs.shadow.mapResolution, renderer::Format::RGBA16F, 0, true, false, false });
    pipeline.DeclareResource("PunctualShadowMap", { renderer::RenderGraph::ResourceKind::RenderTarget, punctualShadowRes, punctualShadowRes, renderer::Format::RGBA16F, 0, true, false, false });
    pipeline.DeclareResource("LightCookieAtlas",  { renderer::RenderGraph::ResourceKind::RenderTarget, kLightCookieAtlasWidth, kLightCookieAtlasHeight, renderer::Format::RGBA16F, 1, true, false, false });
    pipeline.DeclareResource("HDR",        { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, renderer::Format::RGBA16F, 1, true,  false, true });
    pipeline.DeclareResource("LDR",        { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, renderer::Format::RGBA16F, 1, false, false, true });
    pipeline.DeclareResource("SelectionMask", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, renderer::Format::RGBA16F, 1, true,  false, true });
    pipeline.DeclareResource("Outline",    { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, renderer::Format::RGBA16F, 1, false, false, true });
    pipeline.DeclareResource("ObjectMask", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, renderer::Format::RGBA16F, 1, true,  false, true });
    pipeline.DeclareResource("Velocity",   { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, renderer::Format::RGBA16F, 1, true,  false, true });
    pipeline.DeclareResource("CustomPostProcess0", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, renderer::Format::RGBA16F, 1, false, false, true });
    pipeline.DeclareResource("CustomPostProcess1", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, renderer::Format::RGBA16F, 1, false, false, true });
    // 実体は upscaleSrcRT。等倍のフレームは誰も触らないので申告もしない。
    const bool upscaleActive = needsUpscale && upscaleSrcRT.IsValid();
    if (upscaleActive)
        pipeline.DeclareResource("UpscaleSrc", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, renderer::Format::RGBA16F, 1, false, false, true });
    pipeline.DeclareResource("Bloom",      { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, renderer::Format::RGBA16F, 1, false, false, true });
    pipeline.DeclareResource("SSRResult", { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, renderer::Format::RGBA16F, 1, false, false, true });
    pipeline.DeclareResource("MotionBlurResult", { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, renderer::Format::RGBA16F, 1, false, false, true });
    pipeline.DeclareResource("VolumetricResult", { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, renderer::Format::RGBA16F, 1, false, false, true });
    // Forward もプリパスで GBuffer へ描くので、ここを Deferred 限定にすると
    // 「宣言されていないリソース」への書き込みになり RenderGraph の検証が落ちる。
    if (screenSpaceReady)
        pipeline.DeclareResource("GBuffer", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, renderer::Format::RGBA16F, 2, true, false, false });
    // AO と接触影は半解像度で持つ (実体は curW/2 x curH/2)。ここをフル解像度で
    // 申告していると、エイリアシングが全画面 RT と同じ枠を貸してしまう。
    const uint32_t halfW = (std::max)(1u, sHdrW / 2);
    const uint32_t halfH = (std::max)(1u, sHdrH / 2);
    pipeline.DeclareResource("LensFlareSource", { renderer::RenderGraph::ResourceKind::Texture, halfW, halfH, renderer::Format::RGBA16F, 1, false, false, true });
    if (ssaoEnabled)
        pipeline.DeclareResource("SSAO",               { renderer::RenderGraph::ResourceKind::Texture, halfW, halfH, renderer::Format::RGBA16F, 1, false, false, true });
    // GTAO / ContactShadows は GBuffer を読んで独自の UAV へ書く。専用名で宣言しないと
    // GBuffer への偽書き込みとみなされ、DeferredLighting との依存順が崩れる。
    if (screenSpaceReady && rs.IsGtaoActive())
        pipeline.DeclareResource("GTAOResult",          { renderer::RenderGraph::ResourceKind::Texture, halfW, halfH, renderer::Format::RGBA16F, 1, false, false, true });
    if (screenSpaceReady && rs.contactShadow.enabled)
        pipeline.DeclareResource("ContactShadowResult", { renderer::RenderGraph::ResourceKind::Texture, halfW, halfH, renderer::Format::RGBA16F, 1, false, false, true });
    pipeline.SetOutputs({ "Output" });

    // IBL BRDF LUT 焼き付け。512x512 の積分テーブルはシーンにも設定にも依存しない定数なので、
    // 毎フレーム呼ぶが実処理は世代追跡で初回のみ走る。
    pipeline.AddRawPass("IBLBrdfBake", {}, {}, [&]() {
        ExecuteIBLBakeBrdfLutPass(passCtx);
    }, false); // 外部 ComputeTexture への副作用パスなので RenderGraph カリング禁止

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

    // HDR の段 (AfterOpaque / SceneHDR) のカスタムパスを積む。どちらも hdrRT を
    // 読んで書くので、違うのは «パイプラインのどこへ挿すか» だけ。
    //
    // WHY マスクを reads に入れるか: 入れないとグラフは «マスクを描く前に» この
    //     パスを走らせてよいことになる。読む宣言をしていない効果でも、同じ段に
    //     読む効果が混ざれば順序は共有されるので、有無で分けずまとめて宣言する。
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

    // ── Skinning (コンピュート) ───────────────────────────────────────────────
    // Shadow より前。変形結果をシャドウ・GBuffer・Forward が共有するので 1 回で済む。
    // 出力は論理リソースではなく SkinnedMeshRenderer の頂点バッファなので依存には乗せない。
    pipeline.AddRawPass("SkinningCompute", {}, {}, [&]() {
        ExecuteSkinningComputePass(passCtx);
    }, false);

    // ── クラスタライトカリング ────────────────────────────────────────────────
    // Shadow より前。どちらの経路も同じ結果を読むので 1 回で済む。
    // 出力は StructuredBuffer で論理リソースではないため reads/writes は空。
    if (clusteredEnabled) {
        pipeline.AddRawPass("ClusterLightCull", {}, {}, [&]() {
            ExecuteClusterLightCullPass(passCtx);
        }, false);
    }

    // ── Light Cookie ──────────────────────────────────────────────────────────
    // Cookie の顔ぶれが変わったフレームだけアトラスを焼き直す。
    pipeline.AddRawPass("LightCookie", {}, { "LightCookieAtlas" }, [&]() {
        ExecuteLightCookiePass(passCtx);
    });

    // ── Shadow ────────────────────────────────────────────────────────────────
    // Directional の CSM と Spot / Point のアトラスを 1 パスで描く。caster の収集と
    // ソートを両者で共有するため、パスを分けるとシーン走査が丸ごと 2 回になる。
    pipeline.AddRawPass("Shadow", {}, { "ShadowMap", "PunctualShadowMap" }, [&]() {
        ExecuteShadowPass(passCtx);
    });

    // ── Forward or Deferred ───────────────────────────────────────────────────
    if (!useGBufferOpaquePipeline) {
        // 画面空間系のための GBuffer プリパス。ライティングはせず法線・深度・roughness だけ書く。
        // 以降の SSAO / GTAO / SSR / 接触影は Deferred と同じ入力を読む。
        if (forwardGBufferPrepass) {
            pipeline.AddRawPass("ForwardGBufferPrepass",
                                { "ShadowMap", "PunctualShadowMap", "LightCookieAtlas" },
                                { "GBuffer" }, [&]() {
                ExecuteGBufferPass(passCtx);
            });

            // 地形も GBuffer へ入れる。飛ばすと地形が AO の遮蔽者にも受け手にもならず、
            // 「Deferred では地形に AO が乗るのに Forward では乗らない」差が残る。
            // WHY 独立したパスにするか: 以前はプリパスの «中で» 入れ子実行していた。
            //     その経路では Setup が呼ばれず、申告はホスト側のラムダが代理していた。
            //     登録順を直後に置けば、実行順はこれまでと同じになる。
            pipeline.AddPass<TerrainRenderPass>(TerrainDrawMode::GBuffer);

            // AO と接触影は ForwardOpaque より前。Forward には合流点が無く各マテリアルが
            // 自分の画素で読むので、本描画の時点で結果が揃っていないと何も掛からない。
            if (rs.IsGtaoActive()) {
                pipeline.AddRawPass("GTAO", { "GBuffer" }, { "GTAOResult" }, [&]() {
                    ExecuteGTAOPass(passCtx);
                });
            }
            if (rs.contactShadow.enabled) {
                // WHY HDR を申告しないか: ContactShadowsPass は gbufferDepthReady が false の
            //     ときだけ HDR の深度へ落ちるが、この登録は 2 箇所とも «GBuffer が揃う»
            //     分岐の中にある。申告すると本描画前の HDR へ偽の依存が張られる。
            pipeline.AddRawPass("ContactShadows", { "GBuffer" }, { "ContactShadowResult" }, [&]() {
                    ExecuteContactShadowsPass(passCtx);
                });
            }
            if (ssaoEnabled) {
                pipeline.AddRawPass("SSAO", { "GBuffer" }, { "SSAO" }, [&]() {
                    ExecuteSSAOPass(passCtx);
                });
            }
        }

        // ForwardOpaque の reads は AO / 接触影の有無で変わる。
        // 宣言しておかないとグラフが AO より先に本描画を並べうる。
        {
            // HDR は Write。ReadWrite にすると読み手にもなるが、この時点で producer が
            // いないため検証が落ちる。ForwardOpaque は自分でクリアしてから描く。
            std::vector<RA> forwardAccesses = {
                { "ShadowMap",         RU::Read  },
                { "PunctualShadowMap", RU::Read  },
                { "LightCookieAtlas",  RU::Read  },
                { "HDR",               RU::Write },
            };
            if (forwardGBufferPrepass) {
                if (ssaoEnabled)              forwardAccesses.push_back({ "SSAO",                RU::Read });
                if (rs.IsGtaoActive())        forwardAccesses.push_back({ "GTAOResult",          RU::Read });
                if (rs.contactShadow.enabled) forwardAccesses.push_back({ "ContactShadowResult", RU::Read });
            }
            pipeline.AddRawPass("ForwardOpaque", std::move(forwardAccesses), [&]() {
                ExecuteForwardPasses(passCtx);
            });
        }
    }

    if (useGBufferOpaquePipeline) {
        pipeline.AddRawPass("DeferredGBuffer", {}, { "GBuffer" }, [&]() {
            ExecuteGBufferPass(passCtx);
        });

        // Deferred Terrain — GBuffer へ書く。DepthCopy / AO / Lighting より前に置くことで
        // GTAO/SSAO/ContactShadows/SSR/IBL が地形へも効く。
        pipeline.AddPass<TerrainRenderPass>(TerrainDrawMode::GBuffer);

        pipeline.AddRawPass("DeferredDepthCopy", { "GBuffer" }, { "HDR" }, [&]() {
            ExecuteDeferredDepthCopyPass(passCtx);
        });
    }

    // ── Terrain (Forward フォールバック用) ────────────────────────────────────
    // ForwardOpaque / Sky の間に HDR RT (depth 共有) へ描く。Sky より前なので空が被らない。
    // 通常は上の GBuffer フェーズで描画済みなのでここは通らない。
    if (!useGBufferOpaquePipeline) {
        pipeline.AddPass<TerrainRenderPass>(TerrainDrawMode::Forward);
    }

    // Sky / SunMoon — Forward フォールバックではここ (不透明描画後・雲前)。
    // GBuffer 経路では Terrain が HDR を書かないので Sky と DeferredDepthCopy の順序保証が
    // 失われ、DepthCopy のクリアで空が消える。だから Lighting 後 (下のブロック) に描く。
    if (!useGBufferOpaquePipeline) {
        pipeline.AddRawPass("Sky", { "HDR" }, { "HDR" }, [&]() {
            ExecuteSkyPass(passCtx);
        });
        pipeline.AddRawPass("SunMoon", { "HDR" }, { "HDR" }, [&]() {
            ExecuteSunMoonPass(passCtx);
        });

        // VolumetricCloud — GBuffer フォールバックの Forward では Sky 後・透明物前に HDR へ合成する。
        // WHY: 空を背景にしつつ、後続の水面・透明エフェクトで上書きできる順序にする。
        pipeline.AddRawPass("VolumetricCloud", { "HDR" }, { "HDR" }, [&]() {
            ExecuteVolumetricCloudPass(passCtx);
        });

        // SSR — Forward でもプリパスの GBuffer から反射を計算する。
        // 映すのはライティング済みのシーンなので HDR が出揃った後に置く。
        if (forwardGBufferPrepass && rs.ssr.enabled) {
            pipeline.AddRawPass("SSR", { "GBuffer", "HDR" }, { "SSRResult", "HDR" }, [&]() {
                ExecuteSSRPass(passCtx);
            });
        }
    }

    // ── SSAO + Deferred Lighting ──────────────────────────────────────────────
    if (useGBufferOpaquePipeline) {
        // GTAO — DeferredLighting より前に GBuffer から AO を計算する。
        // "GTAOResult" として宣言することで DeferredLighting が正確な依存で待てる。
        if (rs.IsGtaoActive()) {
            pipeline.AddRawPass("GTAO", { "GBuffer" }, { "GTAOResult" }, [&]() {
                ExecuteGTAOPass(passCtx);
            });
        }
        // ContactShadows — DeferredLighting より前に深度から接触影マスクを生成する。
        // WHY: GTAO と同様に ContactShadowResult として宣言し偽依存を除去する。
        if (rs.contactShadow.enabled) {
            // WHY HDR を申告しないか: ContactShadowsPass は gbufferDepthReady が false の
            //     ときだけ HDR の深度へ落ちるが、この登録は 2 箇所とも «GBuffer が揃う»
            //     分岐の中にある。申告すると本描画前の HDR へ偽の依存が張られる。
            pipeline.AddRawPass("ContactShadows", { "GBuffer" }, { "ContactShadowResult" }, [&]() {
                ExecuteContactShadowsPass(passCtx);
            });
        }
        if (ssaoEnabled) {
            pipeline.AddRawPass("SSAO", { "GBuffer" }, { "SSAO" }, [&]() {
                ExecuteSSAOPass(passCtx);
            });
        }
        // DeferredLighting の reads を動的に構築し、有効な AO の出力だけへ依存を張る。
        // 静的に書くと有効/無効の組み合わせごとに分岐が要る。
        {
            std::vector<RA> deferredAccesses = {
                { "GBuffer", RU::Read     },
                { "HDR",     RU::ReadWrite }, // 深度を読み、ライティング結果を書く
                // 影と Cookie はライティングの本体が読む (t13 / t28 / t31)。
                // 申告が抜けていたので «Shadow / LightCookie の後» という依存が張られず、
                // 登録順が偶然そうなっているだけの状態だった。
                { "ShadowMap",         RU::Read },
                { "PunctualShadowMap", RU::Read },
                { "LightCookieAtlas",  RU::Read },
            };
            if (ssaoEnabled)              deferredAccesses.push_back({ "SSAO",               RU::Read });
            if (rs.IsGtaoActive())        deferredAccesses.push_back({ "GTAOResult",          RU::Read });
            if (rs.contactShadow.enabled) deferredAccesses.push_back({ "ContactShadowResult", RU::Read });
            pipeline.AddRawPass("DeferredLighting", std::move(deferredAccesses), [&]() {
                ExecuteDeferredLightingPass(passCtx);
            });
        }

        // Sky / SunMoon — GBuffer ライティング後に HDR へ描く。深度==1.0 の画素だけを埋め、
        // HDR 依存チェーンで DeferredDepthCopy のクリアより確実に後段になる。
        pipeline.AddRawPass("Sky", { "HDR" }, { "HDR" }, [&]() {
            ExecuteSkyPass(passCtx);
        });
        pipeline.AddRawPass("SunMoon", { "HDR" }, { "HDR" }, [&]() {
            ExecuteSunMoonPass(passCtx);
        });

        // VolumetricCloud — GBuffer Lighting / Sky 後・透明物前に HDR へ合成する。
        // WHY: Lighting・空に上書きされず、透明物や水面を雲の手前に描ける順序にする。
        pipeline.AddRawPass("VolumetricCloud", { "HDR" }, { "HDR" }, [&]() {
            ExecuteVolumetricCloudPass(passCtx);
        });

        // Deferred の中で «前方描画される» 2 パス。どちらも BindForwardShadingResources を
        // 通るので、Forward パスと同じく Spot/Point の影 (t28) と Cookie (t31) を引く。
        // 申告しないと依存辺が張られず、Shadow / LightCookie より先に走ってよいことになる。
        pipeline.AddRawPass("DeferredSkinnedForward",
                            { "HDR", "ShadowMap", "PunctualShadowMap", "LightCookieAtlas" },
                            { "HDR" }, [&]() {
            ExecuteDeferredSkinnedForwardPass(passCtx);
        });

        pipeline.AddRawPass("DeferredForwardTransparent",
                            { "HDR", "ShadowMap", "PunctualShadowMap", "LightCookieAtlas" },
                            { "HDR" }, [&]() {
            ExecuteDeferredForwardTransparentPass(passCtx);
        });

        // SSR — 透明オブジェクト通過後の深度を使うので DeferredForwardTransparent の後。
        // 実行条件はパイプラインの選択ではなく GBuffer の有無。
        if (rs.ssr.enabled) {
            pipeline.AddRawPass("SSR", { "GBuffer", "HDR" }, { "SSRResult", "HDR" }, [&]() {
                ExecuteSSRPass(passCtx);
            });
        }
    }

    // VolumetricLight — ゴッドレイ・光柱を HDR バッファへ加算合成する。
    //
    // WHY 半透明より «前» か: レイは不透明深度で止まる。水や半透明は深度を書かないので、
    //     レイの終端は水底のジオメトリになる。それを水面の描画より «後» に足すと、
    //     水底までの光芒がまるごと水面の手前へ描かれ、水が光の靄で塗り潰される。
    //     不透明深度が確定したこの位置で足しておけば、水面・トレイル・パーティクルが
    //     光芒の上へ順番に乗り、遮蔽も屈折も普通の透明描画として処理される。
    //     WaterCaustics が「Water の前でなければならない」のと同じ理由。
    if (rs.volumetricLight.enabled) {
        pipeline.AddRawPass("VolumetricLight", { "HDR", "ShadowMap" }, { "VolumetricResult", "HDR" }, [&]() {
            ExecuteVolumetricLightPass(passCtx);
        });
    }

    // WaterCaustics — 水面下の不透明ジオメトリへコースティクスを投影してから、水面本体を透明描画する。
    // WHY: Water の後に加算すると水面そのものへ模様が乗りやすいため、深度が不透明物だけを指す段階で実行する。
    pipeline.AddRawPass("WaterCaustics", { "HDR" }, { "HDR" }, [&]() {
        ExecuteCausticsPass(passCtx);
    });

    pipeline.AddPass<WaterRenderPass>();

    for (EntityID id : scene.GetEntities<ScriptComponent>()) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        if (!sc || !go)
            continue;
        for (auto& entry : sc->scripts) {
            if (!entry.script || !entry.script->enabled)
                continue;
            entry.script->SetContext(&scene, go);
            entry.script->ExecuteCallback(&Script::OnSetupRenderPasses, pipeline, passCtx);
        }
    }

    appendQueuedUserPasses(UserRenderPassInjectionPoint::AfterOpaque);

    // オブジェクトマスク。描くのはジオメトリなので不透明が出揃ったここで済ませる。
    //
    // WHY 半透明より前か: マスクの中身は «不透明の形と、その時点の深度» で決まる。
    //     半透明は深度を書かないので待っても結果は変わらない。一方でここより後ろへ
    //     置くと、AfterOpaque 段のカスタムパスがマスクを読めなくなる
    //     (グラフ上「まだ描かれていないもの」を読む宣言になり、順序が閉じない)。
    if (objectMaskEnabled) {
        pipeline.AddRawPass("ObjectMask", { "HDR" }, { "ObjectMask" }, [&]() {
            ExecuteObjectMaskPass(passCtx);
        });
    }

    // ── AfterOpaque 段のユーザーシェーダー ────────────────────────────────────
    // WHY ここか: 背景だけが描かれていて、デカール・トレイル・パーティクル・半透明は
    //     まだ乗っていない。画面を歪める効果をこの後 (SceneHDR) に置くと、既に
    //     描かれたパーティクルごと曲がって «エフェクトだけ別の場所に居る» 絵になる。
    appendCustomHdrPasses("CustomAfterOpaque", customAfterOpaqueIndices);

    // ── デカール用深度スナップショット ────────────────────────────────────────
    // 深度専用 RT (colorCount = 0)。カラーを持つ RT と貸し回してはいけない。
    pipeline.DeclareResource("DecalDepth", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, renderer::Format::RGBA16F, 0, true, false, true });
    pipeline.AddRawPass("DecalDepthCopy", { useGBufferOpaquePipeline ? "GBuffer" : "HDR" }, { "DecalDepth" }, [&]() {
        ExecuteDecalDepthCopyPass(passCtx);
    });

    // ── Decal + Trail + Particle ──────────────────────────────────────────────
    pipeline.AddRawPass("Decal", { "HDR", "DecalDepth" }, { "HDR" }, [&]() {
        ExecuteDecalPass(passCtx);
    });

    pipeline.AddPass<MeshTrailRenderPass>();
    pipeline.AddPass<TrailRenderPass>();

    // ShadowMap は粒子の自己影が読む (t8)。申告していないと影より前に走れてしまう。
    // WHY Particle より前に登録するか: 粒子は同じフレームの霧を読んで «自分の奥行きの霧» を逆算する
    //     (ParticleLighting.hlsli の ApplyParticleFog)。依存を申告しあわない 2 つのパスは登録順に並ぶ。
    pipeline.AddRawPass("FroxelFog", { "ShadowMap", "PunctualShadowMap", "LightCookieAtlas" }, {}, [&]() {
        ExecuteFroxelFogPass(passCtx);
    }, false);

    // PunctualShadowMap / LightCookieAtlas は «点光源を受ける» .mat の粒子が読む (ParticleLighting.hlsli)。
    pipeline.AddRawPass("Particle", { "HDR", "DecalDepth", "ShadowMap", "PunctualShadowMap", "LightCookieAtlas" },
                        { "HDR" }, [&]() {
        ExecuteParticlePass(passCtx);
    });

    // Overdraw 可視化は診断表示。有効なときだけ Particle の直後に HDR を上書きする。
    // GPU 時間を Particle パスの実測値と混ぜないよう、別パスとして計測させる。
    if (rs.particleOverdrawView) {
        pipeline.AddRawPass("ParticleOverdraw", { "HDR" }, { "HDR" }, [&]() {
            ExecuteParticleOverdrawPass(passCtx);
        });
    }

    // TAA の反応マスク。HDR へは書かないが、Particle の後・Composite (→ TAA) の前に並べるために
    // HDR の書き手として申告する (Overdraw と同じ申告の仕方)。
    if (rs.IsTaaActive()) {
        pipeline.AddRawPass("ParticleReactive", { "HDR", "DecalDepth" }, { "HDR" }, [&]() {
            ExecuteParticleReactivePass(passCtx);
        });
    }

    appendQueuedUserPasses(UserRenderPassInjectionPoint::AfterTransparent);

    // ── Selection / Debug ─────────────────────────────────────────────────────
    if (selectionOutlineEnabled) {
        pipeline.AddRawPass("SelectionMask", { "HDR" }, { "SelectionMask" },
                            [&](PassResources& res) {
            ExecuteSelectionMaskPass(res, passCtx);
        });
    }

    // ── SceneHDR 段のユーザーシェーダー ───────────────────────────────────────
    // WHY ここか: 絵が出揃っていて、まだブルームにも露出にも触れていない唯一の場所。
    //     Bloom より後ろへ置くと «光っているのに滲まない»、AutoExposure より後ろへ
    //     置くと «明るくしたのに露出が反応しない» という、段を選べる意味が消える
    //     並びになる。デバッグ描画より前なのは、ギズモを効果で歪ませないため。
    appendCustomHdrPasses("CustomSceneHDR", customSceneHdrIndices);

    // 自動露出はデバッグ描画より前。
    //
    // WHY ここか: 測るのは «シーンの明るさ» で、グリッド・ギズモ・コライダー・NavMesh は
    //     Scene View にしか無い。後ろへ置くと Scene View だけ画面の何割かをグリッドの色に
    //     占められ、同じシーンなのに Game View と露出が食い違う。RenderGraph は
    //     «登録順より前の書き手» を読み手の世代とするので、ここへ置けばデバッグ描画が
    //     乗る前の HDR を測る。
    //
    // WHY MotionBlur / LensFlare より前になるか: どちらも HDR を書き換えるが、
    //     フレアを測光へ入れると «明るい → 露出が下がる → フレアが弱る» の輪ができる。
    //     測るのは素のシーンでよい。Bloom は HDR を書かないので位置に関わらず同じ。
    //
    // 出力は StructuredBuffer (Composite が t29 で読む) で、グラフの論理リソースに
    // 乗らない。FroxelFog と同じ理由でカリング対象から外す。
    if (rs.autoExposure.enabled) {
        pipeline.AddRawPass("AutoExposure", { "HDR" }, {}, [&]() {
            ExecuteAutoExposurePass(passCtx);
        }, false);
    }

    pipeline.AddPass<ConstraintDebugPass>();
    pipeline.AddPass<RagdollDebugPass>();
    pipeline.AddPass<AnimatorDebugPass>();
    pipeline.AddPass<GridDebugPass>();
    pipeline.AddPass<LightRangeDebugPass>();
    pipeline.AddPass<VFXGizmoDebugPass>();
    pipeline.AddPass<TerrainCollisionDebugPass>();

    pipeline.AddRawPass("ScriptDebugDraw", { "HDR" }, { "HDR" }, [&]() {
        ExecuteScriptDebugDrawPass(passCtx);
    });

    // コライダーは Script の Gizmo より後。どちらも深度オフの 1px ラインなので、同じ形が
    // 重なるとサブピクセルの被り方でちらつく。コライダーを破線にして最後に描けば両方読める
    // (DebugCollidersPass の DASH_* を参照)。
    pipeline.AddPass<DebugCollidersPass>();

    pipeline.AddPass<NavMeshDebugPass>();
    pipeline.AddPass<DecalDebugPass>();

    appendQueuedUserPasses(UserRenderPassInjectionPoint::BeforePostProcess);

    // ── モーションベクター ────────────────────────────────────────────────────
    // TAA とモーションブラーは深度再投影だけでは「カメラの動き」しか復元できない。
    // 不透明ジオメトリの実際の移動量を専用 RT へ描いて両者へ供給する。
    // 消費側が 1 つも無いフレームは丸ごと省く (不透明をもう一度ラスタライズするため)。
    const bool velocityNeeded =
        velocityRT.IsValid() && (rs.motionBlur.enabled || rs.IsTaaActive());
    if (velocityNeeded) {
        pipeline.AddRawPass("Velocity", {}, { "Velocity" }, [&]() {
            ExecuteVelocityPass(passCtx);
        });
    }

    // ── PostProcess チェーン ──────────────────────────────────────────────────
    // MotionBlur CS — HDR 空間で計算し motionBlurResult へ書く (Composite が hdrRT の代わりに読む)。
    // Bloom の前に走らせるので blur 後の輝度が Bloom に乗る。
    if (rs.motionBlur.enabled) {
        // Velocity は誰も書かないフレームがある。書かれないものを読むと申告した瞬間に
        // «producer が居ない» で Plan が落ちるので、要るときだけ足す。
        std::vector<RA> motionBlurAccesses = { { "MotionBlurResult", RU::Write }, { "HDR", RU::ReadWrite } };
        if (velocityNeeded) motionBlurAccesses.push_back({ "Velocity", RU::Read });
        pipeline.AddRawPass("MotionBlur", std::move(motionBlurAccesses), [&]() {
            ExecuteMotionBlurPass(passCtx);
        });
    }
    // LensFlare PS — 輝度抽出した光源を ADDITIVE で HDR へ合成する。
    // Bloom の前に置くのでフレアも Bloom に乗るが、その順序では bloomHalf に今フレームの
    // 輝点がまだ無い。パス自身が bloomHalf へ焼いてから読む (LensFlareSource がこの出力)。
    if (rs.lensFlare.enabled) {
        pipeline.AddRawPass("LensFlare", { "HDR" }, { "HDR", "LensFlareSource" }, [&]() {
            ExecuteLensFlarePass(passCtx);
        });
    }
    if (rs.postProcess.bloom.enabled) {
        pipeline.AddRawPass("Bloom", { "HDR" }, { "Bloom" }, [&]() {
            ExecuteBloomPass(passCtx);
        });
    }

    // フロクセル霧。シャドウマップを読むので Shadow より後、Composite より前。
    // 霧はライトとシャドウだけから作るので HDR の完成を待つ必要はない。
    // 無効でも積むのは、パス側が b13 へ「無効」を書き戻さないと前フレームの定数が残り
    // 画面が真っ黒になるため (FroxelFogPass 参照)。
    // 出力先の 3D ボリュームは論理リソースに乗らないので、writes が空でもカリングさせない。

    const bool customPostProcessEnabled =
        !customPostProcessIndices.empty() &&
        customPostProcessRT[0].IsValid() &&
        customPostProcessRT[1].IsValid();
    // hasPostCompositeEffects: Composite の出力先が "LDR" かチェーン終端かを決める。
    // WHY: このフラグが true なら Composite は ldrRT に書き、後続エフェクトがチェーンを形成する。
    const bool hasPostCompositeEffects =
        rs.IsTaaActive() || customPostProcessEnabled || selectionOutlineEnabled || rs.postProcess.fxaaEnabled;

    // ここが «Composite がどこへ書くか» の唯一の正本。グラフへの申告 (下の writes) と
    // パスが実際に束縛するハンドルを、同じ 1 つの判定から配る。
    passCtx.compositeOutputRT = hasPostCompositeEffects ? ldrRT : passCtx.chainOutputRT;

    // LDR チェーンの終端リソース。passCtx.chainOutputRT の «グラフ側の名前» で、
    // 実寸へ引き伸ばすのは UpscalePass だけという対応を保つ。
    const char* const chainOutRes = upscaleActive ? "UpscaleSrc" : "Output";

    {
        // Bloom を読むのは bloom.enabled のときだけ (CompositePass の bloomWritten と同条件)。
        std::vector<RA> compositeAccesses = {
            { "HDR", RU::Read },
            { hasPostCompositeEffects ? "LDR" : chainOutRes, RU::Write },
        };
        if (rs.postProcess.bloom.enabled) compositeAccesses.push_back({ "Bloom", RU::Read });
        pipeline.AddRawPass("Composite", std::move(compositeAccesses), [&]() {
            ExecuteCompositePass(passCtx);
        });
    }

    // ---- Post-composite チェーン ----
    // ppCurrent は「LDR 空間の最新フレームを持つリソース名」。これを進めるだけで
    // TAA/CustomPP/SelectionOutline/FXAA の任意の組み合わせが 1 本の直列チェーンになる。
    std::string ppCurrent  = hasPostCompositeEffects ? "LDR" : chainOutRes;
    int         ppPingPong = 0; // customPostProcessRT の ping-pong インデックス

    // TAA — 最初に適用することで後続の CustomPP/SelectionOutline が TAA 済み映像に乗る。
    // 登録順が RenderGraph のタイブレークになる (Kahn's algorithm)。
    if (rs.IsTaaActive()) {
        const auto taaBody = [&]() {
            ExecuteTAAPass(passCtx);
            // taaFlip は ExecuteTAAPass 内で反転済み — 反転後のフラグで「書いた方」を特定する。
            auto& taaOut = passHandles.taaFlip ? passHandles.taaHistoryB : passHandles.taaHistoryA;
            passHandles.fxaaInput = resources.GetColorTexture(taaOut, 0);
            // TAA 後は履歴バッファが最新フレーム。更新しないと後続が TAA 前の ldrRT を読む。
            passHandles.postProcessInput = passHandles.fxaaInput;
            // LDR の論理的な最新世代は履歴 RT に移る。診断も後続パスも同じ実体を引く。
            passCtx.resourceRegistry.BindTarget("LDR", taaOut);
        };
        // MotionBlur と同じ理由で Velocity は要るときだけ足す。
        std::vector<RA> taaAccesses = { { ppCurrent, RU::ReadWrite } };
        if (velocityNeeded) taaAccesses.push_back({ "Velocity", RU::Read });
        pipeline.AddRawPass("TAA", std::move(taaAccesses), taaBody);
    }

    // Custom PostProcess チェーン
    for (uint32_t i = 0; i < static_cast<uint32_t>(customPostProcessIndices.size()); ++i) {
        const uint32_t customIndex  = customPostProcessIndices[i];
        const bool     isLastEffect = (i + 1 == static_cast<uint32_t>(customPostProcessIndices.size()))
                                       && !selectionOutlineEnabled
                                       && !rs.postProcess.fxaaEnabled;
        // outputIndex == 2 → ExecuteCustomPostProcessPass が ctx.outputRT に直書きする規約
        const uint32_t    outputIndex = isLastEffect ? 2u : static_cast<uint32_t>(ppPingPong % 2);
        const std::string outRes      = isLastEffect
            ? chainOutRes
            : ("CustomPostProcess" + std::to_string(outputIndex));
        const auto customBody = [&, customIndex, outputIndex]() {
            ExecuteCustomPostProcessPass(passCtx, customIndex, outputIndex);
        };
        // 輪郭マスクはユーザーシェーダーの入力になりうる。読むと宣言しないと、
        // グラフはマスクを描く前にこのパスを走らせてよいことになる (reads が
        // initializer_list なので MotionBlur と同じく 2 通りに分ける)。
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

    // SelectionOutline
    if (selectionOutlineEnabled) {
        const bool        isLastEffect = !rs.postProcess.fxaaEnabled;
        const std::string outRes       = isLastEffect ? chainOutRes : "Outline";
        // HDR は輪郭の深度比較が読む (t7)。
        pipeline.AddRawPass("SelectionOutline",
            { ppCurrent, "SelectionMask", "HDR" },
            { outRes },
            [&](PassResources& res) { ExecuteSelectionOutlinePass(res, passCtx); });
        ppCurrent = outRes;
    }

    // FXAA
    if (rs.postProcess.fxaaEnabled) {
        pipeline.AddRawPass("FXAA", { ppCurrent }, { chainOutRes }, [&]() { ExecuteFxaaPass(passCtx); });
        ppCurrent = chainOutRes;
    }

    // TAA_Blit — TAA は ping-pong 履歴にしか書かないので、後続エフェクトが 1 つも無いときは
    // ppCurrent が "LDR" のまま残る。ここでチェーン終端へ届ける。
    if (ppCurrent != chainOutRes) {
        pipeline.AddRawPass("TAA_Blit", { ppCurrent }, { chainOutRes }, [&]() {
            ExecuteTAABlitPass(passCtx);
        });
        ppCurrent = chainOutRes;
    }

    // Upscale — 内部解像度で仕上がった絵を出力先の実寸へ解像する。
    // UI より «前» に置くのが要点。後ろに回すと UI まで引き伸ばされて滲む。
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
            if (!sc || !go)
                continue;
            for (auto& entry : sc->scripts) {
                if (!entry.script || !entry.script->enabled)
                    continue;
                entry.script->SetContext(&scene, go);
                entry.script->ExecuteCallback(&Script::OnPreRender, "OnPreRender");
            }
        }
    }

    // =========================================================================
    // RenderPipeline 実行 + デバッグスナップショット更新
    // =========================================================================

    // GPU Timestamp Query の前フレーム結果を収集してからフレームを開始する。
    // WHY: GpuProfCollect を先に呼ぶことで前フレームの非同期クエリが確定している可能性を最大化する。
    {
        FBZZ_PROFILE_SCOPE("RenderSystem::GpuProfilerSetup");
        renderer.GpuProfCollect();
        renderer.GpuProfBeginFrame();

        // GPU フックを RenderPipeline に設定する。CPU フックとは独立しているため、
        // Profiler の CPU スコープ計測と干渉しない。
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
    // パスが書き換えたフレームをまたぐ状態をビューへ戻す。
    // TAA は反転させた向き (次フレームは «書いた方» を履歴として読む)、
    // 自動露出は消費したリセット世代。
    viewTargets.taaFlip                 = passHandles.taaFlip;
    viewTargets.exposureResetGeneration = passHandles.exposureResetGeneration;

    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ScriptPostRender");
        for (EntityID id : scene.GetEntities<ScriptComponent>()) {
            auto* sc = scene.GetComponent<ScriptComponent>(id);
            auto* go = scene.GetGameObject(id);
            if (!sc || !go)
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

        // GPU 計測結果を Snapshot に詰める。QUERY_LATENCY フレーム以内は空になる。
        for (const auto& gp : renderer.GpuProfGetResults())
            dbgSnap.gpuPassTimings.push_back({ gp.name, gp.gpuMs });

        // カリング統計を Snapshot に詰める
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

        renderer::RenderDebugOverlay::UpdateSnapshot(dbgSnap, rs.passViewerEnabled);
    }
}

} // namespace fbzz::scene
