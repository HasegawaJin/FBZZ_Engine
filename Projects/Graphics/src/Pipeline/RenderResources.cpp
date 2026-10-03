/// @file    RenderResources.cpp
/// @brief   描画資源の初期化・ビューのリサイズと解放。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Effects/RenderProbeInput.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Math/MathUtils.hpp>
#include <Core/Logger.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include "CloudNoiseBake.hpp"
#include <algorithm>
#include <bit>
#include <cmath>

namespace fbzz::renderer {
namespace {
std::unique_ptr<Mesh> CreateSkyMesh(ResourceManager& resources)
{
    constexpr int segments = 32;
    std::vector<Vertex>   verts;
    std::vector<uint32_t> idx;

    int rings = segments / 2;

    for (int r = 0; r <= rings; ++r) {
        /// @note 0 → π
        float phi    = math::PI * r / rings;
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);

        for (int s = 0; s <= segments; ++s) {
            /// @note 0 → 2π
            float theta    = math::TWO_PI * s / segments;
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);

            math::Vector3 n = { sinPhi * cosTheta, cosPhi, sinPhi * sinTheta };
            /// @note tangent = dPos/dTheta 方向 (正規化)
            math::Vector3 t = { -sinTheta, 0.0f, cosTheta };
            verts.push_back({
                .position = { n.x * 0.5f, n.y * 0.5f, n.z * 0.5f },
                .normal   = n,
                .tangent  = t,
                .uv       = { static_cast<float>(s) / segments, static_cast<float>(r) / rings }
            });
        }
    }

    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            uint32_t a = r       * (segments + 1) + s;
            uint32_t b = (r + 1) * (segments + 1) + s;
            idx.push_back(a); idx.push_back(a + 1); idx.push_back(b);
            idx.push_back(b); idx.push_back(a + 1); idx.push_back(b + 1);
        }
    }

    auto mesh = std::make_unique<Mesh>();
    if (!mesh) return nullptr;
    mesh->vertexBuffer = resources.CreateVertexBuffer(
        verts.data(), verts.size() * sizeof(Vertex), sizeof(Vertex));
    mesh->indexBuffer  = resources.CreateIndexBuffer(idx.data(), static_cast<uint32_t>(idx.size()));
    mesh->vertexCount  = static_cast<uint32_t>(verts.size());
    mesh->indexCount   = static_cast<uint32_t>(idx.size());
    mesh->cpuVertices  = verts;
    mesh->cpuIndices   = idx;
    mesh->ComputeBounds();
    return mesh;
}
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
void ReleaseRayTracingViewResources(RenderViewResources& targets, ResourceManager& resources)
{
    resources.Release(targets.rayDebug.output);
    resources.Release(targets.rayDebug.constants);
    targets.rayDebug = {};
    resources.Release(targets.rayReflection.output);
    resources.Release(targets.rayReflection.halfRaw);
    resources.Release(targets.rayReflection.constants);
    resources.Release(targets.rayReflection.emitters);
    resources.Release(targets.rayReflection.deltaLights);
    resources.Release(targets.rayReflection.shapes);
    resources.Release(targets.rayReflection.environmentTable);
    for (const auto surface : targets.rayReflection.reconstruction.surfaces) resources.Release(surface);
    for (const auto history : targets.rayReflection.reconstruction.histories) resources.Release(history);
    resources.Release(targets.rayReflection.reconstruction.output);
    resources.Release(targets.rayReflection.reconstruction.constants);
    targets.rayReflection = {};
    resources.Release(targets.rayPath.output);
    resources.Release(targets.rayPath.firstSurface);
    resources.Release(targets.rayPath.firstMaterial);
    resources.Release(targets.rayPath.firstGeometry);
    resources.Release(targets.rayPath.historyBuffer);
    resources.Release(targets.rayPath.idsBuffer);
    resources.Release(targets.rayPath.constants);
    resources.Release(targets.rayPath.emitters);
    resources.Release(targets.rayPath.deltaLights);
    resources.Release(targets.rayPath.shapes);
    resources.Release(targets.rayPath.environmentTable);
    resources.Release(targets.rayPath.game.transport);
    resources.Release(targets.rayPath.game.surface);
    for (const auto history : targets.rayPath.game.reconstructionHistory) resources.Release(history);
    resources.Release(targets.rayPath.game.motionInstances);
    resources.Release(targets.rayPath.game.traceConstants);
    resources.Release(targets.rayPath.game.reconstructionConstants);
    targets.rayPath = {};
    targets.rayReflectionCovered = false;
    targets.rayPathCovered = false;
    targets.rayPathPrepared = false;
}
/// @note Resize 前のネイティブリソースを ResourceManager から確実に解放する。
void ReleaseRenderViewResources(RenderViewResources& targets, renderer::ResourceManager& resources)
{
    /// @note transient RT も同じ Viewport 寿命に属するため、固定 RT より先に明示解放する。
    targets.pipeline.ReleaseViewResources(resources);
    ReleaseRayTracingViewResources(targets, resources);
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
    const auto savedMaterialCB = targets.gbufferMaterialCB;
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
    const auto nativeWidth = targets.nativeWidth;
    const auto nativeHeight = targets.nativeHeight;
    const auto output = targets.output;
    targets = {};
    targets.nativeWidth = nativeWidth;
    targets.nativeHeight = nativeHeight;
    targets.output = output;
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
    targets.gbufferMaterialCB = savedMaterialCB;
    targets.prevViewProjection    = savedPrevVP;
    targets.invPrevViewProjection = savedPrevInvVP;
    targets.taaFrameIndex         = savedTaaIndex;
}

}
void RenderSharedResources::Initialize(ResourceManager& resources)
{
    /// @note シャドウアトラスは «解像度が動く» リソース (画質プリセットとエディタの Play/Stop)。
    /// @note 作り直しと解放は SizedRenderTarget に任せる ─ 自前で書くと、返し忘れた 1 か所が
    /// @note そのまま «ShadowPass だけ突然重い» になる。

    /// @note Spot / Point 用のシャドウアトラス。Directional の CSM とは面積を共有しない。

    /// @note ライト Cookie を敷き詰めるアトラス。寸法は固定なので作り直しは起きない。

    cookieBlitShader = resources.LoadShader("Assets/Shaders/Pipeline/Lighting/CookieBlit.hlsl");
    shadowShader = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMap.hlsl");
    /// @note 束ねた caster 用の変種。@see Docs/design/gpu-instancing.md
    shadowInstancedShader = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMapInstanced.hlsl");
    skinnedShadowShader = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/SkinnedShadowMap.hlsl");
    velocityShader = resources.LoadShader("Assets/Shaders/Motion/Velocity.hlsl");
    /// @note 束ねた物体の速度用の変種。@see Docs/design/gpu-instancing.md
    velocityInstancedShader = resources.LoadShader("Assets/Shaders/Motion/VelocityInstanced.hlsl");
    velocitySkinnedShader = resources.LoadShader("Assets/Shaders/Motion/VelocitySkinned.hlsl");
    /// @note コンピュートスキニング。無効ならスキンド描画は従来の VS スキニング経路へ落ちる。
    skinningComputeCS = resources.LoadShader("Assets/Shaders/Pipeline/Skinning/SkinningCompute.cs.hlsl");

    /// @note スキンドメッシュに AnimatorComponent がない場合のアイデンティティボーンパレット

    if (!bindPoseSkinningCB.IsValid()) {
        struct BindPoseData { math::Matrix4 bones[RENDER_SKINNING_BONES]; };
        BindPoseData bp{};
        for (auto& m : bp.bones) m = math::Matrix4::Identity();
        bindPoseSkinningCB = resources.CreateConstantBuffer(sizeof(BindPoseData));
        resources.Update(bindPoseSkinningCB, &bp, sizeof(BindPoseData));
    }

    compositeShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/Composite.hlsl");
    causticsShader = resources.LoadShader("Assets/Shaders/PostProcess/Water/Caustics.hlsl");
    volumetricCloudShader = resources.LoadShader("Assets/Shaders/PostProcess/Cloud/VolumetricCloud.hlsl");
    cloudUpscaleShader = resources.LoadShader("Assets/Shaders/PostProcess/Cloud/CloudUpscale.hlsl");
    ssaoShader = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAO.cs.hlsl");
    ssaoBlurShader = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAOBlur.cs.hlsl");
    bloomDownShader = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomDownsample.cs.hlsl");
    bloomUpShader = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomUpsample.cs.hlsl");
    selectionMaskShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMask.hlsl");
    selectionMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskSkinnedMesh.hlsl");
    selectionMaskParticleShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskParticle.hlsl");
    selectionMaskParticleGpuShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskParticleGPU.hlsl");
    selectionOutlineShader = resources.LoadShader("Assets/Shaders/PostProcess/Outline/SelectionOutline.hlsl");
    objectMaskShader = resources.LoadShader("Assets/Shaders/Pipeline/Mask/ObjectMask.hlsl");
    /// @note 束ねたシルエット用の変種。@see Docs/design/gpu-instancing.md
    objectMaskInstancedShader = resources.LoadShader("Assets/Shaders/Pipeline/Mask/ObjectMaskInstanced.hlsl");
    objectMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Pipeline/Mask/ObjectMaskSkinned.hlsl");
    copyColorShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
    customComposeShader = resources.LoadShader("Assets/Shaders/PostProcess/Custom/CustomCompose.hlsl");
    fxaaShader = resources.LoadShader("Assets/Shaders/PostProcess/AntiAliasing/FXAA.hlsl");
    upscaleShader = resources.LoadShader("Assets/Shaders/PostProcess/Upscale/Upscale.hlsl");
    downscaleShader = resources.LoadShader("Assets/Shaders/PostProcess/Upscale/Downscale.hlsl");

    /// @note Advanced Graphics シェーダー
    iblBrdfBakeShader = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/BRDFIntegration.cs.hlsl");
    gtaoShader = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/GTAO.cs.hlsl");
    gtaoBlurShader = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/GTAOBlur.cs.hlsl");
    ssrShader = resources.LoadShader("Assets/Shaders/PostProcess/Reflections/SSR.cs.hlsl");
    volumetricShader = resources.LoadShader("Assets/Shaders/PostProcess/Lighting/VolumetricLight.cs.hlsl");
    contactShadowShader = resources.LoadShader("Assets/Shaders/PostProcess/Shadow/ContactShadows.cs.hlsl");
    taaShader = resources.LoadShader("Assets/Shaders/PostProcess/AntiAliasing/TAA.hlsl");
    motionBlurShader = resources.LoadShader("Assets/Shaders/PostProcess/Motion/MotionBlur.cs.hlsl");
    lensFlareShader = resources.LoadShader("Assets/Shaders/PostProcess/Flare/LensFlare.hlsl");
    /// @note BRDF LUT は 512x512 の定数テーブルで、解像度・シーンが変わっても内容は変わらない。
    /// @note Manager の初期化時に一度だけ生成し、IBLBakePass でのみ書き込む。
    iblBrdfLut = resources.CreateComputeTexture(512, 512);

    proceduralColorLutHash = 0u;

    skydomeShader = resources.LoadShader("Assets/Shaders/Material/Sky/Skydome.hlsl");
    sunMoonShader = resources.LoadShader("Assets/Shaders/Material/Sky/SunMoon.hlsl");
    skydomeMesh = CreateSkyMesh(resources);

    /// @note 空連動 IBL (環境システム Phase A): 空を焼くキューブマップ RT と、面ごとの view/proj 用 CB。
    /// @note EnvironmentResources はフレームをまたいで保持し、SkyRenderer が変化した時だけ再キャプチャする。
    constexpr uint32_t kSkyEnvCubeSize = 128;



    if (!skyEnvCubeRT.IsValid()) {
    
        skyEnvCubeRT      = resources.CreateCubemapRenderTarget(kSkyEnvCubeSize, 1);
        skyCaptureFrameCB = resources.CreateConstantBuffer(sizeof(PerFrameCB));
        /// @note リソース再生成後 (デバイスリセット等) は古い動的 IBL ハンドルが無効。クリアして焼き直す。
        sEnvironmentResources.skyEnvCube    = {};
        sEnvironmentResources.skyIrradiance = {};
        sEnvironmentResources.skyPrefilter  = {};
        sEnvironmentResources.immutableIblOwner = nullptr;
        sEnvironmentResources.immutableIblEpoch = 0;
        sEnvironmentResources.immutableIrradiance = {};
        sEnvironmentResources.immutablePrefilter = {};
        sEnvironmentResources.needsConvolution = false;
        sEnvironmentResources.MarkDirty();
    }

    /// @note ボリューメトリック雲の 3D ノイズ (Shape 128³ + Detail 32³) を起動時に 1 回だけ CPU 焼きする。
    /// @note タイラブルなので WRAP サンプルで無限に並べられる。デバイスリセット後は SRV が無効になるため焼き直す。


    if (resources.Get(cloudShapeTex) == nullptr || resources.Get(cloudDetailTex) == nullptr) {
        const std::vector<uint8_t> shape  = cloudnoise::BakeShape(128);
        const std::vector<uint8_t> detail = cloudnoise::BakeDetail(32);
        cloudShapeTex  = resources.CreateTexture3D(shape.data(),  128, 128, 128);
        cloudDetailTex = resources.CreateTexture3D(detail.data(),  32,  32,  32);
        FBZZ_LOG_DEBUG("VolumetricCloud: baked tileable 3D noise (shape 128^3, detail 32^3)");
    }

    gbufferShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBuffer.hlsl");
    /// @note 束ねた不透明メッシュ用の変種。@see Docs/design/gpu-instancing.md
    gbufferInstancedShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBufferInstanced.hlsl");
    /// @note スキンドを GBuffer へ入れる経路のフォールバック。@see Docs/design/pipeline-boundary.md
    gbufferSkinnedShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBufferSkinned.hlsl");
    deferredLightingShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DeferredLighting.hlsl");
    depthCopyShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");

    /// @note クラスタライトカリング (Forward+ / Deferred+)。
    clusterCullCS = resources.LoadShader("Assets/Shaders/Pipeline/Clustered/ClusterLightCull.cs.hlsl");
    /// @note clusterIndexBuffer は解像度非依存の固定長なので確保は初回の 1 回だけ。
    /// @note CS が u2 へ書き PS が t30 から読むので RW。
    /// @note punctualLightBuffer (CPU が書いて GPU が読むだけ) は 1 枚を共有せず、描くたびに借りる。
    /// @note DX12 の読み取り専用 StructuredBuffer は Upload ヒープへの直 memcpy。1 枚だと
    /// @note Scene View の Draw が Game View の書いた配列を読み、GPU がまだ読んでいる
    /// @note 前フレームの配列も上書きする (ライトが増減したフレームにだけ幽霊が出る)。

    clusterIndexBuffer = resources.CreateRWStructuredBuffer(
        nullptr, kClusterCount * kClusterStride, static_cast<uint32_t>(sizeof(uint32_t)));

    /// @note 自動露出のバッファはビュー単位 (ViewRenderTargets) で確保する。シェーダーだけ共有。
    exposureHistogramCS = resources.LoadShader("Assets/Shaders/PostProcess/Color/ExposureHistogram.cs.hlsl");
    exposureAverageCS = resources.LoadShader("Assets/Shaders/PostProcess/Color/ExposureAverage.cs.hlsl");

    /// @note フロクセル霧の CS。ボリューム本体はビュー単位なので、下の per-view ブロックで確保する。
    froxelInjectCS = resources.LoadShader("Assets/Shaders/PostProcess/Cloud/FroxelInject.cs.hlsl");
    froxelIntegrateCS = resources.LoadShader("Assets/Shaders/PostProcess/Cloud/FroxelIntegrate.cs.hlsl");

    /// @note Light Probe Volume の焼き。ボリュームと面の RT は各コンポーネントが持つので、ここは共有の CS と定数だけ。
    lightProbeProjectCS = resources.LoadShader("Assets/Shaders/IBL/LightProbeProject.cs.hlsl");
    lightProbeProjectCB = resources.CreateConstantBuffer(kLightProbeProjectCBSize);
    lightProbeCaptureFrameCB = resources.CreateConstantBuffer(sizeof(PerFrameCB));
    lightProbeCaptureAdvancedCB = resources.CreateConstantBuffer(sizeof(AdvancedGraphicsCB));
    lightProbeDilateCS = resources.LoadShader("Assets/Shaders/IBL/LightProbeDilate.cs.hlsl");
    lightProbeDilateCB = resources.CreateConstantBuffer(kLightProbeDilateCBSize);
    lightProbeFacingShader = resources.LoadShader("Assets/Shaders/IBL/LightProbeFacing.hlsl");
    clusterCB = resources.CreateConstantBuffer(sizeof(ClusterConstantsCB));
    /// @note 別視点から描くパス用に、供給モードだけ Linear へ落とした同内容の CB。
    clusterLinearCB = resources.CreateConstantBuffer(sizeof(ClusterConstantsCB));

    decalShader = resources.LoadShader("Assets/Shaders/Material/Decal/Decal.hlsl");
    decalMaskShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMask.hlsl");
    decalMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMaskSkinned.hlsl");

    particleShader = resources.LoadShader("Assets/Shaders/Material/Effects/Particle.hlsl");
    particleGpuSimCS = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSim.cs.hlsl");
    particleGpuShader = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGPU.hlsl");
    /// @note GPU ソート 3 段 (キー生成 / グローバル段 / LDS 段)。sortMode != None のときだけ走る。
    particleGpuSortKeysCS = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortKeys.cs.hlsl");
    particleGpuSortStepCS = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortStep.cs.hlsl");
    particleGpuSortLocalCS = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortLocal.cs.hlsl");
    particleGpuMeshShader = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuMesh.hlsl");
    /// @note 自己影: 光源から見た密度を積む。selfShadowStrength > 0 のエミッターがあるときだけ走る。
    particleSelfShadowShader = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleSelfShadowDensity.hlsl");
    trailShader = resources.LoadShader("Assets/Shaders/Material/Effects/Trail.hlsl");
    meshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/MeshTrail.hlsl");
    skinnedMeshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/SkinnedMeshTrail.hlsl");
    frameCB = resources.CreateConstantBuffer(sizeof(PerFrameCB));
    objectCB = resources.CreateConstantBuffer(sizeof(PerObjectCB));
    lightCB = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    shadowCB = resources.CreateConstantBuffer(sizeof(ShadowConstantsCB));
    punctualShadowCB = resources.CreateConstantBuffer(sizeof(PunctualShadowConstantsCB));
    cookieBlitCB = resources.CreateConstantBuffer(sizeof(CookieBlitCB));
    exposureCB = resources.CreateConstantBuffer(sizeof(AutoExposureCB));
    froxelFogCB = resources.CreateConstantBuffer(sizeof(FroxelFogCB));
    /// @note コンピュートスキニングの b0 (頂点数のみ)。16 バイト境界へ切り上げられる。
    skinningCB = resources.CreateConstantBuffer(16);
    postprocCB = resources.CreateConstantBuffer(sizeof(PostProcCB));
    outlineCB = resources.CreateConstantBuffer(sizeof(OutlineCB));
    objectMaskCB = resources.CreateConstantBuffer(sizeof(ObjectMaskCB));
    atmCB = resources.CreateConstantBuffer(sizeof(AtmosphereCB));
    decalCB = resources.CreateConstantBuffer(sizeof(DecalCB));
    decalMaterialCB = resources.CreateConstantBuffer(sizeof(DecalMaterialCB));
    decalReceiverCB = resources.CreateConstantBuffer(sizeof(DecalReceiverCB));
    volumetricCloudCB = resources.CreateConstantBuffer(176);
    /// @note パーティクル自己影: 光源側の密度 RT と、光源行列を入れる専用 frame CB。
    /// @note RenderPassHandles は毎フレーム作り直される値型なので、パス側で遅延生成すると RT を漏らす。
    /// @note 解像度が固定なのは、拾うのが「煙の内部で光がどれだけ減るか」という低周波の情報だから。

    particleSelfShadowFrameCB = resources.CreateConstantBuffer(sizeof(PerFrameCB));

    /// @note Manager の Reset で描画状態ごと破棄し、次の描画で再生成する。
    /// @note これによりデバイスロスト復帰時も旧ネイティブリソースへ触らない。
    defaultPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    wireframePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::WIREFRAME,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    selectionMaskPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    skydomePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_SKY
    });
    sunMoonPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_SKY
    });
    particlePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_READ
    });
    particleAlphaPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    particlePremultipliedPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::PREMULTIPLIED,
        renderer::DepthMode::DEPTH_READ
    });
    particleGpuPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_READ
    });
    particleGpuAlphaPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    particleGpuPremultipliedPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::PREMULTIPLIED,
        renderer::DepthMode::DEPTH_READ
    });
    trailPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    meshTrailPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    meshTrailDoubleSidedPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    postprocPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    causticsPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_OFF
    });
    volumetricCloudPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    /// @note VolumetricCloud.hlsl は scatter.rgb に既に透過率を積分した premultiplied 値を返す。
    /// @note Overdraw 可視化も volumetricCloudPSO を共有するため、雲の合成だけ専用 PSO に分離する。
    volumetricCloudPremultipliedPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::PREMULTIPLIED,
        renderer::DepthMode::DEPTH_OFF
    });
    /// @note Advanced Graphics PSO / 定数バッファ
    /// @note taaPSO: OPAQUE — TAA は ping-pong バッファへ上書きするため α ブレンドは不要
    taaPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    /// @note lensFlarePSO: ADDITIVE — ゴーストはフレアを HDR バッファに加算合成する
    lensFlarePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_OFF
    });
    decalPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    decalMaskPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    /// @note パーティクルのクワッド用インデックスバッファ (最大描画数分を事前確保)。
    /// @note 頂点側は共有せず、描画時に DynamicVertexBufferPool から 1 エミッターぶんずつ借りる。
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

}
bool RenderSharedResources::Prepare(ResourceManager& resources, const RenderSettings& rs)
{
    const bool initializedNow = !m_initialized;
    if (initializedNow) {
        Initialize(resources);
        m_initialized = true;
    }
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

    return initializedNow;
}
RenderResources::RenderResources(ResourceManager& resources) : m_resources(resources)
{
}
void RenderResources::SetExperimentalRayTracingEnabled(bool enabled)
{
    if (m_experimentalRayTracingEnabled == enabled) return;
    m_experimentalRayTracingEnabled = enabled;
    if (enabled) return;
    for (auto& entry : m_views) ReleaseRayTracingViewResources(entry.second, m_resources);
    m_shared.rayGeometry.Release(m_resources);
    m_resources.Release(m_shared.rayDebugShader);
    m_resources.Release(m_shared.rayReflectionShader);
    m_resources.Release(m_shared.rayReflectionReconstructionShader);
    m_resources.Release(m_shared.rayPathShader);
    m_resources.Release(m_shared.rayPathResolveShader);
    m_resources.Release(m_shared.rayGameReconstructionShader);
    m_resources.Release(m_shared.rayPathResolvePSO);
    m_shared.rayDebugShader = {};
    m_shared.rayReflectionShader = {};
    m_shared.rayReflectionReconstructionShader = {};
    m_shared.rayPathShader = {};
    m_shared.rayPathResolveShader = {};
    m_shared.rayGameReconstructionShader = {};
    m_shared.rayPathResolvePSO = {};
}
RenderViewResources& RenderResources::View(uint32_t key) { return m_views[key]; }
bool RenderResources::PrepareView(RenderViewResources& viewTargets, IRenderer& renderer,
    ResourceHandle<RenderTargetTag> outputRT, const RenderSettings& rs)
{
    viewTargets.output = outputRT;
    viewTargets.renderPlan = {};
    viewTargets.rayReflection.gpu = {};
    viewTargets.rayReflection.scene = {};
    viewTargets.rayReflectionCovered = false;
    viewTargets.rayPath.gpu = {};
    viewTargets.rayPath.scene = {};
    viewTargets.rayPath.dispatchSucceeded = false;
    viewTargets.rayPathCovered = false;
    viewTargets.rayPathPrepared = false;
    auto& resources = m_resources;
    if (!resources.Get(viewTargets.gbufferMaterialCB))
        viewTargets.gbufferMaterialCB = resources.CreateConstantBuffer(96);
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
    /// @note Reset 後は新しいビュー状態へ再生成する。
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
    auto& nativeW = viewTargets.nativeWidth;
    auto& nativeH = viewTargets.nativeHeight;
    /// @note 内部解像度と出力先の実寸が食い違うフレームか。UpscalePass の要否そのもの。
    auto& needsUpscale = viewTargets.needsUpscale;
    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ResizeRenderTargets");
        const auto* output = resources.Get(outputRT);
        nativeW = output ? output->GetWidth()  : renderer.GetWidth();
        nativeH = output ? output->GetHeight() : renderer.GetHeight();
        if (nativeW == 0 || nativeH == 0) return false;

        /// @note 内部描画解像度。ここで倍率を掛ければ中間 RT もビューポートも texelSize も追従する。
        /// @note 実寸へ戻すのは UpscalePass ただ 1 つ。ポストの各段が outputRT へ直接書くと、
        /// @note その段だけが実寸で走り、描画スケールで浮かせたはずのコストが最後に戻ってくる。
        uint32_t curW = 0;
        uint32_t curH = 0;
        renderer::ResolveRenderResolution(nativeW, nativeH, rs.renderScale, curW, curH);
        if (curW == 0 || curH == 0) return false;
        if (!hdrRT.IsValid() || sHdrW != curW || sHdrH != curH)
        {
            /// @note 全画面ポストの中継先。深度テストも深度書き込みもしないので深度を持たない。
            /// @note 深度が要るのは «ジオメトリを描く RT» と «深度を SRV で読まれる RT» の 2 つだけで、
            /// @note 中継先はどちらでもない。1080p で 1 枚 8MB、ビューごとに 7 枚ぶん浮く。
            constexpr renderer::RenderTargetDesc kPostChainRT{
                1, renderer::Format::RGBA16F, false };
            /// @note カメラ視点の深度を持つ RT は Reversed-Z。GPU へ渡す射影 (Camera::GetGpuProjectionMatrix) と対。
            const auto cameraDepthRT = renderer::CameraDepthTargetDesc;

            ReleaseRenderViewResources(viewTargets, resources);
            hdrRT           = resources.CreateRenderTarget(curW, curH, cameraDepthRT(1));
            ldrRT           = resources.CreateRenderTarget(curW, curH, kPostChainRT);
            /// @note 選択マスクと輪郭マスクはジオメトリを描き、深度も読まれる。
            selectionMaskRT = resources.CreateRenderTarget(curW, curH, cameraDepthRT(1));
            outlineRT       = resources.CreateRenderTarget(curW, curH, kPostChainRT);
            objectMaskRT   = resources.CreateRenderTarget(curW, curH, cameraDepthRT(1));
            customPostProcessRT[0] = resources.CreateRenderTarget(curW, curH, kPostChainRT);
            customPostProcessRT[1] = resources.CreateRenderTarget(curW, curH, kPostChainRT);
            gbufferRT       = resources.CreateRenderTarget(curW, curH, cameraDepthRT(GBUFFER_COLOR_COUNT));
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
    /// @note Advanced Graphics per-view テクスチャ
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
                    renderer::RenderTargetDesc{ 1, renderer::Format::RGBA16F, false });
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

    return viewTargets.hdr.IsValid() && viewTargets.ldr.IsValid();
}
void RenderResources::ReleaseView(uint32_t key)
{
    const auto found = m_views.find(key);
    if (found == m_views.end()) return;
    auto& view = found->second;
    view.output = {};
    ReleaseRenderViewResources(view, m_resources);
    m_resources.Release(view.advancedGraphicsCB);
    m_resources.Release(view.gbufferMaterialCB);
    m_resources.Release(view.exposureHistogram);
    m_resources.Release(view.exposureResult);
    m_resources.Release(view.froxelScatter);
    m_resources.Release(view.froxelScatterHistory);
    m_resources.Release(view.froxelIntegrated);
    m_views.erase(found);
}
void RenderResources::ReleaseOutput(ResourceHandle<RenderTargetTag> output)
{
    if (!output.IsValid()) return;
    std::vector<uint32_t> keys;
    for (const auto& [key, view] : m_views)
        if (view.output == output) keys.push_back(key);
    for (uint32_t key : keys) ReleaseView(key);
}
void RenderResources::BindPassHandles(RenderViewResources& viewTargets, RenderPassHandles& passHandles)
{
    auto& resources = m_resources;
    if (!resources.Get(viewTargets.gbufferMaterialCB))
        viewTargets.gbufferMaterialCB = resources.CreateConstantBuffer(96);
    passHandles.gbufferMaterialCB = viewTargets.gbufferMaterialCB;
    auto& shared = m_shared;
    if (!resources.Get(shared.reflectionProbeCaptureShadowCB))
        shared.reflectionProbeCaptureShadowCB = resources.CreateConstantBuffer(sizeof(ShadowConstantsCB));
    if (!resources.Get(shared.reflectionProbeCapturePunctualCB))
        shared.reflectionProbeCapturePunctualCB = resources.CreateConstantBuffer(sizeof(PunctualShadowConstantsCB));
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
    auto& advancedGraphicsCB = viewTargets.advancedGraphicsCB;
    /// @note RenderPassHandles を組み立て

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
    passHandles.ssaoShader        = shared.ssaoShader;
    passHandles.ssaoBlurShader    = shared.ssaoBlurShader;
    passHandles.bloomDownShader   = shared.bloomDownShader;
    passHandles.bloomUpShader     = shared.bloomUpShader;
    passHandles.compositeShader   = shared.compositeShader;
    passHandles.causticsShader    = shared.causticsShader;
    passHandles.volumetricCloudShader = shared.volumetricCloudShader;
    passHandles.cloudUpscaleShader    = shared.cloudUpscaleShader;
    passHandles.selectionMaskShader       = shared.selectionMaskShader;
    passHandles.selectionMaskSkinnedShader = shared.selectionMaskSkinnedShader;
    passHandles.selectionMaskParticleShader = shared.selectionMaskParticleShader;
    passHandles.selectionMaskParticleGpuShader = shared.selectionMaskParticleGpuShader;
    passHandles.selectionOutlineShader    = shared.selectionOutlineShader;
    passHandles.objectMaskShader         = shared.objectMaskShader;
    passHandles.objectMaskInstancedShader = shared.objectMaskInstancedShader;
    passHandles.objectMaskSkinnedShader  = shared.objectMaskSkinnedShader;
    passHandles.copyColorShader           = shared.copyColorShader;
    passHandles.upscaleShader             = shared.upscaleShader;
    passHandles.downscaleShader           = shared.downscaleShader;
    passHandles.customComposeShader       = shared.customComposeShader;
    passHandles.fxaaShader        = shared.fxaaShader;
    passHandles.selectionMaskPSO  = shared.selectionMaskPso;
    passHandles.postprocPSO       = shared.postprocPSO;
    /// @note Cookie 焼き。全画面三角形を不透明で塗るだけなので postproc と同じ状態でよい。
    passHandles.cookieBlitShader  = shared.cookieBlitShader;
    passHandles.cookieBlitCB      = shared.cookieBlitCB;
    passHandles.cookieBlitPSO     = shared.postprocPSO;

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
    passHandles.exposureHistogramCS = shared.exposureHistogramCS;
    passHandles.exposureAverageCS   = shared.exposureAverageCS;
    passHandles.exposureCB          = shared.exposureCB;

    /// @note 散乱ボリュームは 2 枚をフレームごとに入れ替える。Inject が scatter へ書きながら
    /// @note history を読むので、同じテクスチャを UAV と SRV に同時に張れない。
    /// @note 向きもビュー別。static だと 1 フレームに 2 回反転し、互いの履歴を読み合う。
    viewTargets.froxelState.ping = !viewTargets.froxelState.ping;
    passHandles.froxelScatter        = viewTargets.froxelState.ping
                                     ? viewTargets.froxelScatter : viewTargets.froxelScatterHistory;
    passHandles.froxelScatterHistory = viewTargets.froxelState.ping
                                     ? viewTargets.froxelScatterHistory : viewTargets.froxelScatter;
    passHandles.froxelIntegrated  = viewTargets.froxelIntegrated;
    passHandles.froxelInjectCS    = shared.froxelInjectCS;
    passHandles.froxelIntegrateCS = shared.froxelIntegrateCS;
    passHandles.froxelFogCB       = shared.froxelFogCB;
    passHandles.causticsPSO       = shared.causticsPSO;
    passHandles.volumetricCloudPSO = shared.volumetricCloudPSO;
    passHandles.volumetricCloudPremultipliedPSO = shared.volumetricCloudPremultipliedPSO;
    passHandles.frameCB           = shared.frameCB;
    passHandles.objectCB          = shared.objectCB;
    passHandles.lightCB           = shared.lightCB;
    passHandles.bindPoseSkinningCB = shared.bindPoseSkinningCB;

    passHandles.postprocCB        = shared.postprocCB;
    passHandles.outlineCB         = shared.outlineCB;
    passHandles.objectMaskCB     = shared.objectMaskCB;
    passHandles.volumetricCloudCB = shared.volumetricCloudCB;
    passHandles.cloudShapeTex     = shared.cloudShapeTex;
    passHandles.cloudDetailTex    = shared.cloudDetailTex;
    passHandles.decalMaskRT       = decalMaskRT;
    passHandles.decalShader       = shared.decalShader;
    passHandles.decalMaskShader   = shared.decalMaskShader;
    passHandles.decalMaskSkinnedShader = shared.decalMaskSkinnedShader;
    passHandles.decalPSO          = shared.decalPSO;
    passHandles.decalMaskPSO      = shared.decalMaskPso;
    passHandles.decalCB           = shared.decalCB;
    passHandles.decalMaterialCB   = shared.decalMaterialCB;
    passHandles.decalReceiverCB   = shared.decalReceiverCB;
    /// @note ジオメトリ用ハンドル
    passHandles.shadowShader         = shared.shadowShader;
    passHandles.shadowInstancedShader = shared.shadowInstancedShader;
    passHandles.shadowSkinnedShader  = shared.skinnedShadowShader;
    passHandles.shadowCB             = shared.shadowCB;
    passHandles.velocityShader        = shared.velocityShader;
    passHandles.velocityInstancedShader = shared.velocityInstancedShader;
    passHandles.velocitySkinnedShader = shared.velocitySkinnedShader;
    passHandles.punctualShadowCB     = shared.punctualShadowCB;
    passHandles.reflectionProbeCaptureShadowCB = shared.reflectionProbeCaptureShadowCB;
    passHandles.reflectionProbeCapturePunctualCB = shared.reflectionProbeCapturePunctualCB;
    passHandles.skinningComputeCS    = shared.skinningComputeCS;
    passHandles.skinningCB           = shared.skinningCB;
    passHandles.defaultPSO           = shared.defaultPSO;
    passHandles.wireframePSO         = shared.wireframePSO;
    passHandles.skyShader            = shared.skydomeShader;
    passHandles.sunMoonShader        = shared.sunMoonShader;
    passHandles.skyPSO               = shared.skydomePSO;
    passHandles.sunMoonPSO           = shared.sunMoonPSO;
    if (shared.skydomeMesh) {
        passHandles.skyVB         = shared.skydomeMesh->vertexBuffer;
        passHandles.skyIB         = shared.skydomeMesh->indexBuffer;
        passHandles.skyIndexCount = shared.skydomeMesh->indexCount;
    }
    passHandles.atmosphereCB         = shared.atmCB;
    passHandles.skyEnvCubeRT         = shared.skyEnvCubeRT;
    passHandles.skyCaptureFrameCB    = shared.skyCaptureFrameCB;
    passHandles.lightProbeProjectCS         = shared.lightProbeProjectCS;
    passHandles.lightProbeProjectCB         = shared.lightProbeProjectCB;
    passHandles.lightProbeCaptureFrameCB    = shared.lightProbeCaptureFrameCB;
    passHandles.lightProbeCaptureAdvancedCB = shared.lightProbeCaptureAdvancedCB;
    passHandles.lightProbeDilateCS          = shared.lightProbeDilateCS;
    passHandles.lightProbeDilateCB          = shared.lightProbeDilateCB;
    passHandles.lightProbeFacingShader      = shared.lightProbeFacingShader;
    passHandles.lightProbeSH[0]             = {};
    passHandles.lightProbeSH[1]             = {};
    passHandles.particleShader       = shared.particleShader;
    passHandles.particlePSO          = shared.particlePSO;
    passHandles.particleAlphaPSO     = shared.particleAlphaPSO;
    passHandles.particlePremultipliedPSO = shared.particlePremultipliedPSO;
    passHandles.particleIB           = shared.particleIB;
    passHandles.particleGpuSimCS     = shared.particleGpuSimCS;
    passHandles.particleGpuSortKeysCS  = shared.particleGpuSortKeysCS;
    passHandles.particleGpuSortStepCS  = shared.particleGpuSortStepCS;
    passHandles.particleGpuSortLocalCS = shared.particleGpuSortLocalCS;
    passHandles.particleGpuMeshShader  = shared.particleGpuMeshShader;
    passHandles.particleSelfShadowShader = shared.particleSelfShadowShader;
    passHandles.particleSelfShadowRT      = shared.particleSelfShadowRT;
    passHandles.particleSelfShadowFrameCB = shared.particleSelfShadowFrameCB;
    passHandles.particleGpuShader    = shared.particleGpuShader;
    passHandles.particleGpuPSO       = shared.particleGpuPSO;
    passHandles.particleGpuAlphaPSO  = shared.particleGpuAlphaPSO;
    passHandles.particleGpuPremultipliedPSO = shared.particleGpuPremultipliedPSO;
    passHandles.trailShader          = shared.trailShader;
    passHandles.trailPSO             = shared.trailPSO;
    passHandles.meshTrailShader      = shared.meshTrailShader;
    passHandles.skinnedMeshTrailShader = shared.skinnedMeshTrailShader;
    passHandles.meshTrailPSO         = shared.meshTrailPSO;
    passHandles.meshTrailDoubleSidedPSO = shared.meshTrailDoubleSidedPSO;
    passHandles.gbufferShader        = shared.gbufferShader;
    passHandles.gbufferInstancedShader = shared.gbufferInstancedShader;
    passHandles.gbufferSkinnedShader   = shared.gbufferSkinnedShader;
    passHandles.deferredLightingShader = shared.deferredLightingShader;
    passHandles.depthCopyShader      = shared.depthCopyShader;
    passHandles.clusterCullCS        = shared.clusterCullCS;
    passHandles.punctualLightBuffer  = shared.punctualLightPool.Acquire(
        resources, kMaxPunctualLights, static_cast<uint32_t>(sizeof(PunctualLightGPU)));
    passHandles.clusterIndexBuffer   = shared.clusterIndexBuffer;
    passHandles.clusterCB            = shared.clusterCB;
    passHandles.clusterLinearCB      = shared.clusterLinearCB;

    /// @note Advanced Graphics ハンドルを passHandles に束縛
    passHandles.advancedGraphicsCB   = advancedGraphicsCB;
    passHandles.proceduralColorLut = shared.proceduralColorLut;
    /// @note IBLBakePass は Manager ごとの LUT へ初回だけ書き込む。
    passHandles.iblBrdfLut           = shared.iblBrdfLut;
    passHandles.iblBrdfBakeShader    = shared.iblBrdfBakeShader;
    /// @note SSR
    passHandles.ssrResult            = ssrResult;
    passHandles.ssrShader            = shared.ssrShader;
    /// @note Volumetric Lighting
    passHandles.volumetricResult     = volumetricResult;
    passHandles.volumetricShader     = shared.volumetricShader;
    /// @note TAA (ping-pong)
    passHandles.taaHistoryA          = taaHistoryA;
    passHandles.taaHistoryB          = taaHistoryB;
    passHandles.taaFlip              = viewTargets.taaFlip;
    passHandles.taaShader            = shared.taaShader;
    passHandles.taaPSO               = shared.taaPSO;
    /// @note Motion Blur
    passHandles.motionBlurResult     = motionBlurResult;
    passHandles.motionBlurShader     = shared.motionBlurShader;
    /// @note GTAO
    passHandles.gtaoRaw              = gtaoRaw;
    passHandles.gtaoShader           = shared.gtaoShader;
    passHandles.gtaoBlurShader       = shared.gtaoBlurShader;
    /// @note Contact Shadows
    passHandles.contactShadowShader  = shared.contactShadowShader;
    /// @note Lens Flare
    passHandles.lensFlareShader      = shared.lensFlareShader;
    passHandles.lensFlarePSO         = shared.lensFlarePSO;

}

}
