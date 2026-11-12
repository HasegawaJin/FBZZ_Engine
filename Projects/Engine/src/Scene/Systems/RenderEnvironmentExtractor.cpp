/// @file    RenderEnvironmentExtractor.cpp
/// @brief   Scene とアセットから描画環境を値として抽出する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <Engine/Scene/Systems/RenderEnvironmentExtractor.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/Components/SunMoonRenderer.hpp>
#include <Engine/Scene/Components/VolumetricCloudComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Engine/Scene/Components/EnvironmentLightComponent.hpp>
#include <Engine/Scene/Fields/FlowFieldFrame.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/WaterOpticalSettings.hpp>
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Util/Mathf.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <vector>
namespace fbzz::scene {
namespace {
struct EnvironmentCandidatePool {
    std::vector<std::unique_ptr<std::vector<EntityID>>> entries;
    size_t depth = 0;
};

EnvironmentCandidatePool& EnvironmentCandidates()
{
    thread_local EnvironmentCandidatePool pool;
    return pool;
}

/// @note Preserve GameObjects precedence for late Component registration without visiting unrelated skeleton nodes.
class EnvironmentCandidateLease {
public:
    explicit EnvironmentCandidateLease(Scene& scene)
    {
        auto& pool = EnvironmentCandidates();
        if (pool.depth == pool.entries.size())
            pool.entries.push_back(std::make_unique<std::vector<EntityID>>());
        m_entities = pool.entries[pool.depth++].get();
        const auto skies = scene.GetEntities<SkyRenderer>();
        const auto suns = scene.GetEntities<SunMoonRenderer>();
        m_entities->assign(skies.begin(), skies.end());
        m_entities->insert(m_entities->end(), suns.begin(), suns.end());
        scene.SortEntitiesBySceneOrder(*m_entities);
        m_entities->erase(std::unique(m_entities->begin(), m_entities->end()), m_entities->end());
    }
    ~EnvironmentCandidateLease() { m_entities->clear(); --EnvironmentCandidates().depth; }
    EnvironmentCandidateLease(const EnvironmentCandidateLease&) = delete;
    EnvironmentCandidateLease& operator=(const EnvironmentCandidateLease&) = delete;
    std::span<const EntityID> Entities() const { return *m_entities; }
private:
    std::vector<EntityID>* m_entities = nullptr;
};

float ReadBoundedMaterialFloat(const asset::MaterialAssetAuthoring* material, const char* key,
    float fallback, float minimum, float maximum)
{
    if (!material) return fallback;
    const auto found = material->params.find(key);
    if (found == material->params.end() || found->second.empty() || !std::isfinite(found->second[0]))
        return fallback;
    return std::clamp(found->second[0], minimum, maximum);
}

struct WaterWaveSummary {
    float amplitude = 0.0f;
    float frequency = 0.12f;
    float speed = 1.0f;
};

WaterWaveSummary SummarizeWaterWaves(const WaterComponent& water)
{
    WaterWaveSummary result;
    if (!water.enableGerstnerWaves) return result;
    double totalAmplitude = 0.0;
    double weightedFrequency = 0.0;
    double weightedSpeed = 0.0;
    const size_t count = (std::min)(static_cast<size_t>(water.waveCount), water.waves.size());
    for (size_t i = 0; i < count; ++i) {
        const auto& wave = water.waves[i];
        if (!std::isfinite(wave.amplitude) || !std::isfinite(wave.wavelength)
            || wave.amplitude <= 0.0f || wave.wavelength <= math::EPSILON) continue;
        const double frequency = static_cast<double>(math::TWO_PI) / wave.wavelength;
        totalAmplitude += wave.amplitude;
        weightedFrequency += frequency * wave.amplitude;
        weightedSpeed += std::sqrt(9.8 * frequency) * wave.amplitude;
    }
    if (totalAmplitude <= math::EPSILON) return result;

    /// @note Match the existing caustics displacement scale while deriving frequency and phase speed from active waves.
    result.amplitude = static_cast<float>(std::clamp(totalAmplitude * 0.12, 0.015, 0.18));
    result.frequency = static_cast<float>(weightedFrequency / totalAmplitude);
    result.speed = static_cast<float>(weightedSpeed / totalAmplitude) * water.WaveSpeedMultiplier();
    return result;
}

bool GetWaterYaw(const Transform& transform, float& cosYaw, float& sinYaw)
{
    const math::Vector3 forward = transform.worldRotation * math::Vector3::FORWARD;
    const float horizontalLength = std::sqrt(forward.x * forward.x + forward.z * forward.z);
    if (!std::isfinite(horizontalLength) || horizontalLength <= 1.0e-6f) return false;
    cosYaw = forward.z / horizontalLength;
    sinYaw = forward.x / horizontalLength;
    return std::isfinite(cosYaw) && std::isfinite(sinYaw);
}

renderer::RenderUnderwaterInput EvaluateUnderwaterInfo(const RenderPassContext& ctx)
{
    renderer::RenderUnderwaterInput best{};
    const float time = Time::time;

    /// @note 親ごと無効化された水面は拾わない。View は GameObject を返さないので実体から引く。
    for (const EntityID waterId : ctx.scene.GetEntities<WaterComponent>()) {
        const GameObject* waterObject = ctx.scene.GetGameObject(waterId);
        auto* waterPtr = ctx.scene.GetComponent<WaterComponent>(waterId);
        if (!waterObject || !waterPtr || !waterPtr->enabled
            || WaterSurfaceZoneOwner(*waterPtr, WaterSurfaceReadView::SIMULATION).IsValid()
            || !waterObject->activeInHierarchy()) continue;
        const uint32_t layer = static_cast<uint32_t>(waterObject->layer);
        if (layer >= 32u || (ctx.cullingMask & (1u << layer)) == 0u) continue;
        WaterComponent& water = *waterPtr;
        const Transform& transform = WaterSurfaceTransform(water, waterObject->transform,
            WaterSurfaceReadView::SIMULATION);

        WaterSurfaceHeightSample sample;
        if (!TrySampleWaterSurfaceHeightWorld(water, transform, ctx.camera.m_position, time, sample,
                WaterSurfaceReadView::SIMULATION)
            || !sample.underwater || sample.depth <= best.depth) continue;
        const float depth = sample.depth;

        /// @note 視覚パラメータは fzmat から読む。未設定時は WaterComponent のデフォルト値と揃えた定数を使う。
        asset::MaterialAsset::AuthoringSnapshotPtr matAuthoringSnapshot;
        const asset::MaterialAssetAuthoring* mat = nullptr;
        if (!water.materialPath.empty()) {
            const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(water.materialPath);
            {
                matAuthoringSnapshot = asset::ReadMaterialAuthoring(handle);
                mat = matAuthoringSnapshot ? &matAuthoringSnapshot->Data() : nullptr;
            }
        }
        auto getF = [mat](const char* name, float def) -> float {
            if (!mat) return def;
            auto it = mat->params.find(name);
            if (it != mat->params.end() && !it->second.empty()) return it->second[0];
            return def;
        };
        auto getF3 = [mat](const char* name, math::Vector3 def) -> math::Vector3 {
            if (!mat) return def;
            auto it = mat->params.find(name);
            if (it != mat->params.end() && it->second.size() >= 3)
                return { it->second[0], it->second[1], it->second[2] };
            return def;
        };

        const float deepDepth = util::Mathf::Max(getF("deepDepth", 5.0f), 0.001f);
        const float t = util::Mathf::Clamp01(depth / deepDepth);
        const auto shallowColor = getF3("shallowColor", { 0.20f, 0.60f, 0.70f });
        const auto deepColor    = getF3("deepColor",    { 0.00f, 0.10f, 0.30f });
        const asset::WaterOpticalSettings optics = (mat ? asset::ResolveWaterOpticalSettings(*mat) : asset::WaterOpticalSettings{});
        const bool boundedRectangle = WaterSurfaceEffectiveShape(water, WaterSurfaceReadView::SIMULATION)
                == WaterSurfaceShape::RECTANGLE
            && !UsesWaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION)
            && !WaterSurfaceUsesHorizon(water, WaterSurfaceReadView::SIMULATION);
        float cosYaw = 1.0f;
        float sinYaw = 0.0f;
        if (optics.enabled && boundedRectangle && !GetWaterYaw(transform, cosYaw, sinYaw)) continue;
        best.enabled = true;
        best.depth = depth;
        best.opticalEnabled = optics.enabled;
        if (optics.enabled) {
            const float transitionDepth = ReadBoundedMaterialFloat(mat, "underwaterTransitionDepth", 0.15f, 0.001f, 2.0f);
            const float transition = util::Mathf::Clamp01(depth / transitionDepth);
            best.strength = transition * transition * (3.0f - 2.0f * transition);
            best.absorption = optics.absorption;
            best.scattering = optics.scattering;
            best.phaseG = optics.phaseG;
            best.surfaceY = sample.position.y;
            best.bottomY = sample.position.y - sample.waterDepthLimit;
            best.maxDistance = ReadBoundedMaterialFloat(mat, "underwaterMaxDistance", 120.0f, 1.0f, 10000.0f);
            best.lightStrength = ReadBoundedMaterialFloat(mat, "underwaterLightStrength", 1.0f, 0.0f, 8.0f);
            const float steps = ReadBoundedMaterialFloat(mat, "underwaterLightSteps", 48.0f, 16.0f, 64.0f);
            best.lightSteps = static_cast<int>(std::lround(steps));
            best.distortion = ReadBoundedMaterialFloat(mat, "underwaterDistortion", 0.001f, 0.0f, 0.01f);
            const WaterWaveSummary waves = SummarizeWaterWaves(water);
            best.waveAmplitude = waves.amplitude;
            best.waveFrequency = waves.frequency;
            best.waveSpeed = waves.speed;
            best.center = { transform.worldPosition.x, transform.worldPosition.z };
            if (boundedRectangle) {
                best.halfExtent = {
                    water.extentX * 0.5f * std::abs(transform.worldScale.x),
                    water.extentZ * 0.5f * std::abs(transform.worldScale.z)
                };
                best.cosYaw = cosYaw;
                best.sinYaw = sinYaw;
            }
        } else {
            best.strength = util::Mathf::Clamp01(depth / 2.0f);
            best.fogDensity = util::Mathf::Lerp(0.04f, 0.35f, t);
            best.color = {
                util::Mathf::Lerp(shallowColor.x, deepColor.x, t),
                util::Mathf::Lerp(shallowColor.y, deepColor.y, t),
                util::Mathf::Lerp(shallowColor.z, deepColor.z, t)
            };
        }
    }

    return best;
}

renderer::RenderCausticsInput FindCausticsSource(RenderPassContext& ctx)
{
    renderer::RenderCausticsInput result{};
    /// @note 親ごと無効化された水面は拾わない。View は GameObject を返さないので実体から引く。
    for (const EntityID waterId : ctx.scene.GetEntities<WaterComponent>()) {
        const GameObject* waterObject = ctx.scene.GetGameObject(waterId);
        const auto* waterPtr = ctx.scene.GetComponent<WaterComponent>(waterId);
        if (!waterObject || !waterPtr || !waterPtr->enabled
            || WaterSurfaceZoneOwner(*waterPtr, WaterSurfaceReadView::SIMULATION).IsValid()
            || !waterObject->activeInHierarchy()) continue;
        const uint32_t layer = static_cast<uint32_t>(waterObject->layer);
        if (layer >= 32u || (ctx.cullingMask & (1u << layer)) == 0u) continue;
        const WaterComponent& water = *waterPtr;
        const Transform& transform = WaterSurfaceTransform(water, waterObject->transform,
            WaterSurfaceReadView::SIMULATION);
        /// @note 旧Causticsは矩形マスクだけを持ち、Splineの輪郭外へ光を漏らすため対象にしない。
        if (WaterSurfaceEffectiveShape(water, WaterSurfaceReadView::SIMULATION) != WaterSurfaceShape::RECTANGLE
            || UsesWaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION)) continue;
        if (!IsWaterSurfaceSupported(water, transform, WaterSurfaceReadView::SIMULATION)) continue;

        asset::MaterialAsset::AuthoringSnapshotPtr matAuthoringSnapshot;
        const asset::MaterialAssetAuthoring* mat = nullptr;
        if (!water.materialPath.empty()) {
            const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(water.materialPath);
            {
                matAuthoringSnapshot = asset::ReadMaterialAuthoring(handle);
                mat = matAuthoringSnapshot ? &matAuthoringSnapshot->Data() : nullptr;
            }
        }

        auto getF = [mat](const char* name, float def) -> float {
            if (!mat) return def;
            const auto it = mat->params.find(name);
            if (it != mat->params.end() && !it->second.empty()) return it->second[0];
            return def;
        };
        auto getTex = [mat](const char* name) -> std::string {
            if (!mat) return {};
            const auto it = mat->textures.find(name);
            if (it != mat->textures.end()) return it->second;
            return {};
        };

        const asset::WaterOpticalSettings optics = (mat ? asset::ResolveWaterOpticalSettings(*mat) : asset::WaterOpticalSettings{});

        const float enableCaustics  = getF("enableCaustics", 0.0f);
        if (enableCaustics < 0.5f) continue;

        const float intensity = getF("causticsIntensity", 1.0f);
        if (intensity <= 0.0f) continue;

        float totalAmp = 0.0f;
        float weightedWaveFreq = 0.0f;
        for (size_t i = 0; i < (std::min)(static_cast<size_t>(water.waveCount), water.waves.size()); ++i) {
            const auto& wave = water.waves[i];
            if (wave.amplitude <= 0.0f || wave.wavelength <= math::EPSILON) continue;
            totalAmp += wave.amplitude;
            weightedWaveFreq += (math::TWO_PI / wave.wavelength) * wave.amplitude;
        }

        /// @note 複数水面は最も強い設定を代表値として扱う。1 回のフルスクリーン加算で済ませるため、代表水面のみを投影元にする。
        if (result.enabled && intensity <= result.intensity) continue;
        float cosYaw = 1.0f;
        float sinYaw = 0.0f;
        if (!GetWaterYaw(transform, cosYaw, sinYaw)) continue;

        result.enabled     = true;
        result.intensity   = intensity;
        result.maxDistance = ReadBoundedMaterialFloat(mat, "underwaterMaxDistance", 120.0f, 1.0f, 10000.0f);
        result.tiling      = getF("causticsTiling", 4.0f);
        result.surfaceY    = transform.worldPosition.y;
        result.timeOffset  = Time::time * getF("causticsSpeed", 0.5f);
        result.centerX     = transform.worldPosition.x;
        result.centerZ     = transform.worldPosition.z;
        const bool horizon = WaterSurfaceUsesHorizon(water, WaterSurfaceReadView::SIMULATION);
        result.halfExtentX = horizon ? 0.0f : water.extentX * 0.5f * std::abs(transform.worldScale.x);
        result.halfExtentZ = horizon ? 0.0f : water.extentZ * 0.5f * std::abs(transform.worldScale.z);
        result.cosYaw = cosYaw;
        result.sinYaw = sinYaw;
        result.opticalEnabled = optics.enabled;
        result.absorption = optics.absorption;
        result.scattering = optics.scattering;
        result.phaseG = optics.phaseG;
        result.waveAmp     = getF("causticsWaveAmp", math::Clamp(totalAmp * 0.12f, 0.015f, 0.18f));
        result.waveFreq    = getF("causticsWaveFreq", totalAmp > math::EPSILON ? weightedWaveFreq / totalAmp : 0.12f);
        result.waveSpeed   = getF("causticsWaveSpeed", 1.0f);
        const auto texturePath = getTex("causticsTex");
        result.texture = texturePath.empty() ? renderer::ResourceHandle<renderer::TextureTag>{} : asset::StreamedTextureResolver::Engine().ResolveGpu(ctx.resources, texturePath);
    }
    return result;
}

VolumetricCloudComponent* FindActiveCloud(RenderPassContext& ctx)
{
    for (EntityID id : ctx.scene.GetEntities<VolumetricCloudComponent>()) {
        auto* cloud = ctx.scene.GetComponent<VolumetricCloudComponent>(id);
        const GameObject* owner = ctx.scene.GetGameObject(id);
        if (cloud && cloud->enabled && owner && owner->activeInHierarchy())
            return cloud;
    }
    return nullptr;
}

void ExtractCloud(RenderPassContext& ctx, renderer::RenderEnvironmentInput& environment)
{
    auto* cloud = FindActiveCloud(ctx);
    if (!cloud) return;
    /// @note 環境流があれば雲もその向き・その速さで流す (XZ 平面へ射影)。
    /// @note 粒子と雲の流れを 1 か所で揃えるため。環境流は SceneEnvironment が正本で、
    /// @note 粒子が受けるものとまったく同じ値を見ている。
    /// @note 環境流が無効なシーンは従来どおりコンポーネント固有の windDirection / windSpeed。
    math::Vector2 wind = cloud->windDirection.Normalized();
    float windSpeed = cloud->windSpeed;
    const AmbientWind& ambient = ctx.scene.FlowFrame().ambient;
    if (ambient.active) {
        const float xzLen = std::sqrt(ambient.direction.x * ambient.direction.x
                                    + ambient.direction.z * ambient.direction.z);
        if (xzLen > 1.0e-4f)
            wind = { ambient.direction.x / xzLen, ambient.direction.z / xzLen };
        /// @note 倍率ではなく m/s。雲の高さでどれだけ乗るかは windResponse が決める。
        windSpeed = ambient.speed * math::Clamp01(cloud->windResponse);
    }
    const float topHeight = cloud->bottomHeight + (std::max)(cloud->thickness, 1.0f);

    renderer::RenderCloudConstants cb{};
    cb.cloudLayer = {
        cloud->bottomHeight,
        topHeight,
        (std::max)(cloud->density, 0.0f),
        math::Clamp01(cloud->coverage)
    };
    /// @note Inspector は「大きさ [m]」で持ち、シェーダーが要る world→noise スケールへここで直す。
    const float cloudSize  = math::Clamp(cloud->cloudSize, 50.0f, 100000.0f);
    const float detailSize = math::Clamp(cloud->detailSize, 1.0f, cloudSize);
    cb.cloudNoise = {
        1.0f / cloudSize,
        /// @note シェーダー内で 1/cloudSize と掛けて 1/detailSize になる
        cloudSize / detailSize,
        Time::time,
        (std::max)(cloud->maxDistance, 100.0f)
    };
    cb.cloudWind = {
        wind.x,
        windSpeed,
        wind.y,
        static_cast<float>(cloud->stepCount < 8 ? 8 : (cloud->stepCount > 96 ? 96 : cloud->stepCount))
    };
    cb.cloudLighting = {
        (std::max)(cloud->lightAbsorption, 0.0f),
        (std::max)(cloud->ambientStrength, 0.0f),
        (std::max)(cloud->silverLining, 0.0f),
        math::Clamp01(cloud->lightShaftStrength)
    };
    cb.cloudAlbedo = {
        cloud->albedo.x, cloud->albedo.y, cloud->albedo.z,
        math::Clamp01(cloud->ambientGradient)
    };
    cb.cloudWeather = {
        1.0f / math::Clamp(cloud->weatherSize, 100.0f, 1000000.0f),
        math::Clamp01(cloud->weatherAmount),
        math::Clamp01(cloud->detailStrength),
        (std::max)(cloud->evolutionSpeed, 0.0f)
    };
    cb.cloudShading = {
        math::Clamp(cloud->extinction, 0.001f, 0.5f),
        (std::max)(cloud->sunIntensity, 0.0f),
        math::Clamp01(cloud->powderStrength),
        math::Clamp01(cloud->multiScatter)
    };
    cb.cloudProfile = {
        math::Clamp(cloud->bottomSoftness, 0.01f, 0.9f),
        math::Clamp(cloud->topSoftness, 0.01f, 0.9f),
        math::Clamp(cloud->anisotropy, 0.0f, 0.95f),
        static_cast<float>(cloud->lightStepCount < 1 ? 1
                         : (cloud->lightStepCount > 8 ? 8 : cloud->lightStepCount))
    };
    const float maxDistance = (std::max)(cloud->maxDistance, 100.0f);
    cb.cloudRange = {
        math::Clamp(cloud->minDistance, 0.0f, maxDistance),
        (std::max)(cloud->fadeDistance, 1.0f),
        math::Clamp01(cloud->horizonFade),
        0.0f
    };
    cb.cloudSunTint = { cloud->sunTint.x, cloud->sunTint.y, cloud->sunTint.z, 0.0f };
    cb.cloudAmbTint = { cloud->ambientTint.x, cloud->ambientTint.y, cloud->ambientTint.z, 0.0f };

    environment.cloud = cb;
    environment.cloudEnabled = true;
    environment.cloudHalfResolution = cloud->halfResolution;
}

}
renderer::RenderEnvironmentInput ExtractRenderEnvironment(RenderPassContext& ctx)
{
    bool collectEnvironmentPhases = false;
    if (profiler::Profiler::IsEnabled()) {
        const auto requestedFilter = profiler::Profiler::GetRequestedCaptureFilter();
        collectEnvironmentPhases = requestedFilter.scope == "RenderSceneExtractor::Environment"
            && (requestedFilter.category.empty() || requestedFilter.category == "General");
    }
    std::optional<profiler::ProfileScope> phaseScope;
    /// @note Only the explicit parent filter opens bounded phase scopes; the applied frame filter still controls recording.
    const auto beginPhase = [&](const char* name) {
        if (!collectEnvironmentPhases) return;
        phaseScope.reset();
        phaseScope.emplace(profiler::ProfilerMarker(name, "General"));
    };
    beginPhase("RenderSceneExtractor::Environment::WaterPrepare");
    PrepareWaterSceneSurfaces(ctx.scene);
    if (collectEnvironmentPhases) phaseScope.reset();
    renderer::RenderEnvironmentInput output;
    static renderer::RayEnvironmentCache rayEnvironmentCache;
    beginPhase("RenderSceneExtractor::Environment::RayEnvironment");
    if (!ctx.experimentalRayTracingEnabled) rayEnvironmentCache.Reset();
    /// @note Global IBL と同じ component order/active 条件で同じ owner を選ぶ。
    if (ctx.experimentalRayTracingEnabled) for (const EntityID id : ctx.scene.GetEntities<EnvironmentLightComponent>()) {
        const auto* go = ctx.scene.GetGameObject(id);
        const auto* light = ctx.scene.GetComponent<EnvironmentLightComponent>(id);
        if (!go || !go->activeInHierarchy() || !light || !light->enabled) continue;
        auto& environment = output.rayEnvironment;
        environment.requested = light->source != IblSource::StaticDDS || !light->rawEnvironmentPath.empty();
        environment.intensity = light->intensity;
        environment.rotationRadians = light->rotationY * 0.017453292519943295f;
        if (light->source == IblSource::StaticDDS && !light->rawEnvironmentPath.empty()) {
            auto& resolver = asset::StreamedTextureResolver::Engine();
            if (const auto* texture = resolver.ResolveAsset(ctx.resources, light->rawEnvironmentPath)) {
                environment.rawTexture = texture->gpuHandle;
                environment.pixels = rayEnvironmentCache.Load(texture->sourcePath, texture->gpuHandle,
                    resolver.ResolveContentRevision(ctx.resources, light->rawEnvironmentPath), ctx.resources);
                environment.ready = environment.pixels != nullptr;
            }
        }
        break;
    }
    beginPhase("RenderSceneExtractor::Environment::SkySunCollect");
    if (!ctx.scene.GetEntities<SkyRenderer>().empty()
        || !ctx.scene.GetEntities<SunMoonRenderer>().empty()) {
        const EnvironmentCandidateLease candidates(ctx.scene);
        for (const EntityID id : candidates.Entities()) {
            const auto* go = ctx.scene.GetGameObject(id);
            if (!go || !ShouldRenderGameObject(*go, ctx.cullingMask)) continue;
            if (!output.sky) if (const auto* sky = ctx.scene.GetComponent<SkyRenderer>(id); sky && sky->enabled)
                output.sky = renderer::RenderSkyInput{sky->rayleighScattering, sky->mieScattering,
                    sky->planetRadius, sky->atmosphereRadius, sky->skyScatterIntensity, sky->mieG};
            if (!output.sunMoon) if (const auto* sun = ctx.scene.GetComponent<SunMoonRenderer>(id); sun && sun->enabled)
                output.sunMoon = renderer::RenderSunMoonInput{sun->sunEnabled, sun->sunDiskIntensity,
                    sun->moonEnabled, sun->moonSize, sun->moonBrightness, sun->moonColor, sun->starBrightness};
        }
    }
    beginPhase("RenderSceneExtractor::Environment::UnderwaterAndCaustics");
    output.underwater = EvaluateUnderwaterInfo(ctx);
    output.cameraUnderwater = output.underwater.enabled;
    output.caustics = FindCausticsSource(ctx);
    beginPhase("RenderSceneExtractor::Environment::CloudAndLightCookies");
    ExtractCloud(ctx, output);
    for (int i = 0; i < ctx.lightCookieViewCount; ++i) {
        auto& cookie = ctx.lightCookieViews[i];
        cookie.sourceTexture = asset::StreamedTextureResolver::Engine().ResolveGpu(ctx.resources, cookie.sourcePath);
        cookie.srgb = IsEffectTextureSrgb(cookie.sourcePath);
    }
    if (collectEnvironmentPhases) phaseScope.reset();
    return output;
}
}
