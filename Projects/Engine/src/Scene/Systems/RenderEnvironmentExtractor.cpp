/// @file    RenderEnvironmentExtractor.cpp
/// @brief   Scene とアセットから描画環境を値として抽出する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <Engine/Scene/Systems/RenderEnvironmentExtractor.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/Components/SunMoonRenderer.hpp>
#include <Engine/Scene/Components/VolumetricCloudComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Fields/FlowFieldFrame.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Util/Mathf.hpp>
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include <algorithm>
#include <cmath>
namespace fbzz::scene {
namespace {
renderer::RenderUnderwaterInput EvaluateUnderwaterInfo(const RenderPassContext& ctx)
{
    renderer::RenderUnderwaterInput best{};
    const float time = Time::time;

    /// @note 親ごと無効化された水面は拾わない。View は GameObject を返さないので実体から引く。
    for (const EntityID waterId : ctx.scene.GetEntities<WaterComponent>()) {
        const GameObject* waterObject = ctx.scene.GetGameObject(waterId);
        const auto* waterPtr = ctx.scene.GetComponent<WaterComponent>(waterId);
        if (!waterObject || !waterPtr || !waterPtr->enabled || !waterObject->activeInHierarchy()) continue;
        const WaterComponent& water = *waterPtr;
        const Transform& transform = waterObject->transform;

        const float localX = ctx.camera.m_position.x - transform.position.x;
        const float localZ = ctx.camera.m_position.z - transform.position.z;
        if (std::abs(localX) > water.extentX * 0.5f || std::abs(localZ) > water.extentZ * 0.5f) {
            continue;
        }

        /// @note 水面は GPU で揺らすが、カメラ水没判定はポストプロセス前に CPU で決める必要が
        /// @note あるため、WaterComponent の Gerstner 評価を再利用し描画された水面と近い高さで判定する。
        const float relativeY = ctx.camera.m_position.y - transform.position.y;
        const float heightBound = water.SurfaceHeightBound();
        if (relativeY >= heightBound) continue;
        const float surfaceY = transform.position.y
            + water.GetSurfaceHeightAt(ctx.camera.m_position.x, ctx.camera.m_position.z, time);
        const float depth = surfaceY - ctx.camera.m_position.y;
        if (depth <= 0.0f || depth <= best.depth) continue;

        /// @note 視覚パラメータは fzmat から読む。未設定時は WaterComponent のデフォルト値と揃えた定数を使う。
        const asset::MaterialAsset* mat = nullptr;
        if (!water.materialPath.empty()) {
            const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(water.materialPath);
            mat = asset::AssetManager::Get<asset::MaterialAsset>(handle);
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
        best.enabled = true;
        best.depth = depth;
        best.strength = util::Mathf::Clamp01(depth / 2.0f);
        best.fogDensity = util::Mathf::Lerp(0.04f, 0.35f, t);
        best.color = {
            util::Mathf::Lerp(shallowColor.x, deepColor.x, t),
            util::Mathf::Lerp(shallowColor.y, deepColor.y, t),
            util::Mathf::Lerp(shallowColor.z, deepColor.z, t)
        };
    }

    return best;
}

bool IsCameraUnderwater(const RenderPassContext& ctx)
{
    const float time = Time::time;
    /// @note 親ごと無効化された水面は拾わない。View は GameObject を返さないので実体から引く。
    for (const EntityID waterId : ctx.scene.GetEntities<WaterComponent>()) {
        const GameObject* waterObject = ctx.scene.GetGameObject(waterId);
        const auto* waterPtr = ctx.scene.GetComponent<WaterComponent>(waterId);
        if (!waterObject || !waterPtr || !waterPtr->enabled || !waterObject->activeInHierarchy()) continue;
        const WaterComponent& water = *waterPtr;
        const Transform& transform = waterObject->transform;

        const float localX = ctx.camera.m_position.x - transform.position.x;
        const float localZ = ctx.camera.m_position.z - transform.position.z;
        if (std::abs(localX) > water.extentX * 0.5f || std::abs(localZ) > water.extentZ * 0.5f) {
            continue;
        }

        /// @note 水中では画面全体の濁りと散乱が支配的になり SSAO の接触影がノイズに見えやすいため、Deferred 合成時点で強度だけ 0 にして既存の SSAO パス構成を変えずに無効化する。
        const float relativeY = ctx.camera.m_position.y - transform.position.y;
        const float heightBound = water.SurfaceHeightBound();
        if (relativeY >= heightBound) continue;
        if (relativeY < -heightBound) return true;
        const float surfaceY = transform.position.y
            + water.GetSurfaceHeightAt(ctx.camera.m_position.x, ctx.camera.m_position.z, time);
        if (ctx.camera.m_position.y < surfaceY) {
            return true;
        }
    }
    return false;
}

renderer::RenderCausticsInput FindCausticsSource(RenderPassContext& ctx)
{
    renderer::RenderCausticsInput result{};
    /// @note 親ごと無効化された水面は拾わない。View は GameObject を返さないので実体から引く。
    for (const EntityID waterId : ctx.scene.GetEntities<WaterComponent>()) {
        const GameObject* waterObject = ctx.scene.GetGameObject(waterId);
        const auto* waterPtr = ctx.scene.GetComponent<WaterComponent>(waterId);
        if (!waterObject || !waterPtr || !waterPtr->enabled || !waterObject->activeInHierarchy()) continue;
        const WaterComponent& water = *waterPtr;
        const Transform& transform = waterObject->transform;

        const asset::MaterialAsset* mat = nullptr;
        if (!water.materialPath.empty()) {
            const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(water.materialPath);
            mat = asset::AssetManager::Get<asset::MaterialAsset>(handle);
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

        const float enableCaustics  = getF("enableCaustics", 0.0f);
        if (enableCaustics < 0.5f) continue;

        const float intensity = getF("causticsIntensity", 1.0f);
        if (intensity <= 0.0f) continue;

        float totalAmp = 0.0f;
        float weightedWaveFreq = 0.0f;
        for (const auto& wave : water.waves) {
            if (wave.amplitude <= 0.0f || wave.wavelength <= math::EPSILON) continue;
            totalAmp += wave.amplitude;
            weightedWaveFreq += (math::TWO_PI / wave.wavelength) * wave.amplitude;
        }

        /// @note 複数水面は最も強い設定を代表値として扱う。1 回のフルスクリーン加算で済ませるため、代表水面のみを投影元にする。
        if (result.enabled && intensity <= result.intensity) continue;

        result.enabled     = true;
        result.intensity   = intensity;
        result.tiling      = getF("causticsTiling", 4.0f);
        result.surfaceY    = transform.position.y;
        result.timeOffset  = Time::time * getF("causticsSpeed", 0.5f);
        result.centerX     = transform.position.x;
        result.centerZ     = transform.position.z;
        result.halfExtentX = water.extentX * 0.5f;
        result.halfExtentZ = water.extentZ * 0.5f;
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
    ///       @note 粒子と雲の流れを 1 か所で揃えるため。環境流は SceneEnvironment が正本で、
    /// @note 粒子が受けるものとまったく同じ値を見ている。
    ///       @note 環境流が無効なシーンは従来どおりコンポーネント固有の windDirection / windSpeed。
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
    renderer::RenderEnvironmentInput output;
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        if (!output.sky) if (const auto* sky = go.GetComponent<SkyRenderer>(); sky && sky->enabled)
            output.sky = renderer::RenderSkyInput{sky->rayleighScattering, sky->mieScattering,
                sky->planetRadius, sky->atmosphereRadius, sky->skyScatterIntensity, sky->mieG};
        if (!output.sunMoon) if (const auto* sun = go.GetComponent<SunMoonRenderer>(); sun && sun->enabled)
            output.sunMoon = renderer::RenderSunMoonInput{sun->sunEnabled, sun->sunDiskIntensity,
                sun->moonEnabled, sun->moonSize, sun->moonBrightness, sun->moonColor};
    }
    output.underwater = EvaluateUnderwaterInfo(ctx);
    output.cameraUnderwater = IsCameraUnderwater(ctx);
    output.caustics = FindCausticsSource(ctx);
    ExtractCloud(ctx, output);
    for (int i = 0; i < ctx.lightCookieViewCount; ++i) {
        auto& cookie = ctx.lightCookieViews[i];
        cookie.sourceTexture = asset::StreamedTextureResolver::Engine().ResolveGpu(ctx.resources, cookie.sourcePath);
        cookie.srgb = IsEffectTextureSrgb(cookie.sourcePath);
    }
    return output;
}
}
