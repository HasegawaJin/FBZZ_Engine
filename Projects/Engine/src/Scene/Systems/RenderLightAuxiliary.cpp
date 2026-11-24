/// @file    RenderLightAuxiliary.cpp
/// @brief   粒子の光源と空が駆動する昼夜・雲影の反映。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderLightExtraction.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/Components/ParticleEmitterSpace.hpp>
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>
#include <Engine/Scene/Components/ParticleLightSelection.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene::light_extraction_detail {

void AppendParticleLights(Scene& scene, bool rayTracingEnabled, RenderLightExtraction& output)
{
    auto& punctualLights = output.punctualLights;

    /// @note 粒子を点光源にする (ParticleEmitter の Lights モジュール)。LightComponent の後に積むので、
    /// @note 枠が足りないときに削られるのは粒子の光の方。Legacy (b3) には載せない。
    /// @note GPU シミュレーションの粒子は位置が GPU にしか無いので対象外 (Inspector に注記がある)。
    std::vector<ParticleLightEmission> particleLights;
    /// @note 粒子光源の未対応は Raster の 256 本上限や GPU runtime の準備状態で消さない。
    if (rayTracingEnabled) for (EntityID id : scene.GetEntities<ParticleEmitter>()) {
        const GameObject* ownerObject = scene.GetGameObject(id);
        const ParticleEmitter* emitter = scene.GetComponent<ParticleEmitter>(id);
        if (!ownerObject || !emitter || !ownerObject->activeInHierarchy() || !emitter->settings.enabled
            || !emitter->settings.light.lightEnabled) continue;
        renderer::RayLightInput ray;
        ray.objectId = {scene.GetRenderSceneGeneration(), id.index, id.generation};
        ray.layerMask = ownerObject->layer >= 0 && ownerObject->layer < 32 ? (uint32_t{1} << ownerObject->layer) : 0;
        ray.position = ownerObject->transform.worldPosition;
        ray.unsupportedFlags = renderer::RAY_LIGHT_UNSUPPORTED_PARTICLE;
        if (ray.layerMask == 0) ray.unsupportedFlags |= renderer::RAY_LIGHT_INVALID_LAYER;
        output.rayLights.push_back(ray);
    }
    for (EntityID id : scene.GetEntities<ParticleEmitter>()) {
        if (punctualLights.size() >= kMaxPunctualLights) break;
        GameObject*      ownerObject      = scene.GetGameObject(id);
        ParticleEmitter* emitter = scene.GetComponent<ParticleEmitter>(id);
        if (!ownerObject || !emitter || !ownerObject->activeInHierarchy() || !emitter->settings.enabled
            || !emitter->settings.light.lightEnabled
            || CanUseGpuSimulation(emitter->settings, &emitter->runtime.material))
            continue;
        SelectParticleLights(emitter->settings.light, emitter->runtime.particles,
                             kMaxPunctualLights - punctualLights.size(), particleLights);
        const bool localSpace = emitter->settings.simulationSpace == ParticleSimulationSpace::Local;
        for (const ParticleLightEmission& emission : particleLights) {
            PunctualLightGPU& particleLight = punctualLights.emplace_back();
            particleLight.position  = localSpace ? TransformEmitterPoint(ownerObject->transform, emission.position) : emission.position;
            particleLight.range     = emission.range;
            particleLight.color     = emission.color;
            particleLight.intensity = emission.intensity;
            particleLight.direction = { 0.0f, -1.0f, 0.0f };
            particleLight.type      = static_cast<uint32_t>(PunctualLightType::Point);
        }
    }
}

void ApplySkyLighting(Scene& scene, bool rayTracingEnabled, RenderLightExtraction& output)
{
    auto& lightData = output.lightData;

    /// @note 昼夜の色・強度カーブ (Phase B)
    /// @note 太陽の向きは DirectionalLight の transform が唯一のソース (lightDir は上書きしない)。
    /// @note dayNightEnabled のときは、その光源の太陽高度から色と強度の遷移だけを駆動する。
    /// @note ライトを回すと 太陽ディスク・空・月・空連動 IBL・ライティングが一緒に動く。
    /// @note 雲シャドウ params (Phase C) も SkyRenderer から読み、passCtx へ後で転送する。
    auto& skyCloudShadowStrength = output.cloudShadowStrength;
    auto& skyCloudShadowCoverage = output.cloudShadowCoverage;
    auto& skyCloudShadowScale = output.cloudShadowScale;
    auto& skyCloudShadowSpeed = output.cloudShadowSpeed;
    /// @note 太陽の向きは DirectionalLight 側で決まるため SkyRenderer の Transform は使わない。
    for (EntityID id : scene.GetEntities<SkyRenderer>()) {
        GameObject* ownerObject     = scene.GetGameObject(id);
        auto*       skyComponent = scene.GetComponent<SkyRenderer>(id);
        if (!ownerObject || !skyComponent || !ownerObject->activeInHierarchy() || !skyComponent->enabled) continue;
        const SkyRenderer& sky = *skyComponent;

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
            const math::Vector3 directionToSun =
                math::Vector3{ -lightData.lightDir.x, -lightData.lightDir.y, -lightData.lightDir.z }.Normalized();

            auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
            auto lerp1   = [](float a, float b, float t) { return a + (b - a) * t; };
            auto lerp3   = [](const math::Vector3& a, const math::Vector3& b, float t) {
                return math::Vector3{ a.x + (b.x - a.x) * t,
                                      a.y + (b.y - a.y) * t,
                                      a.z + (b.z - a.z) * t };
            };

            constexpr float RADIANS_TO_DEGREES = 57.29577951f;
            const float sineAltitude      = (std::max)(-1.0f, (std::min)(1.0f, directionToSun.y));
            const float altitudeDeg = std::asin(sineAltitude) * RADIANS_TO_DEGREES;

            const bool  aboveHorizon = altitudeDeg >= 0.0f;
            const float transitionDegrees  = aboveHorizon ? (std::max)(sky.dayAltitude,   0.1f)
                                      : (std::max)(sky.nightAltitude, 0.1f);
            float t = clamp01(std::fabs(altitudeDeg) / transitionDegrees);
            /// @note smoothstep: 帯の端で色・明るさが折れないようにする
            t = t * t * (3.0f - 2.0f * t);

            lightData.lightColor     = lerp3(sky.sunsetColor,
                                             aboveHorizon ? sky.dayColor : sky.nightColor, t);
            lightData.lightIntensity = lerp1(sky.sunsetIntensity,
                                             aboveHorizon ? sky.dayIntensity : sky.nightIntensity, t);
            /// @note 空の明るさは太陽光の強さとは別軸。共用していた頃は太陽を強くすると空も白飛びした。
            /// @note 詳細は SkyRenderer::skyDayBrightness。
            lightData.skyDimmer      = lerp1(sky.skySunsetBrightness,
                                             aboveHorizon ? sky.skyDayBrightness : sky.skyNightBrightness, t);
            /// @note independent RayLightTable は初期段で昼夜カーブを解決しない。primary owner を診断に残す。
            if (rayTracingEnabled) {
                const auto primary = std::find_if(output.rayLights.rbegin(), output.rayLights.rend(),
                    [](const renderer::RayLightInput& light) { return light.type == renderer::RayLightType::DIRECTIONAL; });
                if (primary != output.rayLights.rend()) primary->unsupportedFlags |= renderer::RAY_LIGHT_UNSUPPORTED_DAY_NIGHT;
                else {
                    renderer::RayLightInput ray;
                    ray.objectId = {scene.GetRenderSceneGeneration(), id.index, id.generation};
                    ray.layerMask = ownerObject->layer >= 0 && ownerObject->layer < 32 ? (uint32_t{1} << ownerObject->layer) : 0;
                    ray.type = renderer::RayLightType::DIRECTIONAL;
                    ray.unsupportedFlags = renderer::RAY_LIGHT_UNSUPPORTED_DAY_NIGHT;
                    if (ray.layerMask == 0) ray.unsupportedFlags |= renderer::RAY_LIGHT_INVALID_LAYER;
                    output.rayLights.push_back(ray);
                }
            }
        }
        break;
    }
}

} /// @note namespace fbzz::scene::light_extraction_detail
