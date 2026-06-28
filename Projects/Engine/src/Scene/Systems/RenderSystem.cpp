// FBZZ Engine
// RenderSystem.cpp | fbzz::scene
// Scene から DrawCall を生成するオーケストレーター
// 各描画パスの実装は RenderPasses/ 以下の Execute*Pass 関数に委譲する。
#include "Engine/Scene/Systems/RenderSystem.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/TerrainRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/WaterRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/DetailRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/FoliageRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/MeshTrailRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/TrailRenderPass.hpp"
#include "Engine/Renderer/RenderSettings.hpp"
#include "Engine/Renderer/RenderDebugOverlay.hpp"
#include "Engine/Renderer/DebugDraw.hpp"
#include "RenderPasses/Debug/DebugPasses.hpp"
#include <Physics/World.hpp>
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include "RenderPasses/PostProcess/PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include "RenderPasses/Debug/SelectionPasses.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/ScriptComponent.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/LightComponent.hpp"
#include "Engine/Scene/Components/EnvironmentLightComponent.hpp"
#include "Engine/Scene/Components/AtmosphericScatteringComponent.hpp"
#include "Engine/Scene/Components/PostProcessVolumeComponent.hpp"
#include "Engine/Scene/Components/ReflectionProbeComponent.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/Camera.hpp"
#include "Engine/Renderer/LightSystem.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Renderer/PrimitiveMesh.hpp"
#include "Engine/Scene/Systems/RenderPasses/RenderPipeline.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Asset/Skeleton.hpp"
#include <Engine/Profiler/ProfileScope.hpp>
#include <Math/Frustum.hpp>
#include <Math/Matrix4.hpp>
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
// WHY: Scene View と Game View は解像度が異なるため、単一の static RT 群を共有すると
//      1 フレーム内で互いのサイズへリサイズし続け、D3D11 リソース生成待ちが発生する。
struct ViewRenderTargets {
    renderer::ResourceHandle<renderer::RenderTargetTag> hdr;
    renderer::ResourceHandle<renderer::RenderTargetTag> ldr;
    renderer::ResourceHandle<renderer::RenderTargetTag> selectionMask;
    renderer::ResourceHandle<renderer::RenderTargetTag> outline;
    renderer::ResourceHandle<renderer::RenderTargetTag> customPostProcess[2];
    renderer::ResourceHandle<renderer::RenderTargetTag> gbuffer;
    renderer::ResourceHandle<renderer::RenderTargetTag> decalDepth;
    renderer::ResourceHandle<renderer::RenderTargetTag> decalMask;
    renderer::ResourceHandle<renderer::TextureTag> bloomHalf;
    renderer::ResourceHandle<renderer::TextureTag> bloomFull;
    renderer::ResourceHandle<renderer::TextureTag> ssaoRaw;
    renderer::ResourceHandle<renderer::TextureTag> ssaoBlur;
    // ---- Advanced Graphics (解像度依存・ビュー単位) ----
    // WHY: これらは解像度変更時に再生成が必要なため ViewRenderTargets に含める。
    //      static なリソース (BRDF LUT 等) は別途 static 変数で管理する。
    renderer::ResourceHandle<renderer::TextureTag>        ssrResult;           // SSR CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        volumetricResult;    // Volumetric CS 出力
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryA;         // TAA ping-pong A
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryB;         // TAA ping-pong B
    renderer::ResourceHandle<renderer::TextureTag>        motionBlurResult;    // Motion Blur CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        gtaoRaw;             // GTAO RAW CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        gtaoBlur;            // GTAO Blur CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        contactShadowResult; // Contact Shadow CS 出力
    // ---- ビュー別定数バッファ / 再投影行列 ----
    // WHY: sPrevVP を static で共有すると複数ビュー (SceneView + GameView) で
    //      フレームをまたいで互いのカメラ行列を誤って参照し、MotionBlur / TAA が
    //      常に壊れた再投影を行う。viewKey で分離した ViewRenderTargets に持つことで正しく分離する。
    renderer::ResourceHandle<renderer::ConstantBufferTag> advancedGraphicsCB;
    math::Matrix4 prevViewProjection    = math::Matrix4::Identity();
    math::Matrix4 invPrevViewProjection = math::Matrix4::Identity();
    uint32_t width = 0;
    uint32_t height = 0;
};

// Resize 前のネイティブリソースを ResourceManager から確実に解放する。
void ReleaseViewRenderTargets(ViewRenderTargets& targets, renderer::ResourceManager& resources)
{
    if (targets.hdr.IsValid())                  resources.Release(targets.hdr);
    if (targets.ldr.IsValid())                  resources.Release(targets.ldr);
    if (targets.selectionMask.IsValid())        resources.Release(targets.selectionMask);
    if (targets.outline.IsValid())              resources.Release(targets.outline);
    if (targets.customPostProcess[0].IsValid()) resources.Release(targets.customPostProcess[0]);
    if (targets.customPostProcess[1].IsValid()) resources.Release(targets.customPostProcess[1]);
    if (targets.gbuffer.IsValid())              resources.Release(targets.gbuffer);
    if (targets.decalDepth.IsValid())           resources.Release(targets.decalDepth);
    if (targets.decalMask.IsValid())            resources.Release(targets.decalMask);
    if (targets.bloomHalf.IsValid())            resources.Release(targets.bloomHalf);
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
    // WHY: advancedGraphicsCB / prevViewProjection は解像度非依存。
    //      リサイズのたびに破棄・再生成するとコストが発生し、prevVP が Identity にリセットされて
    //      MotionBlur / TAA が 1 フレーム乱れる。保存して復元することで連続性を維持する。
    auto savedCB         = targets.advancedGraphicsCB;
    auto savedPrevVP     = targets.prevViewProjection;
    auto savedPrevInvVP  = targets.invPrevViewProjection;
    targets = {};
    targets.advancedGraphicsCB  = savedCB;
    targets.prevViewProjection    = savedPrevVP;
    targets.invPrevViewProjection = savedPrevInvVP;
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
                if (ComputeSkinnedWorldBounds(go.transform, *smr, bounds))
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
                  const renderer::Camera& camera,
                  renderer::ResourceHandle<renderer::RenderTargetTag> outputRT,
                  const renderer::RenderSettings* settings,
                  fbzz::LayerMask cullingMask,
                  const RenderSystemUIOptions* uiOptions,
                  const physics::World* physicsWorld)
{
    FBZZ_PROFILE_SCOPE("RenderSystem");

    static renderer::RenderSettings sDefaultSettings;
    renderer::RenderSettings effectiveSettings = settings ? *settings : sDefaultSettings;
    if (const auto* runtimePostProcess = scene.TryGetRuntimePostProcessSettings())
        effectiveSettings.postProcess = *runtimePostProcess;

    // ── コンポーネントによる設定上書き (ProjectSettings < runtimePostProcess < Component) ───
    // EnvironmentLightComponent — シーン Inspector から IBL を上書きする。
    // 最初のアクティブなコンポーネントのみ採用する。複数置かれた場合は先着優先。
    for (auto [tf, elc] : scene.View<Transform, EnvironmentLightComponent>()) {
        if (!elc.enabled) continue;
        effectiveSettings.ibl.enabled       = !elc.irradiancePath.empty() && !elc.prefilterPath.empty();
        effectiveSettings.ibl.irradiancePath = elc.irradiancePath;
        effectiveSettings.ibl.prefilterPath  = elc.prefilterPath;
        effectiveSettings.ibl.intensity      = elc.intensity;
        effectiveSettings.ibl.diffuseScale   = elc.diffuseScale;
        effectiveSettings.ibl.specularScale  = elc.specularScale;
        effectiveSettings.ibl.maxMipLevel    = elc.maxMipLevel;
        break;
    }
    // AtmosphericScatteringComponent — シーン Inspector から霧設定を上書きする。
    for (auto [tf, atm] : scene.View<Transform, AtmosphericScatteringComponent>()) {
        if (!atm.enabled) continue;
        auto& fog      = effectiveSettings.postProcess.fog;
        fog.enabled    = atm.fogEnabled;
        fog.density    = atm.fogDensity;
        fog.farDistance = atm.fogFar;
        fog.color[0]   = atm.fogColor.x;
        fog.color[1]   = atm.fogColor.y;
        fog.color[2]   = atm.fogColor.z;
        break;
    }
    // PostProcessVolumeComponent — カメラに付けてポストプロセス設定を Inspector から制御する。
    // isGlobal=true のとき常時適用する。将来的に blendWeight によるブレンド合成へ拡張予定。
    for (auto [tf, ppv] : scene.View<Transform, PostProcessVolumeComponent>()) {
        if (!ppv.enabled || !ppv.isGlobal) continue;
        effectiveSettings.postProcess = ppv.settings;
        break;
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
    static uint32_t sShadowMapResolution  = 0u;
    static renderer::ResourceHandle<renderer::RenderTargetTag> shadowMapRT;
    static auto shadowShader         = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMap.hlsl");
    static auto skinnedShadowShader  = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/SkinnedShadowMap.hlsl");

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
    static auto ssaoShader              = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAO.cs.hlsl");
    static auto ssaoBlurShader          = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAOBlur.cs.hlsl");
    static auto bloomDownShader         = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomDownsample.cs.hlsl");
    static auto bloomUpShader           = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomUpsample.cs.hlsl");
    static auto selectionMaskShader     = resources.LoadShader("Assets/Shaders/Debug/SelectionMask.hlsl");
    static auto selectionMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskSkinnedMesh.hlsl");
    static auto selectionOutlineShader  = resources.LoadShader("Assets/Shaders/PostProcess/Outline/SelectionOutline.hlsl");
    static auto fxaaShader              = resources.LoadShader("Assets/Shaders/PostProcess/AntiAliasing/FXAA.hlsl");

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
    static auto skydomeMesh   = renderer::PrimitiveMesh::Sphere(resources, 32);

    static auto gbufferShader          = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBuffer.hlsl");
    static auto deferredLightingShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DeferredLighting.hlsl");
    static auto depthCopyShader        = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");

    static auto decalShader     = resources.LoadShader("Assets/Shaders/Material/Decal/Decal.hlsl");
    static auto decalMaskShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMask.hlsl");

    static auto particleShader      = resources.LoadShader("Assets/Shaders/Material/Effects/Particle.hlsl");
    static auto particleGpuSimCS   = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSim.cs.hlsl");
    static auto particleGpuShader  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGPU.hlsl");
    static auto trailShader    = resources.LoadShader("Assets/Shaders/Material/Effects/Trail.hlsl");
    static auto meshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/MeshTrail.hlsl");
    static auto skinnedMeshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/SkinnedMeshTrail.hlsl");
    static auto detailMeshShader      = resources.LoadShader("Assets/Shaders/Detail/Detail.hlsl");
    static auto detailBillboardShader = resources.LoadShader("Assets/Shaders/Detail/Detail.hlsl"); // 同一ソース、isBillboard フラグで切り替え
    static auto detailGrassShader     = resources.LoadShader("Assets/Shaders/Detail/DetailGrass.hlsl");
    static auto detailGrassCB         = resources.CreateConstantBuffer(sizeof(DetailGrassCB));
    static auto foliageShader         = resources.LoadShader("Assets/Shaders/Foliage/Foliage.hlsl");
    static auto detailMeshPSO   = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto detailNoCullPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto foliagePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto foliageNoCullPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });

    static auto frameCB    = resources.CreateConstantBuffer(sizeof(PerFrameCB));
    static auto objectCB   = resources.CreateConstantBuffer(sizeof(PerObjectCB));
    static auto lightCB    = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    static auto shadowCB   = resources.CreateConstantBuffer(sizeof(ShadowConstantsCB));
    static auto postprocCB = resources.CreateConstantBuffer(sizeof(PostProcCB));
    static auto outlineCB  = resources.CreateConstantBuffer(sizeof(OutlineCB));
    static auto atmCB      = resources.CreateConstantBuffer(sizeof(AtmosphereCB));
    static auto decalCB    = resources.CreateConstantBuffer(sizeof(DecalCB));

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

    const uint32_t shadowRes = rs.shadow.mapResolution;
    if (sResourceResetVersion != resources.GetResetVersion() || sShadowMapResolution != shadowRes || !shadowMapRT.IsValid()) {
        sResourceResetVersion = resources.GetResetVersion();
        sShadowMapResolution  = shadowRes;

        shadowMapRT = resources.CreateRenderTarget(shadowRes, shadowRes, 0);
        shadowShader        = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMap.hlsl");
        skinnedShadowShader = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/SkinnedShadowMap.hlsl");
        compositeShader     = resources.LoadShader("Assets/Shaders/PostProcess/Color/Composite.hlsl");
        causticsShader      = resources.LoadShader("Assets/Shaders/PostProcess/Water/Caustics.hlsl");
        ssaoShader          = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAO.cs.hlsl");
        ssaoBlurShader      = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAOBlur.cs.hlsl");
        bloomDownShader     = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomDownsample.cs.hlsl");
        bloomUpShader       = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomUpsample.cs.hlsl");
        selectionMaskShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMask.hlsl");
        selectionMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskSkinnedMesh.hlsl");
        selectionOutlineShader = resources.LoadShader("Assets/Shaders/PostProcess/Outline/SelectionOutline.hlsl");
        fxaaShader = resources.LoadShader("Assets/Shaders/PostProcess/AntiAliasing/FXAA.hlsl");
        skydomeShader = resources.LoadShader("Assets/Shaders/Material/Sky/Skydome.hlsl");
        skydomeMesh   = renderer::PrimitiveMesh::Sphere(resources, 32);
        gbufferShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBuffer.hlsl");
        deferredLightingShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DeferredLighting.hlsl");
        depthCopyShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
        decalShader = resources.LoadShader("Assets/Shaders/Material/Decal/Decal.hlsl");
        decalMaskShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMask.hlsl");
        particleShader     = resources.LoadShader("Assets/Shaders/Material/Effects/Particle.hlsl");
        particleGpuSimCS  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSim.cs.hlsl");
        particleGpuShader = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGPU.hlsl");
        trailShader = resources.LoadShader("Assets/Shaders/Material/Effects/Trail.hlsl");
        meshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/MeshTrail.hlsl");
        skinnedMeshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/SkinnedMeshTrail.hlsl");
        detailMeshShader      = resources.LoadShader("Assets/Shaders/Detail/Detail.hlsl");
        detailBillboardShader = resources.LoadShader("Assets/Shaders/Detail/Detail.hlsl");
        detailGrassShader     = resources.LoadShader("Assets/Shaders/Detail/DetailGrass.hlsl");
        detailGrassCB         = resources.CreateConstantBuffer(sizeof(DetailGrassCB));
        foliageShader         = resources.LoadShader("Assets/Shaders/Foliage/Foliage.hlsl");
        detailMeshPSO   = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID,        renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        detailNoCullPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        foliagePSO       = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID,        renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        foliageNoCullPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });

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
        frameCB    = resources.CreateConstantBuffer(sizeof(PerFrameCB));
        objectCB   = resources.CreateConstantBuffer(sizeof(PerObjectCB));
        lightCB    = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
        shadowCB   = resources.CreateConstantBuffer(sizeof(ShadowConstantsCB));
        postprocCB = resources.CreateConstantBuffer(sizeof(PostProcCB));
        outlineCB  = resources.CreateConstantBuffer(sizeof(OutlineCB));
        atmCB      = resources.CreateConstantBuffer(sizeof(AtmosphereCB));
        decalCB    = resources.CreateConstantBuffer(sizeof(DecalCB));

        defaultPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        wireframePSO = resources.CreatePipelineState({ renderer::RasterizerMode::WIREFRAME, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        selectionMaskPso = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        skydomePSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_SKY });
        particlePSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ADDITIVE, renderer::DepthMode::DEPTH_READ });
        particleAlphaPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        particleGpuPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ADDITIVE, renderer::DepthMode::DEPTH_READ });
        particleGpuAlphaPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        trailPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        meshTrailPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        meshTrailDoubleSidedPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        postprocPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF });
        causticsPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::ADDITIVE, renderer::DepthMode::DEPTH_OFF });
        decalPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_OFF });
        decalMaskPso = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF });
    }

    // パーティクルバッファ (最大描画数分を事前確保)
    static renderer::ResourceHandle<renderer::BufferTag> particleVB;
    static renderer::ResourceHandle<renderer::BufferTag> particleIB;
    static uint64_t sParticleResetVersion = 0;
    if (sParticleResetVersion != resources.GetResetVersion()) {
        particleVB = {};
        particleIB = {};
        sParticleResetVersion = resources.GetResetVersion();
    }
    constexpr uint32_t kMaxParticleVertices = static_cast<uint32_t>(kMaxParticleDraw) * 4u;
    if (!particleVB.IsValid())
    {
        particleVB = resources.CreateVertexBuffer(
            nullptr,
            kMaxParticleVertices * static_cast<uint32_t>(sizeof(ParticleVertex)),
            static_cast<uint32_t>(sizeof(ParticleVertex)));
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
    auto& customPostProcessRT     = viewTargets.customPostProcess;
    auto& gbufferRT               = viewTargets.gbuffer;
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

    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ResizeRenderTargets");
        const auto* output = resources.Get(outputRT);
        uint32_t curW = output ? output->GetWidth()  : renderer.GetWidth();
        uint32_t curH = output ? output->GetHeight() : renderer.GetHeight();
        if (curW == 0 || curH == 0) return;
        if (!hdrRT.IsValid() || sHdrW != curW || sHdrH != curH)
        {
            ReleaseViewRenderTargets(viewTargets, resources);
            hdrRT           = resources.CreateRenderTarget(curW, curH, 1);
            ldrRT           = resources.CreateRenderTarget(curW, curH, 1);
            selectionMaskRT = resources.CreateRenderTarget(curW, curH, 1);
            outlineRT       = resources.CreateRenderTarget(curW, curH, 1);
            customPostProcessRT[0] = resources.CreateRenderTarget(curW, curH, 1);
            customPostProcessRT[1] = resources.CreateRenderTarget(curW, curH, 1);
            gbufferRT       = resources.CreateRenderTarget(curW, curH, 2);
            decalDepthRT    = resources.CreateRenderTarget(curW, curH, 0);
            decalMaskRT     = resources.CreateRenderTarget(curW, curH, 1);
            bloomHalf       = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2));
            bloomFull       = resources.CreateComputeTexture(curW, curH);
            ssaoRaw         = resources.CreateComputeTexture(curW, curH);
            ssaoBlur        = resources.CreateComputeTexture(curW, curH);
            // ---- Advanced Graphics per-view テクスチャ ----
            ssrResult           = resources.CreateComputeTexture(curW, curH);
            volumetricResult    = resources.CreateComputeTexture(curW, curH);
            taaHistoryA         = resources.CreateRenderTarget(curW, curH, 1);
            taaHistoryB         = resources.CreateRenderTarget(curW, curH, 1);
            motionBlurResult    = resources.CreateComputeTexture(curW, curH);
            gtaoRaw             = resources.CreateComputeTexture(curW, curH);
            gtaoBlur            = resources.CreateComputeTexture(curW, curH);
            contactShadowResult = resources.CreateComputeTexture(curW, curH);
            sHdrW = curW;
            sHdrH = curH;
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

    constexpr float kDegToRad = 3.14159265f / 180.0f;
    for (auto [tf, lc] : scene.View<Transform, LightComponent>()) {
        if (!lc.enabled) continue;
        if (lc.type == LightComponent::Type::Directional) {
            lightData.lightDir       = tf.forward.Normalized();
            lightData.lightColor     = lc.color;
            lightData.lightIntensity = lc.intensity;
            dirCastShadows    = lc.castShadows;
            dirShadowBias     = lc.shadowBias;
            dirShadowStrength = lc.shadowStrength;
            dirShadowDistance = lc.shadowDistance;
        } else if (lc.type == LightComponent::Type::Point
                   && lightData.pointLightCount < 8) {
            auto& pl    = lightData.pointLights[lightData.pointLightCount++];
            pl.position  = tf.position;
            pl.range     = lc.range;
            pl.color     = lc.color;
            pl.intensity = lc.intensity;
        } else if (lc.type == LightComponent::Type::Spot
                   && lightData.spotLightCount < 4) {
            auto& sl    = lightData.spotLights[lightData.spotLightCount++];
            sl.position  = tf.position;
            sl.direction = tf.forward.Normalized();
            sl.range     = lc.range;
            sl.innerCos  = std::cos(lc.innerCone * kDegToRad);
            sl.outerCos  = std::cos(lc.outerCone * kDegToRad);
            sl.color     = lc.color;
            sl.intensity = lc.intensity;
        }
    }

    // ambientColor: Lit モードでは AMBIENT_SCALE 相当値、Unlit 系では白に上書き
    lightData.ambientColor = { 0.08f, 0.08f, 0.08f };
    if (rs.IsUnlit()) {
        lightData.ambientColor    = { 1.0f, 1.0f, 1.0f };
        lightData.lightIntensity  = 0.0f;
        lightData.pointLightCount = 0;
        lightData.spotLightCount  = 0;
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

    // dirShadowDistance > 0 のとき手動サイズを優先、0 のときシーンに自動フィット
    const float shadowRadius = (dirShadowDistance > 0.0f)
        ? dirShadowDistance
        : (std::max)(shadowBounds.radius, 5.0f);
    math::Vector3 lightPos = shadowBounds.center - lightDir * (shadowRadius + 20.0f);
    math::Vector3 up = (std::abs(lightDir.y) > 0.99f)
                       ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                       : math::Vector3{ 0.0f, 1.0f, 0.0f };
    math::Matrix4 lightView = math::Matrix4::LookAt(lightPos, shadowBounds.center, up);
    math::Matrix4 lightProj = math::Matrix4::Orthographic(-shadowRadius, shadowRadius,
                                                           -shadowRadius, shadowRadius,
                                                           1.0f,
                                                           shadowRadius * 2.0f + 40.0f);
    math::Matrix4 lightVP   = lightProj * lightView;

    const bool isDeferred = (rs.pipeline == renderer::RenderingPipeline::Deferred);
    const bool ssaoEnabled =
        isDeferred &&
        rs.postProcess.ambientOcclusion.enabled &&
        ssaoShader.IsValid() &&
        ssaoBlurShader.IsValid() &&
        ssaoRaw.IsValid() &&
        ssaoBlur.IsValid();

    const bool selectionOutlineEnabled =
        rs.showSelectionOutline && !rs.selectedObjects.empty() &&
        selectionMaskRT.IsValid() && selectionMaskPso.IsValid() &&
        selectionOutlineShader.IsValid();
    profiler::Profiler::EndSample();

    // =========================================================================
    // RenderPassHandles を組み立て
    // =========================================================================
    RenderPassHandles passHandles{};
    passHandles.shadowMapRT       = shadowMapRT;
    passHandles.hdrRT             = hdrRT;
    passHandles.ldrRT             = ldrRT;
    passHandles.selectionMaskRT   = selectionMaskRT;
    passHandles.outlineRT         = outlineRT;
    passHandles.customPostProcessRT[0] = customPostProcessRT[0];
    passHandles.customPostProcessRT[1] = customPostProcessRT[1];
    passHandles.gbufferRT         = gbufferRT;
    passHandles.bloomHalf         = bloomHalf;
    passHandles.bloomFull         = bloomFull;
    passHandles.ssaoRaw           = ssaoRaw;
    passHandles.ssaoBlur          = ssaoBlur;
    passHandles.ssaoShader        = ssaoShader;
    passHandles.ssaoBlurShader    = ssaoBlurShader;
    passHandles.bloomDownShader   = bloomDownShader;
    passHandles.bloomUpShader     = bloomUpShader;
    passHandles.compositeShader   = compositeShader;
    passHandles.causticsShader    = causticsShader;
    passHandles.selectionMaskShader       = selectionMaskShader;
    passHandles.selectionMaskSkinnedShader = selectionMaskSkinnedShader;
    passHandles.selectionOutlineShader    = selectionOutlineShader;
    passHandles.fxaaShader        = fxaaShader;
    passHandles.customPostProcessShaders.resize(rs.postProcess.customEffects.size());
    std::vector<uint32_t> customPostProcessIndices;
    customPostProcessIndices.reserve(rs.postProcess.customEffects.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(rs.postProcess.customEffects.size()); ++i) {
        const auto& custom = rs.postProcess.customEffects[i];
        if (!custom.enabled || custom.shaderPath.empty()) continue;
        passHandles.customPostProcessShaders[i] = resources.LoadShader(custom.shaderPath);
        if (passHandles.customPostProcessShaders[i].IsValid())
            customPostProcessIndices.push_back(i);
    }
    passHandles.selectionMaskPSO  = selectionMaskPso;
    passHandles.postprocPSO       = postprocPSO;
    passHandles.causticsPSO       = causticsPSO;
    passHandles.frameCB           = frameCB;
    passHandles.objectCB          = objectCB;
    passHandles.lightCB           = lightCB;
    passHandles.bindPoseSkinningCB = bindPoseSkinningCB;
    passHandles.postprocCB        = postprocCB;
    passHandles.outlineCB         = outlineCB;
    passHandles.decalDepthRT      = decalDepthRT;
    passHandles.decalMaskRT       = decalMaskRT;
    passHandles.decalShader       = decalShader;
    passHandles.decalMaskShader   = decalMaskShader;
    passHandles.decalPSO          = decalPSO;
    passHandles.decalMaskPSO      = decalMaskPso;
    passHandles.decalCB           = decalCB;
    // ── ジオメトリ用ハンドル ──────────────────────────────────────────────────
    passHandles.shadowShader         = shadowShader;
    passHandles.shadowSkinnedShader  = skinnedShadowShader;
    passHandles.shadowCB             = shadowCB;
    passHandles.defaultPSO           = defaultPSO;
    passHandles.wireframePSO         = wireframePSO;
    passHandles.skyShader            = skydomeShader;
    passHandles.skyPSO               = skydomePSO;
    if (skydomeMesh) {
        passHandles.skyVB         = skydomeMesh->vertexBuffer;
        passHandles.skyIB         = skydomeMesh->indexBuffer;
        passHandles.skyIndexCount = skydomeMesh->indexCount;
    }
    passHandles.atmosphereCB         = atmCB;
    passHandles.particleShader       = particleShader;
    passHandles.particlePSO          = particlePSO;
    passHandles.particleAlphaPSO     = particleAlphaPSO;
    passHandles.particleVB           = particleVB;
    passHandles.particleIB           = particleIB;
    passHandles.particleGpuSimCS     = particleGpuSimCS;
    passHandles.particleGpuShader    = particleGpuShader;
    passHandles.particleGpuAlphaShader = particleGpuShader; // 同一シェーダー、PSO で合成モードを切り替える
    passHandles.particleGpuPSO       = particleGpuPSO;
    passHandles.particleGpuAlphaPSO  = particleGpuAlphaPSO;
    passHandles.trailShader          = trailShader;
    passHandles.trailPSO             = trailPSO;
    passHandles.meshTrailShader      = meshTrailShader;
    passHandles.skinnedMeshTrailShader = skinnedMeshTrailShader;
    passHandles.meshTrailPSO         = meshTrailPSO;
    passHandles.meshTrailDoubleSidedPSO = meshTrailDoubleSidedPSO;
    passHandles.detailMeshShader      = detailMeshShader;
    passHandles.detailBillboardShader = detailBillboardShader;
    passHandles.detailGrassShader     = detailGrassShader;
    passHandles.detailGrassCB         = detailGrassCB;
    passHandles.detailMeshPSO         = detailMeshPSO;
    passHandles.detailNoCullPSO       = detailNoCullPSO;
    passHandles.foliageShader          = foliageShader;
    passHandles.foliagePSO             = foliagePSO;
    passHandles.foliageNoCullPSO       = foliageNoCullPSO;
    passHandles.gbufferShader        = gbufferShader;
    passHandles.deferredLightingShader = deferredLightingShader;
    passHandles.depthCopyShader      = depthCopyShader;

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
    passHandles.taaShader            = taaShader;
    passHandles.taaPSO               = taaPSO;
    // Motion Blur
    passHandles.motionBlurResult     = motionBlurResult;
    passHandles.motionBlurShader     = motionBlurShader;
    // GTAO
    passHandles.gtaoRaw              = gtaoRaw;
    passHandles.gtaoBlur             = gtaoBlur;
    passHandles.gtaoShader           = gtaoShader;
    passHandles.gtaoBlurShader       = gtaoBlurShader;
    // Contact Shadows
    passHandles.contactShadowResult  = contactShadowResult;
    passHandles.contactShadowShader  = contactShadowShader;
    // Lens Flare
    passHandles.lensFlareShader      = lensFlareShader;
    passHandles.lensFlarePSO         = lensFlarePSO;

    // カメラ視錐台とライト視錐台を事前に抽出する。
    // WHY: Gribb-Hartmann 法は VP 行列の各行の和・差から 6 平面を直接導出するため
    //      逆行列を使わず高速に抽出できる。全ジオメトリパスで共有する。
    const math::Frustum cameraFrustum = math::Frustum::FromViewProjection(camera.GetViewProjection());
    const math::Frustum lightFrustum  = math::Frustum::FromViewProjection(lightVP);
    OcclusionCuller occlusionCuller;

    RenderPassContext passCtx{
        scene, renderer, resources, camera, rs,
        outputRT, cullingMask, passHandles,
        sHdrW, sHdrH, selectionOutlineEnabled,
        lightData, lightVP, isDeferred, ssaoEnabled, 0.0f, 1.0f,
        &cameraFrustum, &lightFrustum, &occlusionCuller
    };
    passCtx.physicsWorld  = physicsWorld;
    // near=1.0, far=shadowRadius*2+40 の深度範囲でスケール正規化したバイアス。
    // 固定 NDC 値はシーンが広がるほど Peter Panning が悪化するため、
    // ワールド空間で約 5mm 相当の一定バイアスになるよう depthRange で除算する。
    // dirShadowBias でスケールし、Inspector から Peter Panning / アクネをチューニング可能にする。
    passCtx.shadowBiasNDC  = (0.005f * dirShadowBias) / (shadowRadius * 2.0f + 39.0f);
    passCtx.shadowStrength = dirCastShadows ? dirShadowStrength : 0.0f;

    // ---- AdvancedGraphicsCB を毎フレーム更新 ----
    // WHAT: IBL・SSR・TAA・GTAO・ContactShadow 等の詳細設定を AdvancedGraphicsCB(b8) に転送する。
    //       各パスはここで書いたデータを読むだけなので、更新はこの 1 か所に集中させる。
    if (advancedGraphicsCB.IsValid()) {
        AdvancedGraphicsCB agData{};
        // WHY: IBL が無効、または必要なキューブマップが欠けている場合は未バインド SRV を
        //      サンプルさせず、従来の ambient ライティングへ確実にフォールバックする。
        const bool iblResourcesReady = rs.ibl.enabled && rs.HasValidIblAssets() &&
            passHandles.iblIrradiance.IsValid() && passHandles.iblPrefilter.IsValid();
        agData.iblIntensity          = iblResourcesReady ? rs.ibl.intensity : 0.0f;
        agData.iblDiffuseScale       = rs.ibl.diffuseScale;
        agData.iblSpecularScale      = rs.ibl.specularScale;
        agData.iblMaxMipLevel        = rs.ibl.maxMipLevel;
        agData.ssrMaxDistance        = rs.ssr.maxDistance;
        agData.ssrThickness          = rs.ssr.thickness;
        agData.ssrSteps              = rs.ssr.steps;
        agData.ssrIntensity          = rs.ssr.enabled ? rs.ssr.intensity : 0.0f;
        agData.volLightIntensity     = rs.volumetricLight.enabled ? rs.volumetricLight.intensity : 0.0f;
        agData.volScattering         = rs.volumetricLight.scattering;
        agData.volSteps              = rs.volumetricLight.steps;
        agData.volMaxDist            = rs.volumetricLight.maxDist;
        agData.taaFeedback           = rs.taa.feedback;
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
        // 前フレームの VP 行列 — TAA / Motion Blur が深度再投影で使用する。
        // WHY: viewTargets に持つことでビュー別に分離し、SceneView と GameView が
        //      互いのカメラ行列を参照して壊れる問題を防ぐ。
        agData.prevViewProjection    = viewTargets.prevViewProjection;
        agData.invPrevViewProjection = viewTargets.invPrevViewProjection;
        resources.Update(advancedGraphicsCB, &agData, sizeof(AdvancedGraphicsCB));
        viewTargets.prevViewProjection    = camera.GetViewProjection();
        viewTargets.invPrevViewProjection = math::Matrix4::Inverse(camera.GetViewProjection());
    } // end AdvancedGraphicsCB update

    // =========================================================================
    // RenderPipeline にパスを登録
    // =========================================================================
    RenderPipeline pipeline;
    profiler::Profiler::BeginSample(
        profiler::ProfilerMarker("RenderSystem::BuildPipeline", "Rendering"));
    pipeline.DeclareResource("Output",     { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, true,  false });
    pipeline.DeclareResource("ShadowMap",  { renderer::RenderGraph::ResourceKind::RenderTarget, rs.shadow.mapResolution, rs.shadow.mapResolution, 0, false, false });
    pipeline.DeclareResource("HDR",        { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("LDR",        { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("SelectionMask", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("Outline",    { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("SceneColor", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("CustomPostProcess0", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("CustomPostProcess1", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("Bloom",      { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, 0, false, true });
    if (isDeferred)
        pipeline.DeclareResource("GBuffer", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, false });
    if (ssaoEnabled)
        pipeline.DeclareResource("SSAO",               { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, 0, false, true });
    // GTAO / ContactShadows は GBuffer を読んで独自の UAV テクスチャに書く。
    // WHY: "GBuffer"→"GBuffer" で宣言すると GBuffer への偽書き込みとみなされ、
    //      DeferredLighting との依存順が崩れる可能性があるため専用名で宣言する。
    if (isDeferred && rs.IsGtaoActive())
        pipeline.DeclareResource("GTAOResult",          { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, 0, false, true });
    if (isDeferred && rs.contactShadow.enabled)
        pipeline.DeclareResource("ContactShadowResult", { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, 0, false, true });
    pipeline.SetOutputs({ "Output" });

    // IBL BRDF LUT 焼き付け — IBLBakePass が対象テクスチャの世代を追跡し、初回のみ実行する。
    // WHY: 512x512 の積分テーブルはシーン・設定に依存しない定数。毎フレーム実行するのは無駄なため
    //      内部フラグでガードし、ここは毎フレーム呼ぶが実処理は初回のみ走る。
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

    // ── Shadow ────────────────────────────────────────────────────────────────
    pipeline.AddRawPass("Shadow", {}, { "ShadowMap" }, [&]() {
        ExecuteShadowPass(passCtx);
    });

    // ── Forward or Deferred ───────────────────────────────────────────────────
    if (!isDeferred) {
        pipeline.AddRawPass("ForwardOpaque", { "ShadowMap" }, { "HDR" }, [&]() {
            ExecuteForwardPasses(passCtx);
        });
    }

    if (isDeferred) {
        pipeline.AddRawPass("DeferredGBuffer", { "ShadowMap" }, { "GBuffer" }, [&]() {
            ExecuteGBufferPass(passCtx);
        });

        pipeline.AddRawPass("DeferredDepthCopy", { "GBuffer" }, { "HDR" }, [&]() {
            ExecuteDeferredDepthCopyPass(passCtx);
        });
    }

    // ── Terrain / Detail / Foliage ────────────────────────────────────────────
    // ForwardOpaque / GBuffer DepthCopy の後・Sky の前に描画する。
    // WHY: Sky より前に描画することで地形の上に空が被らない。
    //      ForwardOpaque と同じ HDR RT (depth buffer 共有) で描画することで
    //      Player 等の不透明オブジェクトと正しく depth test される。
    //      RenderSystem 内に統合することでポストプロセス（bloom/SSAO等）も適用される。
    pipeline.AddPass<TerrainRenderPass>();
    pipeline.AddPass<DetailRenderPass>();
    pipeline.AddPass<FoliageRenderPass>();

    pipeline.AddRawPass("Sky", { "HDR" }, { "HDR" }, [&]() {
        ExecuteSkyPass(passCtx);
    });

    // ── SSAO + Deferred Lighting ──────────────────────────────────────────────
    if (isDeferred) {
        // GTAO — DeferredLighting より前に GBuffer から AO を計算する。
        // WHY: DeferredLighting は t23 (TEX_GTAO) を SRV として読む。
        //      "GTAOResult" として宣言することで GBuffer への偽書き込みを除去し、
        //      DeferredLighting が正確な依存でこの出力を待てるようにする。
        if (rs.IsGtaoActive()) {
            pipeline.AddRawPass("GTAO", { "GBuffer" }, { "GTAOResult" }, [&]() {
                ExecuteGTAOPass(passCtx);
            });
        }
        // ContactShadows — DeferredLighting より前に深度から接触影マスクを生成する。
        // WHY: GTAO と同様に ContactShadowResult として宣言し偽依存を除去する。
        if (rs.contactShadow.enabled) {
            pipeline.AddRawPass("ContactShadows", { "GBuffer" }, { "ContactShadowResult" }, [&]() {
                ExecuteContactShadowsPass(passCtx);
            });
        }
        if (ssaoEnabled) {
            pipeline.AddRawPass("SSAO", { "GBuffer" }, { "SSAO" }, [&]() {
                ExecuteSSAOPass(passCtx);
            });
        }
        // DeferredLighting の reads を動的に構築し、GTAO/ContactShadows/SSAO が有効な
        // ときだけその出力への依存を宣言する。
        // WHY: 静的な reads 文字列では有効/無効の組み合わせごとに分岐が必要になり、
        //      将来の AO 種類追加時にも変更が局所化されない。
        {
            using RA = renderer::RenderGraph::ResourceAccess;
            using RU = renderer::RenderGraph::ResourceUsage;
            std::vector<RA> deferredAccesses = {
                { "GBuffer", RU::Read     },
                { "HDR",     RU::ReadWrite }, // 深度を読み、ライティング結果を書く
            };
            if (ssaoEnabled)              deferredAccesses.push_back({ "SSAO",               RU::Read });
            if (rs.IsGtaoActive())        deferredAccesses.push_back({ "GTAOResult",          RU::Read });
            if (rs.contactShadow.enabled) deferredAccesses.push_back({ "ContactShadowResult", RU::Read });
            pipeline.AddRawPass("DeferredLighting", std::move(deferredAccesses), [&]() {
                ExecuteDeferredLightingPass(passCtx);
            });
        }

        pipeline.AddRawPass("DeferredSkinnedForward", { "HDR" }, { "HDR" }, [&]() {
            ExecuteDeferredSkinnedForwardPass(passCtx);
        });

        pipeline.AddRawPass("DeferredForwardTransparent", { "HDR" }, { "HDR" }, [&]() {
            ExecuteDeferredForwardTransparentPass(passCtx);
        });

        // SSR — 全透明オブジェクト描画後に GBuffer の法線・深度・金属度を使って反射を計算する。
        // WHY: Deferred パイプラインでのみ有効 (GBuffer 必須)。
        //      透明オブジェクト通過後の深度を使うため、DeferredForwardTransparent の後に配置する。
        if (rs.ssr.enabled) {
            pipeline.AddRawPass("SSR", { "GBuffer", "HDR" }, { "HDR" }, [&]() {
                ExecuteSSRPass(passCtx);
            });
        }
    }

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
            entry.script->OnSetupRenderPasses(pipeline, passCtx);
        }
    }

    appendQueuedUserPasses(UserRenderPassInjectionPoint::AfterOpaque);

    // ── デカール用深度スナップショット ────────────────────────────────────────
    pipeline.DeclareResource("DecalDepth", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.AddRawPass("DecalDepthCopy", { isDeferred ? "GBuffer" : "HDR" }, { "DecalDepth" }, [&]() {
        renderer.SetRenderTarget(decalDepthRT, resources);
        renderer.ClearDepth();
        if (depthCopyShader.IsValid()) {
            renderer::DrawCall dc;
            dc.shader        = depthCopyShader;
            dc.pipelineState = defaultPSO;
            dc.vertexCount   = 3;
            dc.textures[7]   = isDeferred
                ? resources.GetDepthTexture(gbufferRT)
                : resources.GetDepthTexture(hdrRT);
            renderer.Submit(dc, resources);
        }
    });

    // ── Decal + Trail + Particle ──────────────────────────────────────────────
    pipeline.AddRawPass("Decal", { "HDR", "DecalDepth" }, { "HDR" }, [&]() {
        ExecuteDecalPass(passCtx);
    });

    pipeline.AddPass<MeshTrailRenderPass>();
    pipeline.AddPass<TrailRenderPass>();

    pipeline.AddRawPass("Particle", { "HDR" }, { "HDR" }, [&]() {
        ExecuteParticlePass(passCtx);
    });

    appendQueuedUserPasses(UserRenderPassInjectionPoint::AfterTransparent);

    // ── Selection / Debug ─────────────────────────────────────────────────────
    pipeline.AddRawPass("UnderwaterCaustics", { "HDR" }, { "HDR" }, [&]() {
        ExecuteCausticsPass(passCtx);
    });

    if (selectionOutlineEnabled) {
        pipeline.AddRawPass("SelectionMask", { "HDR" }, { "SelectionMask" }, [&]() {
            ExecuteSelectionMaskPass(passCtx);
        });
    }

    pipeline.AddPass<DebugCollidersPass>();
    pipeline.AddPass<ConstraintDebugPass>();
    pipeline.AddPass<AnimatorDebugPass>();
    pipeline.AddPass<GridDebugPass>();
    pipeline.AddPass<LightRangeDebugPass>();
    pipeline.AddPass<TerrainCollisionDebugPass>();

    pipeline.AddRawPass("ScriptDebugDraw", { "HDR" }, { "HDR" }, [&]() {
        scene.TickScriptDebugDrawCommands(Time::deltaTime);
        renderer::DebugDraw::BeginFrame(passCtx.renderer, passCtx.resources, passCtx.camera.GetViewProjection());

        // OnDrawGizmos: DebugDraw::BeginFrame/Flush の区間内で Script が gizmo プロキシを使って
        // 視野錐・ウェイポイント・検知範囲などを直接描画する。
        // WHY: コマンドキュー経由の ScriptDebugProxy と異なり、Gizmo の複合プリミティブは
        //      レンダリング区間内で直接呼ぶ必要があるため、専用コールバックを設ける。
        for (EntityID id : scene.GetEntities<ScriptComponent>()) {
            auto* sc = scene.GetComponent<ScriptComponent>(id);
            auto* go = scene.GetGameObject(id);
            if (!sc || !go) continue;
            for (auto& entry : sc->scripts) {
                if (!entry.script || !entry.script->enabled) continue;
                entry.script->SetContext(&scene, go);
                entry.script->gizmo.renderer = &passCtx.renderer;
                entry.script->OnDrawGizmos();
                entry.script->gizmo.renderer = nullptr;
            }
        }

        for (const auto& command : scene.GetScriptDebugDrawCommands()) {
            switch (command.type) {
            case ScriptDebugDrawType::Line:
                renderer::DebugDraw::Line(passCtx.renderer, command.a, command.b, command.color);
                break;
            case ScriptDebugDrawType::Sphere:
                renderer::DebugDraw::Sphere(passCtx.renderer, command.a, command.radius, command.color);
                break;
            case ScriptDebugDrawType::Box:
                renderer::DebugDraw::Box(passCtx.renderer, command.a, command.halfExtents, command.color);
                break;
            case ScriptDebugDrawType::Ray:
                renderer::DebugDraw::Line(passCtx.renderer, command.a, command.b, command.color);
                break;
            case ScriptDebugDrawType::Arrow:
                // headLength = radius, headRadius = halfExtents.x
                renderer::DebugDraw::Arrow(passCtx.renderer, command.a, command.b,
                                           command.radius, command.halfExtents.x, command.color);
                break;
            case ScriptDebugDrawType::Cone:
                // direction = b, height = halfExtents.x, baseRadius = radius
                renderer::DebugDraw::Cone(passCtx.renderer, command.a, command.b,
                                          command.halfExtents.x, command.radius, command.color);
                break;
            }
        }
        renderer::DebugDraw::Flush();
    });

    pipeline.AddPass<NavMeshDebugPass>();
    pipeline.AddPass<DecalDebugPass>();

    appendQueuedUserPasses(UserRenderPassInjectionPoint::BeforePostProcess);

    // ── PostProcess チェーン ──────────────────────────────────────────────────
    // MotionBlur CS — HDR 空間でカメラモーションブラーを計算し motionBlurResult に書く。
    // WHY: Composite パスが motionBlurResult を hdrRT の代わりに読む。
    //      Bloom の前に走らせることで blur 後の輝度が Bloom に乗る。
    if (rs.motionBlur.enabled) {
        pipeline.AddRawPass("MotionBlur", { "HDR" }, { "HDR" }, [&]() {
            ExecuteMotionBlurPass(passCtx);
        });
    }
    // VolumetricLight CS — ゴッドレイ・光柱を HDR バッファに加算合成する。
    // WHY: Bloom の前に配置することで体積光が Bloom に乗り、より明るい光の広がりが出る。
    if (rs.volumetricLight.enabled) {
        pipeline.AddRawPass("VolumetricLight", { "HDR", "ShadowMap" }, { "HDR" }, [&]() {
            ExecuteVolumetricLightPass(passCtx);
        });
    }
    // LensFlare PS — bloomHalf を光源ソースとして ADDITIVE に HDR に合成する。
    // WHY: bloomHalf は既に輝度抽出済みで hdrRT とは別リソースなので SRV/RTV 競合しない。
    //      Bloom の前に配置することでフレアも Bloom に乗る。
    if (rs.lensFlare.enabled) {
        pipeline.AddRawPass("LensFlare", { "HDR" }, { "HDR" }, [&]() {
            ExecuteLensFlarePass(passCtx);
        });
    }
    if (rs.postProcess.bloom.enabled) {
        pipeline.AddRawPass("Bloom", { "HDR" }, { "Bloom" }, [&]() {
            ExecuteBloomPass(passCtx);
        });
    }

    const bool customPostProcessEnabled =
        !customPostProcessIndices.empty() &&
        customPostProcessRT[0].IsValid() &&
        customPostProcessRT[1].IsValid();
    // hasPostCompositeEffects: Composite の出力先が "LDR" か "Output" かを決める。
    // WHY: このフラグが true なら Composite は ldrRT に書き、後続エフェクトがチェーンを形成する。
    const bool hasPostCompositeEffects =
        rs.IsTaaActive() || customPostProcessEnabled || selectionOutlineEnabled || rs.postProcess.fxaaEnabled;

    if (rs.postProcess.bloom.enabled) {
        pipeline.AddRawPass("Composite", { "HDR", "Bloom" }, { hasPostCompositeEffects ? "LDR" : "Output" }, [&]() {
            ExecuteCompositePass(passCtx);
        });
    } else {
        pipeline.AddRawPass("Composite", { "HDR" }, { hasPostCompositeEffects ? "LDR" : "Output" }, [&]() {
            ExecuteCompositePass(passCtx);
        });
    }

    // ---- Post-composite チェーン ----
    // ppCurrent: 「LDR 空間の最新フレームを持つ RenderGraph リソース名」を追跡する。
    // WHY: エフェクトごとに条件分岐でリソース名を手動管理する代わりに ppCurrent を
    //      進めることで、TAA/CustomPP/SelectionOutline/FXAA の組み合わせを
    //      単一の直列チェーンとして表現できる。
    std::string ppCurrent  = hasPostCompositeEffects ? "LDR" : "Output";
    int         ppPingPong = 0; // customPostProcessRT の ping-pong インデックス

    // TAA — 最初に適用することで後続の CustomPP/SelectionOutline が TAA 済み映像に乗る。
    // WHY: CustomPP より前に登録することで RenderGraph が TAA → CustomPP の
    //      依存順を正しく解決する (Kahn's algorithm は登録順をタイブレークに使う)。
    if (rs.IsTaaActive()) {
        pipeline.AddRawPass("TAA", { ppCurrent }, { ppCurrent }, [&]() {
            ExecuteTAAPass(passCtx);
            // taaFlip は ExecuteTAAPass 内で反転済み — 反転後のフラグで「書いた方」を特定する。
            auto& taaOut = passHandles.taaFlip ? passHandles.taaHistoryB : passHandles.taaHistoryA;
            passHandles.fxaaInput = resources.GetColorTexture(taaOut, 0);
            // WHY: CustomPP / SelectionOutline は postProcessInput を参照する。
            //      TAA 後は履歴バッファが最新フレームなので postProcessInput も更新する。
            //      更新しないと後続エフェクトが TAA 適用前の ldrRT を誤読する。
            passHandles.postProcessInput = passHandles.fxaaInput;
        });
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
            ? "Output"
            : ("CustomPostProcess" + std::to_string(outputIndex));
        pipeline.AddRawPass(
            "CustomPostProcess" + std::to_string(i),
            { ppCurrent },
            { outRes },
            [&, customIndex, outputIndex]() {
                ExecuteCustomPostProcessPass(passCtx, customIndex, outputIndex);
            });
        ppCurrent = outRes;
        ++ppPingPong;
    }

    // SelectionOutline
    if (selectionOutlineEnabled) {
        const bool        isLastEffect = !rs.postProcess.fxaaEnabled;
        const std::string outRes       = isLastEffect ? "Output" : "Outline";
        pipeline.AddRawPass("SelectionOutline",
            { ppCurrent, "SelectionMask" },
            { outRes },
            [&]() { ExecuteSelectionOutlinePass(passCtx); });
        ppCurrent = outRes;
    }

    // FXAA
    if (rs.postProcess.fxaaEnabled) {
        pipeline.AddRawPass("FXAA", { ppCurrent }, { "Output" }, [&]() { ExecuteFxaaPass(passCtx); });
        ppCurrent = "Output";
    }

    // TAA_Blit — TAA が有効で後続エフェクトが何もない場合のみ OutputRT への転送が必要。
    // WHY: TAA は ping-pong 履歴バッファにのみ書き OutputRT には書かない。
    //      FXAA/SelectionOutline/CustomPP がすべて無効のとき ppCurrent は "LDR" のままなので
    //      ここで Output に届ける。ppCurrent が "Output" なら既に書かれているためスキップ。
    if (ppCurrent != "Output") {
        pipeline.AddRawPass("TAA_Blit", { ppCurrent }, { "Output" }, [&]() {
            ExecuteTAABlitPass(passCtx);
        });
    }

    if (uiOptions && uiOptions->enabled && uiOptions->context) {
        pipeline.AddRawPass(
            "UIPass",
            { { "Output", renderer::RenderGraph::ResourceUsage::ReadWrite } },
            [&]() {
                // WHY: UI は最終フレームへの合成であり、Composite / FXAA / CustomPostProcess の
                //      どの分岐が最後に Output を書いたかに依存してはいけない。
                //      Output を ReadWrite する RenderGraph pass として登録し、この pass 内で
                //      明示的に outputRT をバインドすることで、Game / Scene / UI Viewport の
                //      いずれでも同じ順序と同じ RT に描画できる。
                renderer.SetRenderTarget(outputRT, resources);
                const float uiWidth = uiOptions->viewportWidth > 0.0f
                    ? uiOptions->viewportWidth
                    : static_cast<float>(sHdrW);
                const float uiHeight = uiOptions->viewportHeight > 0.0f
                    ? uiOptions->viewportHeight
                    : static_cast<float>(sHdrH);
                UISystem(scene,
                         renderer,
                         resources,
                         *uiOptions->context,
                         uiWidth,
                         uiHeight,
                         uiOptions->mouseInCanvasSpace,
                         uiOptions->mousePressed,
                         camera.m_position,
                         camera.m_rotation,
                         camera.GetViewProjection(),
                         uiOptions->targetView);
            });
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
                entry.script->OnPreRender();
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

    const bool graphExecuted = pipeline.Execute(passCtx);

    renderer.GpuProfEndFrame();
    assert(graphExecuted);
    (void)graphExecuted;

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
                entry.script->OnPostRender();
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
        for (const auto& profile : pipeline.LastReport().profiles)
            dbgSnap.passTimings.push_back({ profile.name, profile.cpuMilliseconds });

        // GPU 計測結果を Snapshot に詰める。QUERY_LATENCY フレーム以内は空になる。
        for (const auto& gp : renderer.GpuProfGetResults())
            dbgSnap.gpuPassTimings.push_back({ gp.name, gp.gpuMs });

        // カリング統計を Snapshot に詰める
        dbgSnap.renderStats.totalObjects    = passCtx.statsTotalObjects;
        dbgSnap.renderStats.frustumCulled   = passCtx.statsFrustumCulled;
        dbgSnap.renderStats.occlusionCulled = passCtx.statsOcclusionCulled;
        dbgSnap.renderStats.drawCalls       = passCtx.statsDrawCalls;
        dbgSnap.renderStats.vertexCount     = passCtx.statsVertexCount;
        dbgSnap.renderStats.triangleCount   = passCtx.statsTriangleCount;

        renderer::RenderDebugOverlay::UpdateSnapshot(dbgSnap, rs.passViewerEnabled);
    }
}

} // namespace fbzz::scene
