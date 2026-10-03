/// @file    RenderSystem.cpp
/// @brief   Scene から DrawCall を生成するオーケストレーター。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note 各描画パスの実装は RenderPasses/ 以下の Execute*Pass 関数に委譲する。
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include "Engine/Scene/Systems/RenderSystem.hpp"
#include "Engine/Scene/SceneUtils.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/MeshTrailRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/TrailRenderPass.hpp"
#include "Engine/Renderer/RenderSettings.hpp"
#include <Engine/Renderer/OpaqueRenderPlan.hpp>
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include <Engine/Scene/Systems/RenderLightExtractor.hpp>
#include <Engine/Scene/Systems/RenderPasses/GeometryPipeline.hpp>
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
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Pipeline/ViewPipeline.hpp>
#include <Graphics/Pipeline/ViewPreparation.hpp>
#include <Engine/Scene/Systems/RenderPasses/InstanceBatch.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include "RenderPasses/Debug/SelectionPasses.hpp"
#include "Engine/Core/Application.hpp"
#include "Engine/Core/Time.hpp"
#include <Engine/Core/DeveloperMode.hpp>
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

static_assert(asset::MAX_SKINNING_BONES == renderer::RENDER_SKINNING_BONES);

namespace {

using SceneShadowBounds = renderer::ShadowBounds;

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

} /// @note namespace

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
                  RenderPassCapture* capture,
                  RenderFrameGeometryCache* frameGeometry)
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

    const renderer::RenderSettings& rs = effectiveSettings;
    const bool experimentalRayTracingEnabled = core::DeveloperMode::IsEnabled();

    auto& renderResources = resources.Rendering();
    renderResources.SetExperimentalRayTracingEnabled(experimentalRayTracingEnabled);
    auto& shared = renderResources.Shared();
    if (shared.Prepare(resources, rs)) {
        ReleaseSkinningComputeCaches();
        ReleaseInstanceBatchCaches(resources);
        ReleaseDecalMaterialCache();
        ReleaseCustomPassMaterialCache();
    }
    const uint32_t viewKey = uiOptions ? static_cast<uint32_t>(uiOptions->targetView) + 1u : 0u;
    auto& viewTargets = renderResources.View(viewKey);
    if (!renderResources.PrepareView(viewTargets, renderer, outputRT, rs)) {
        const auto failedPlan = renderer::PrepareViewRenderPlan(resources, renderer, rs,
            viewTargets, shared, {}, experimentalRayTracingEnabled);
        FBZZ_LOG_ERROR("RenderSystem: %s", renderer::DescribeRenderPlanReason(failedPlan.failureReason));
        return;
    }
    const uint32_t nativeW = viewTargets.nativeWidth;
    const uint32_t nativeH = viewTargets.nativeHeight;
    const bool needsUpscale = viewTargets.needsUpscale;
    const uint32_t punctualShadowRes = (std::max)(rs.shadow.punctualMapResolution, 64u);
    auto& hdrRT                   = viewTargets.hdr;
    auto& ldrRT                   = viewTargets.ldr;
    auto& selectionMaskRT         = viewTargets.selectionMask;
    auto& outlineRT               = viewTargets.outline;
    auto& objectMaskRT           = viewTargets.objectMask;
    auto& upscaleSrcRT            = viewTargets.upscaleSrc;
    auto& gbufferRT               = viewTargets.gbuffer;
    auto& ssaoRaw                 = viewTargets.ssaoRaw;
    auto& ssaoBlur                = viewTargets.ssaoBlur;
    auto& ssrResult               = viewTargets.ssrResult;
    auto& gtaoBlur                = viewTargets.gtaoBlur;
    auto& contactShadowResult     = viewTargets.contactShadowResult;
    uint32_t& sHdrW               = viewTargets.width;
    uint32_t& sHdrH               = viewTargets.height;
    for (auto& go : scene.GameObjects()) {
        if (auto* smr = go.GetComponent<SkinnedMeshRenderer>(); smr && smr->model)
            if (!resources.Get(smr->model->referencePoseCB)) smr->model->referencePoseCB = {};
    }
    auto& ssaoShader = shared.ssaoShader;
    auto& ssaoBlurShader = shared.ssaoBlurShader;
    auto& selectionOutlineShader = shared.selectionOutlineShader;
    auto& objectMaskShader = shared.objectMaskShader;
    auto& objectMaskSkinnedShader = shared.objectMaskSkinnedShader;
    auto& ssrShader = shared.ssrShader;
    auto& sEnvironmentResources = shared.sEnvironmentResources;
    auto& clusterCB = shared.clusterCB;
    auto& clusterLinearCB = shared.clusterLinearCB;
    auto& selectionMaskPso = shared.selectionMaskPso;

    /// @note ライト収集とシャドウ範囲計算はシーン全体を走査するため、独立して計測する。
    profiler::Profiler::BeginSample(
        profiler::ProfilerMarker("RenderSystem::LightingSetup", "Rendering"));

    auto lighting = ExtractRenderLights(scene, camera, rs, punctualShadowRes, experimentalRayTracingEnabled);
    auto& lightData = lighting.lightData;
    auto& dirCastShadows = lighting.dirCastShadows;
    auto& dirShadowBias = lighting.dirShadowBias;
    auto& dirShadowStrength = lighting.shadowStrength;
    auto& dirShadowDistance = lighting.dirShadowDistance;
    auto& punctualLights = lighting.punctualLights;
    auto& legacySourceRadius = lighting.legacySourceRadius;
    auto& punctualViews = lighting.punctualShadowViews;
    auto& punctualViewCount = lighting.punctualShadowViewCount;
    auto& legacyShadowSlots = lighting.legacyShadowSlots;
    auto& cookieViews = lighting.lightCookieViews;
    auto& cookieViewCount = lighting.lightCookieViewCount;
    auto& legacyCookieSlots = lighting.legacyCookieSlots;
    auto& skyCloudShadowStrength = lighting.cloudShadowStrength;
    auto& skyCloudShadowCoverage = lighting.cloudShadowCoverage;
    auto& skyCloudShadowScale = lighting.cloudShadowScale;
    auto& skyCloudShadowSpeed = lighting.cloudShadowSpeed;
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

    const auto shadows = renderer::PrepareShadows(camera, rs, shadowBounds, lightDir,
        dirShadowDistance, dirShadowBias);
    const auto& cascades = shadows.cascades;
    const int cascadeCount = shadows.count;
    const auto& lightVP = shadows.lightVP;
    const auto& lightView = shadows.lightView;
    const auto& lightPos = shadows.lightPos;

    RenderPassHandles passHandles{};
    renderResources.BindPassHandles(viewTargets, passHandles);
    const auto renderPlan = renderer::PrepareViewRenderPlan(
        resources, renderer, rs, viewTargets, shared, passHandles, experimentalRayTracingEnabled);
    if (!renderPlan.IsValid()) {
        profiler::Profiler::EndSample();
        FBZZ_LOG_ERROR("RenderSystem: %s", renderer::DescribeRenderPlanReason(renderPlan.failureReason));
        return;
    }
    const auto& opaquePlan = renderPlan.rasterPlan;
    const bool screenSpaceReady = opaquePlan.HasScreenSpaceInputs();

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
    /// @note IBL キューブマップ: RenderSettings に指定されたパスを毎フレーム LoadTexture でキャッシュ参照する。
    /// @note LoadTexture は内部でキャッシュするため、毎フレーム呼んでも I/O は初回のみ。
    if (!rs.ibl.irradiancePath.empty())
        passHandles.iblIrradiance = asset::StreamedTextureResolver::Engine().ResolveGpu(resources, rs.ibl.irradiancePath);
    if (!rs.ibl.prefilterPath.empty())
        passHandles.iblPrefilter  = asset::StreamedTextureResolver::Engine().ResolveGpu(resources, rs.ibl.prefilterPath);
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
    passCtx.experimentalRayTracingEnabled = experimentalRayTracingEnabled;
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
    /// @note 構成側の opaquePlan.HasScreenSpaceInputs() と
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
    passCtx.isDeferred              = opaquePlan.UsesDeferredLighting();
    passCtx.ssaoEnabled             = ssaoEnabled;
    passCtx.gbufferDepthReady       = screenSpaceReady;

    /// @name 前方描画のマテリアルへ渡す画面空間の遮蔽
    /// @note GTAO と SSAO は排他 (IsGtaoActive が解決済み)。走った方を 1 つのスロットへ入れる。
    /// @note Deferred でも渡すのは、半透明・エフェクト・スキンドが Forward で描かれ
    /// @note DeferredLighting を通らないため。本体は共有ヘッダーを外しているので二重適用にならない。
    /// @note 強度は b8 が運ぶが、あれを組むのは IBL 解決後なのでここでは値だけ決める。
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
    /// @note opaquePlan はライトの供給方法と直交する軸なので触らない。
    const bool punctualBufferReady =
        passHandles.punctualLightBuffer.IsValid() && clusterCB.IsValid();
    /// @note クラスタで絞れるか。カリング CS とインデックスバッファが揃って初めて成立する。
    const bool canCullClusters = renderPlan.clusteredLighting;

    ClusterLightMode clusterMode = ClusterLightMode::Legacy;
    if (punctualBufferReady && !rs.IsUnlit()) {
        clusterMode = canCullClusters ? ClusterLightMode::Clustered
                                      : ClusterLightMode::Linear;
    }
    const bool clusteredEnabled = (clusterMode == ClusterLightMode::Clustered);

    passCtx.punctualLights    = std::move(punctualLights);
    passCtx.rayLights = std::move(lighting.rayLights);
    passCtx.rayLightsComplete = lighting.rayLightsComplete;
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

    /// @note プローブも同じ入力を読むため、グラフ外の捕捉より前に変形と抽出を完了する。
    ExecuteSkinningComputePass(passCtx);
    ExtractRenderScene(passCtx, scene.GetEntities<ScriptComponent>().empty() ? frameGeometry : nullptr);

    /// @name 空連動 IBL: source=DynamicSky のとき空→動的 IBL を用意する
    /// @note AdvancedGraphicsCB / 各 Lit パスより前に焼くことで同フレームで消費できる。
    /// @note キャプチャ先と畳み込み出力は RenderGraph 管理外なのでグラフ実行前に直接呼ぶ。
    /// @note SkyCapture / SkyLightBake は dirty を内部判定し、不要フレームは即 return する。
    bool dynamicIblReady = false;
    bool reflectionProbeSelected = false;
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
            passCtx.iblIrradiancePublication = {sEnvironmentResources.immutableIrradiance, sEnvironmentResources.immutableIblOwner,
                sEnvironmentResources.immutableIblEpoch};
            passCtx.iblPrefilterPublication = {sEnvironmentResources.immutablePrefilter, sEnvironmentResources.immutableIblOwner,
                sEnvironmentResources.immutableIblEpoch};
        }
    }

    /// @note 局所 Reflection Probe はカメラが影響範囲内にいるとき、グローバル IBL より優先する。
    /// @note IBL スロットを共有するので、マテリアル側に専用分岐も追加テクスチャも要らない。
    if (auto* localProbe = ExecuteReflectionProbeCapturePass(passCtx)) {
        reflectionProbeSelected = true;
        passHandles.iblIrradiance = localProbe->runtimeIrradiance;
        passHandles.iblPrefilter  = localProbe->runtimePrefilter;
        dynamicIblReady = true;
        reflectionProbeIntensity = localProbe->intensity;
        dynamicIblMipCount = static_cast<int>(localProbe->runtimePrefilterMipCount);
        passCtx.iblIrradiancePublication = {};
        passCtx.iblPrefilterPublication = {};
        if (localProbe->runtimeImmutablePublished) {
            passCtx.iblIrradiancePublication = {localProbe->runtimePublishedIrradiance, localProbe->runtimePublicationOwner,
                localProbe->runtimePublicationEpoch};
            passCtx.iblPrefilterPublication = {localProbe->runtimePublishedPrefilter, localProbe->runtimePublicationOwner,
                localProbe->runtimePublicationEpoch};
        }
    }

    const ActiveWeather weather = FindActiveWeather(scene);
    renderer::PrepareAdvancedConstants(passCtx, viewTargets,
        { dynamicIblReady, reflectionProbeIntensity, dynamicIblMipCount,
          screenAoStrength, screenContactShadowStrength,
          weather.wetness, weather.darkening, weather.puddleAmount, reflectionProbeSelected },
        [&](AdvancedGraphicsCB& agData) {
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
        });

    RenderPipeline pipeline(std::move(viewTargets.pipeline));
    renderer::DebugDrawCapture scriptGizmoCapture;
    renderer::ViewPipelineExtensions extensions;
    extensions.selectionMask = [&]() { pipeline.AddPass<SelectionMaskPass>(); };
    extensions.selectionOutline = [&](renderer::PassResources& res) { ExecuteSelectionOutlinePass(res, passCtx); };
    extensions.begin = [&]() { scene.ClearUserRenderPasses(); };
    extensions.setup = [&]() {
        for (EntityID id : scene.GetEntities<ScriptComponent>()) {
            auto* sc = scene.GetComponent<ScriptComponent>(id);
            auto* go = scene.GetGameObject(id);
            if (!sc || !go || !go->activeInHierarchy())
                continue;
            for (auto& entry : sc->scripts) {
                if (!entry.script || !entry.script->enabled)
                    continue;
                entry.script->SetContext(&scene, go);
                entry.script->ExecuteProfiledCallback(&Script::OnSetupRenderPasses, pipeline, passCtx, ScriptCallbackKind::SETUP_RENDER_PASSES, "OnSetupRenderPasses");
            }
        }
    };
    extensions.userPasses = [&](UserRenderPassInjectionPoint injectionPoint) {
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
    extensions.depthDebug = [&]() {
        scene.TickScriptDebugDrawCommands(Time::deltaTime);
        CaptureScriptGizmos(passCtx, scriptGizmoCapture);
        pipeline.AddPass<GridDebugPass>();
        pipeline.AddPass<NavMeshDebugPass>();
        pipeline.AddPass<ScriptGizmoPass>("HDR", "ScriptGizmosDepth",
                                          renderer::DebugDrawLayer::DepthTested, scriptGizmoCapture);
    };
    extensions.overlayDebug = [&](const char* chainOutRes) {
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
    };
    extensions.ui = [&]() {
        if (uiOptions && uiOptions->enabled && uiOptions->context) {
            pipeline.AddRawPass(
                "UIPass",
                { { "Output", renderer::RenderGraph::ResourceUsage::ReadWrite } },
                [&]() { ExecuteUIPass(passCtx); });
        }
    };
    renderer::BuildViewPipeline(pipeline, passCtx, viewTargets, shared,
        { renderPlan, customAfterOpaqueIndices,
          customSceneHdrIndices, customPostProcessIndices }, extensions);

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
                entry.script->ExecuteProfiledCallback(&Script::OnPreRender, ScriptCallbackKind::PRE_RENDER, "OnPreRender");
            }
        }
    }

    /// @note RenderPipeline 実行 + デバッグスナップショット更新

    /// @note GPU query 領域は renderer の物理フレームが所有する。ビュー開始で再初期化しない。
    {
        FBZZ_PROFILE_SCOPE("RenderSystem::GpuProfilerSetup");
        renderer.GpuProfCollect();

        /// @note GPU フックを RenderPipeline に設定する。CPU フックとは独立しているため、
        /// @note Profiler の CPU スコープ計測と干渉しない。
        pipeline.SetGpuProfilerHooks(
            [&](std::string_view name) { renderer.GpuProfBeginPass(name.data()); },
            [&](std::string_view name) { renderer.GpuProfEndPass(name.data()); }
        );
    }

    renderer::GpuProfilerViewMetadata gpuView;
    gpuView.applicationFrameSerial = resources.FrameStamp();
    gpuView.viewId = viewKey;
    gpuView.sceneGeneration = scene.GetRenderSceneGeneration();
    gpuView.resourceEpoch = resources.GetResetVersion();
    gpuView.outputId = outputRT.id;
    gpuView.outputGeneration = outputRT.gen;
    gpuView.width = sHdrW;
    gpuView.height = sHdrH;
    const bool graphExecuted = pipeline.Execute(passCtx, capture, &gpuView, &viewTargets.renderPlan);

    if (capture)
        capture->Finish(pipeline.LastReport(), renderer.GpuProfGetSnapshot(), pipeline.LastGpuProfilerView());
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
                entry.script->ExecuteProfiledCallback(&Script::OnPostRender, ScriptCallbackKind::POST_RENDER, "OnPostRender");
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

        /// @note 遅延値は出自を保持し、別ビュー・旧 Plan・再生成前の資源へ帰属させない。
        dbgSnap.gpuCurrentView = pipeline.LastGpuProfilerView();
        dbgSnap.gpuProfiler = renderer.GpuProfGetSnapshot();
        std::erase_if(dbgSnap.gpuProfiler.passes, [&](const auto& gp) {
            return !gp.available || !std::isfinite(gp.gpuMs) || gp.gpuMs < 0.0
                || gp.physicalFrameSerial != dbgSnap.gpuProfiler.physicalFrameSerial
                || gp.deviceEpoch != dbgSnap.gpuProfiler.deviceEpoch
                || !renderer::IsGpuProfilerViewCompatible(gp.metadata, dbgSnap.gpuCurrentView, 8u);
        });
        dbgSnap.gpuProfiler.available = dbgSnap.gpuProfiler.available && !dbgSnap.gpuProfiler.passes.empty();
        for (const auto& gp : dbgSnap.gpuProfiler.passes)
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
    scene.ClearUserRenderPasses();
    pipeline.BeginBuild();
    viewTargets.pipeline = std::move(static_cast<renderer::RenderPipeline&>(pipeline));

}

} /// @note namespace fbzz::scene
