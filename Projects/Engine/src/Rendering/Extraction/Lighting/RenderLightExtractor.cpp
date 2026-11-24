/// @file    RenderLightExtractor.cpp
/// @brief   シーンのライトを収集して描画用の光源設定を組み立てる。
/// @author  Hasegawa Jin
/// @date    2026-09-21

#include "RenderLightExtraction.hpp"
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/Lighting/LightComponent.hpp>
#include <Graphics/Renderer/View/Lighting/ColorTemperature.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {

namespace {

using namespace light_extraction_detail;

void CollectComponentLights(Scene& scene, const renderer::Camera& camera,
    const renderer::RenderSettings& settings, bool rayTracingEnabled, RenderLightExtraction& output,
    std::vector<PunctualShadowCandidate>& shadowCandidates, std::vector<LightCookieCandidate>& cookieCandidates)
{
    auto& lightData = output.lightData;
    auto& punctualLights = output.punctualLights;
    /// @note Directional Light のシャドウ設定 (LightComponent から取得)
    auto& dirCastShadows = output.dirCastShadows;
    auto& dirShadowBias = output.dirShadowBias;
    auto& dirShadowStrength = output.shadowStrength;
    auto& dirShadowDistance = output.dirShadowDistance;
    /// @note b3 経路の点光源 / スポットの光源半径。添字は legacyShadowSlots と同じ。
    auto& legacySourceRadius = output.legacySourceRadius;
    /// @note Area の影を焼く錐台の半画角 [degrees]。Spot の outerCone に相当する値として渡す。
    /// @note 面光源は法線側の半球 (= 90 度) を照らすが、透視投影は 90 度で無限に広がるため
    /// @note 張れない。75 度は「パネルの正面に置いた物の影は出る / 真横は諦める」の線。
    constexpr float AREA_SHADOW_HALF_FOV_DEGREES = 75.0f;
    /// @note View<> だと GameObject が取れず activeInHierarchy() を見られないので、GO を切っても
    /// @note 光だけが残る。GetEntities<> は View<> と同じ基底 span なので走査順は変わらない。
    for (EntityID id : scene.GetEntities<LightComponent>()) {
        GameObject*     ownerObject    = scene.GetGameObject(id);
        LightComponent* light = scene.GetComponent<LightComponent>(id);
        if (!ownerObject || !light || !ownerObject->activeInHierarchy() || !light->enabled) continue;
        const Transform&      transform = ownerObject->transform;
        const LightComponent& component = *light;

        /// @note 色温度モードでは color 欄ではなく colorTemperature が正本。
        /// @note 毎フレーム引き直す。キャッシュは Inspector の反映漏れという見つけにくい種になる。
        const math::Vector3 lightColor =
            component.useColorTemperature ? renderer::ColorFromTemperature(component.colorTemperature)
                                   : component.color;

        if (rayTracingEnabled) {
            renderer::RayLightInput ray;
            ray.objectId = {scene.GetRenderSceneGeneration(), id.index, id.generation};
            ray.layerMask = ownerObject->layer >= 0 && ownerObject->layer < 32 ? (uint32_t{1} << ownerObject->layer) : 0;
            ray.type = static_cast<renderer::RayLightType>(component.type);
            ray.position = transform.worldPosition;
            ray.direction = transform.forward;
            ray.tangent = transform.right;
            ray.bitangent = transform.up;
            ray.color = lightColor;
            ray.intensity = component.intensity;
            ray.range = component.range;
            ray.innerCone = component.innerCone;
            ray.outerCone = component.outerCone;
            ray.areaWidth = component.areaWidth;
            ray.areaHeight = component.areaHeight;
            ray.sourceRadius = component.sourceRadius;
            ray.sourceLength = component.sourceLength;
            ray.twoSided = component.areaTwoSided;
            ray.castShadows = settings.shadowEnabled && component.castShadows;
            ray.shadowStrength = component.shadowStrength;
            if (!component.cookiePath.empty()) ray.unsupportedFlags |= renderer::RAY_LIGHT_UNSUPPORTED_COOKIE;
            if (ray.layerMask == 0) ray.unsupportedFlags |= renderer::RAY_LIGHT_INVALID_LAYER;
            output.rayLights.push_back(ray);
        }

        /// @note 点光源 / スポット / 大きさを持つ光源は上限に達するまで統合配列へも積む。
        /// @note b3 は「点を全部→スポットを全部」の 2 配列だがこちらは 1 本なので評価順が変わりうる。
        /// @note 加算なので結果は同じ (順序による丸め差のみ)。
        if (component.type != LightComponent::Type::Directional
            && punctualLights.size() < kMaxPunctualLights) {
            const size_t punctualIndex = punctualLights.size();
            PunctualLightGPU& punctualLight = punctualLights.emplace_back();
            /// @note worldPosition を使う: Transform::position は親基準のローカル座標。
            /// @note 子 GameObject にライトを置くと (キャラクターの発光部・車のヘッドライト・
            /// @note ボーンに付けた松明)、親の姿勢が一切効かず原点付近に光が落ちる。
            /// @note 向き (forward / right / up) は worldRotation から作られるので既に
            /// @note ワールド空間で、位置だけが取り残されていた。
            punctualLight.position  = transform.worldPosition;
            punctualLight.range     = component.range;
            punctualLight.color     = lightColor;
            punctualLight.intensity = component.intensity;
            /// @note 既定は Point。他の型が以降で上書きする。
            punctualLight.direction   = { 0.0f, -1.0f, 0.0f };
            punctualLight.innerCos    = 0.0f;
            punctualLight.outerCos    = 0.0f;
            punctualLight.type        = static_cast<uint32_t>(PunctualLightType::Point);
            punctualLight.shadowIndex = -1;
            punctualLight.cookieIndex = -1;
            punctualLight.tangent     = { 1.0f, 0.0f, 0.0f };
            punctualLight.bitangent   = { 0.0f, 1.0f, 0.0f };
            /// @note 点光源 / スポットでも halfWidth は光源半径として意味を持つ
            /// @note (形状は点のまま、ハイライトの広がりと影のにじみ幅にだけ効く)。
            punctualLight.halfWidth   = (std::max)(component.sourceRadius, 0.0f);
            punctualLight.halfHeight  = 0.0f;

            if (component.type == LightComponent::Type::Spot) {
                punctualLight.direction = transform.forward.Normalized();
                punctualLight.innerCos  = std::cos(component.innerCone * DEGREES_TO_RADIANS);
                punctualLight.outerCos  = std::cos(component.outerCone * DEGREES_TO_RADIANS);
                punctualLight.type      = static_cast<uint32_t>(PunctualLightType::Spot);
            } else if (component.type == LightComponent::Type::Sphere) {
                punctualLight.type      = static_cast<uint32_t>(PunctualLightType::Sphere);
            } else if (component.type == LightComponent::Type::Tube) {
                /// @note 管の軸は Transform の Right。蛍光灯を横向きに置く姿勢が既定になる。
                punctualLight.tangent    = transform.right.NormalizedOr({ 1.0f, 0.0f, 0.0f });
                punctualLight.halfHeight = (std::max)(component.sourceLength, 0.0f) * 0.5f;
                punctualLight.type       = static_cast<uint32_t>(PunctualLightType::Tube);
            } else if (component.type == LightComponent::Type::Area) {
                punctualLight.direction  = transform.forward.NormalizedOr({ 0.0f, 0.0f, 1.0f });
                punctualLight.tangent    = transform.right.NormalizedOr({ 1.0f, 0.0f, 0.0f });
                punctualLight.bitangent  = transform.up.NormalizedOr({ 0.0f, 1.0f, 0.0f });
                punctualLight.halfWidth  = (std::max)(component.areaWidth,  0.001f) * 0.5f;
                punctualLight.halfHeight = (std::max)(component.areaHeight, 0.001f) * 0.5f;
                /// @note Area では innerCos / outerCos が空くので、両面フラグの運搬に使う。
                /// @note 専用フィールドを足すと 96 バイトの構造体がキャッシュライン 2 本に収まらない。
                punctualLight.outerCos   = component.areaTwoSided ? 1.0f : 0.0f;
                punctualLight.type       = static_cast<uint32_t>(PunctualLightType::Area);
            }

            const math::Vector3 toCamera = transform.worldPosition - camera.m_position;
            const float cameraDistanceSquared = math::Vector3::Dot(toCamera, toCamera);

            /// @note b3 側でこのライトが取る添字。直後のブロックが末尾へ 1 つ足すだけなので、
            /// @note 採番される番号は今のカウンタ値そのもの。
            int legacySlot = -1;
            if (component.type == LightComponent::Type::Point && lightData.pointLightCount < 8)
                legacySlot = lightData.pointLightCount;
            else if (component.type == LightComponent::Type::Spot && lightData.spotLightCount < 4)
                legacySlot = kLegacySpotSlotBase + lightData.spotLightCount;
            if (legacySlot >= 0)
                legacySourceRadius[legacySlot] = (std::max)(component.sourceRadius, 0.0f);

            /// @note Cookie の候補。Spot 専用 — Point はキューブマップ、Directional は
            /// @note ワールド空間のタイリングという別の仕組みが要る。
            if (component.type == LightComponent::Type::Spot && !component.cookiePath.empty()) {
                LightCookieCandidate cookie{};
                cookie.punctualIndex = punctualIndex;
                cookie.legacySlot    = legacySlot;
                cookie.position      = transform.worldPosition;
                cookie.direction     = transform.forward.NormalizedOr({ 0.0f, 0.0f, 1.0f });
                cookie.range         = (std::max)(component.range, 0.05f);
                cookie.outerCone     = component.outerCone;
                cookie.nearPlane     = (std::max)(component.shadowNearPlane, 0.01f);
                cookie.rotationRad   = component.cookieRotation * DEGREES_TO_RADIANS;
                cookie.path          = component.cookiePath;
                cookie.cameraDistanceSquared  = cameraDistanceSquared;
                cookieCandidates.push_back(std::move(cookie));
            }

            /// @note 影の候補として控える。Directional 以外は全型が落とせる。
            /// @note 形状を持つ光源も点から焼いた影でよい: 影の形は遮蔽物と受光面の配置でほぼ決まり、
            /// @note 光源の大きさは半影の広さ (sourceRadius から作る penumbraTexels) にしか効かない。
            /// @note 管が長いと本来は半影が軸方向へ伸びるが、それには軸に沿った複数枚が要り 16 タイルでは足りない。
            const bool canCastShadow =
                component.castShadows && component.shadowStrength > 0.0f &&
                component.type != LightComponent::Type::Directional;
            if (canCastShadow) {
                /// @note 遠すぎるライトへタイルを割り当てない。判定距離に range を足すのは、
                /// @note range の大きいライトは離れていても画面を広く照らすため。
                const float fadeWidth = settings.shadow.punctualShadowDistance * 0.2f;
                const float limit = settings.shadow.punctualShadowDistance + component.range;
                if (cameraDistanceSquared <= limit * limit) {
                    /// @note フェードの距離は光源の影響範囲から測る。
                    const float fadeStart = limit - fadeWidth;
                    float visibility = 1.0f;
                    if (fadeWidth > 0.0f && cameraDistanceSquared > fadeStart * fadeStart) {
                        const float t = std::clamp((limit - std::sqrt(cameraDistanceSquared)) / fadeWidth, 0.0f, 1.0f);
                        visibility = t * t * (3.0f - 2.0f * t);
                    }
                    PunctualShadowCandidate shadowCandidate{};
                    shadowCandidate.punctualIndex = punctualIndex;
                    shadowCandidate.legacySlot    = legacySlot;
                    /// @note Sphere / Tube は Point と同じ全方位。Area だけが向きを持つ。
                    shadowCandidate.needsCube     = (component.type == LightComponent::Type::Point
                                       || component.type == LightComponent::Type::Sphere
                                       || component.type == LightComponent::Type::Tube);
                    shadowCandidate.position      = transform.worldPosition;
                    shadowCandidate.direction     = shadowCandidate.needsCube
                                       ? math::Vector3{ 0.0f, -1.0f, 0.0f }
                                       : transform.forward.NormalizedOr({ 0.0f, 0.0f, 1.0f });
                    shadowCandidate.range         = (std::max)(component.range, 0.05f);
                    /// @note Area は法線側の半球を照らすが、1 枚の透視投影では 180 度を張れない。
                    /// @note 実用上そこまでで、これ以上広げると端のテクセル密度が落ちるだけ。
                    shadowCandidate.outerCone     = (component.type == LightComponent::Type::Area)
                                       ? AREA_SHADOW_HALF_FOV_DEGREES
                                       : component.outerCone;
                    shadowCandidate.nearPlane     = (std::max)(component.shadowNearPlane, 0.01f);
                    shadowCandidate.bias          = (std::max)(component.shadowBias, 0.0f);
                    shadowCandidate.strength      = std::clamp(component.shadowStrength, 0.0f, 1.0f) * visibility;
                    shadowCandidate.sourceRadius  = (std::max)(component.sourceRadius, 0.0f);
                    shadowCandidate.cameraDistanceSquared  = cameraDistanceSquared;
                    if (shadowCandidate.strength > 0.0f) shadowCandidates.push_back(shadowCandidate);
                }
            }
        }

        if (component.type == LightComponent::Type::Directional) {
            lightData.lightDir       = transform.forward.Normalized();
            lightData.lightColor     = lightColor;
            lightData.lightIntensity = component.intensity;
            dirCastShadows    = component.castShadows;
            dirShadowBias     = component.shadowBias;
            dirShadowStrength = component.shadowStrength;
            dirShadowDistance = component.shadowDistance;
            output.dirShadowBakeLight = component.castShadows && component.shadowStrength > 0.0f
                && component.shadowMode == LightComponent::ShadowMode::AUTO_BAKE ? id : EntityID::INVALID;
            output.dirCachedFarShadowLight = component.castShadows && component.shadowStrength > 0.0f
                && component.shadowMode == LightComponent::ShadowMode::CACHED_FAR_CASCADE ? id : EntityID::INVALID;
        } else if (component.type == LightComponent::Type::Point
                   && lightData.pointLightCount < 8) {
            auto& pointLight    = lightData.pointLights[lightData.pointLightCount++];
            pointLight.position  = transform.worldPosition;
            pointLight.range     = component.range;
            pointLight.color     = lightColor;
            pointLight.intensity = component.intensity;
        } else if (component.type == LightComponent::Type::Spot
                   && lightData.spotLightCount < 4) {
            auto& spotLight    = lightData.spotLights[lightData.spotLightCount++];
            spotLight.position  = transform.worldPosition;
            spotLight.direction = transform.forward.Normalized();
            spotLight.range     = component.range;
            spotLight.innerCos  = std::cos(component.innerCone * DEGREES_TO_RADIANS);
            spotLight.outerCos  = std::cos(component.outerCone * DEGREES_TO_RADIANS);
            spotLight.color     = lightColor;
            spotLight.intensity = component.intensity;
        }
    }
}

}

RenderLightExtraction ExtractRenderLights(Scene& scene, const renderer::Camera& camera,
    const renderer::RenderSettings& settings, uint32_t atlasResolution, bool experimentalRayTracingEnabled)
{
    using namespace light_extraction_detail;

    RenderLightExtraction output;
    output.rayLightsComplete = experimentalRayTracingEnabled;
    auto& lightData = output.lightData;
    lightData.lightDir       = { 0.0f, -1.0f, 0.5f };
    lightData.lightColor     = { 1.0f,  1.0f, 1.0f };
    lightData.lightIntensity = 1.0f;
    output.punctualLights.reserve(32);
    std::vector<PunctualShadowCandidate> shadowCandidates;
    std::vector<LightCookieCandidate> cookieCandidates;
    CollectComponentLights(scene, camera, settings, experimentalRayTracingEnabled, output, shadowCandidates, cookieCandidates);
    AppendParticleLights(scene, experimentalRayTracingEnabled, output);
    BuildPunctualShadowAtlas(shadowCandidates, settings, atlasResolution, output);
    BuildLightCookieAtlas(cookieCandidates, output);
    ApplySkyLighting(scene, experimentalRayTracingEnabled, output);

    /// @note ambientColor: Lit モードでは AMBIENT_SCALE 相当値、Unlit 系では白に上書き
    lightData.ambientColor = { 0.08f, 0.08f, 0.08f };
    if (settings.IsUnlit()) {
        lightData.ambientColor    = { 1.0f, 1.0f, 1.0f };
        lightData.lightIntensity  = 0.0f;
        lightData.pointLightCount = 0;
        lightData.spotLightCount  = 0;
        /// @note 空も消灯する。分離前は lightIntensity=0 が空系シェーダーにも効いていたので、
        /// @note Unlit 表示で空が黒く落ちる従来の挙動を skyDimmer 側で維持する。
        lightData.skyDimmer       = 0.0f;
    }

    return output;
}

}
