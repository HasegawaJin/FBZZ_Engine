/// @file    RenderEnvironmentClouds.cpp
/// @brief   雲レイヤーとシーンの風から描画定数を組み立てる。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderEnvironmentExtraction.hpp"
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/Environment/VolumetricCloudComponent.hpp>
#include <Engine/Scene/Fields/FlowFieldFrame.hpp>
#include <Engine/Core/Timing/Time.hpp>
#include <Math/Scalar/MathUtils.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene::environment_extraction_detail {

namespace {

VolumetricCloudComponent* FindActiveCloud(RenderPassContext& context)
{
    for (EntityID entity : context.scene.GetEntities<VolumetricCloudComponent>()) {
        auto* cloud = context.scene.GetComponent<VolumetricCloudComponent>(entity);
        const GameObject* owner = context.scene.GetGameObject(entity);
        if (cloud && cloud->enabled && owner && owner->activeInHierarchy())
            return cloud;
    }
    return nullptr;
}

} /// @note namespace

void ExtractCloud(RenderPassContext& context, renderer::RenderEnvironmentInput& environment)
{
    auto* cloud = FindActiveCloud(context);
    if (!cloud) return;
    /// @note 環境流があれば雲もその向き・その速さで流す (XZ 平面へ射影)。
    /// @note 粒子と雲の流れを 1 か所で揃えるため。環境流は SceneEnvironment が正本で、
    /// @note 粒子が受けるものとまったく同じ値を見ている。
    /// @note 環境流が無効なシーンは従来どおりコンポーネント固有の windDirection / windSpeed。
    math::Vector2 wind = cloud->windDirection.Normalized();
    float windSpeed = cloud->windSpeed;
    const AmbientWind& sceneWind = context.scene.FlowFrame().ambient;
    if (sceneWind.active) {
        const float horizontalWindLength = std::sqrt(sceneWind.direction.x * sceneWind.direction.x
                                    + sceneWind.direction.z * sceneWind.direction.z);
        if (horizontalWindLength > 1.0e-4f)
            wind = { sceneWind.direction.x / horizontalWindLength, sceneWind.direction.z / horizontalWindLength };
        /// @note 倍率ではなく m/s。雲の高さでどれだけ乗るかは windResponse が決める。
        windSpeed = sceneWind.speed * math::Clamp01(cloud->windResponse);
    }
    const float topHeight = cloud->bottomHeight + (std::max)(cloud->thickness, 1.0f);

    renderer::RenderCloudConstants constants{};
    constants.cloudLayer = {
        cloud->bottomHeight,
        topHeight,
        (std::max)(cloud->density, 0.0f),
        math::Clamp01(cloud->coverage)
    };
    /// @note Inspector は「大きさ [m]」で持ち、シェーダーが要る world→noise スケールへここで直す。
    const float cloudSize  = math::Clamp(cloud->cloudSize, 50.0f, 100000.0f);
    const float detailSize = math::Clamp(cloud->detailSize, 1.0f, cloudSize);
    constants.cloudNoise = {
        1.0f / cloudSize,
        /// @note シェーダー内で 1/cloudSize と掛けて 1/detailSize になる
        cloudSize / detailSize,
        Time::time,
        (std::max)(cloud->maxDistance, 100.0f)
    };
    constants.cloudWind = {
        wind.x,
        windSpeed,
        wind.y,
        static_cast<float>(cloud->stepCount < 8 ? 8 : (cloud->stepCount > 96 ? 96 : cloud->stepCount))
    };
    constants.cloudLighting = {
        (std::max)(cloud->lightAbsorption, 0.0f),
        (std::max)(cloud->ambientStrength, 0.0f),
        (std::max)(cloud->silverLining, 0.0f),
        math::Clamp01(cloud->lightShaftStrength)
    };
    constants.cloudAlbedo = {
        cloud->albedo.x, cloud->albedo.y, cloud->albedo.z,
        math::Clamp01(cloud->ambientGradient)
    };
    constants.cloudWeather = {
        1.0f / math::Clamp(cloud->weatherSize, 100.0f, 1000000.0f),
        math::Clamp01(cloud->weatherAmount),
        math::Clamp01(cloud->detailStrength),
        (std::max)(cloud->evolutionSpeed, 0.0f)
    };
    constants.cloudShading = {
        math::Clamp(cloud->extinction, 0.001f, 0.5f),
        (std::max)(cloud->sunIntensity, 0.0f),
        math::Clamp01(cloud->powderStrength),
        math::Clamp01(cloud->multiScatter)
    };
    constants.cloudProfile = {
        math::Clamp(cloud->bottomSoftness, 0.01f, 0.9f),
        math::Clamp(cloud->topSoftness, 0.01f, 0.9f),
        math::Clamp(cloud->anisotropy, 0.0f, 0.95f),
        static_cast<float>(cloud->lightStepCount < 1 ? 1
                         : (cloud->lightStepCount > 8 ? 8 : cloud->lightStepCount))
    };
    const float maxDistance = (std::max)(cloud->maxDistance, 100.0f);
    constants.cloudRange = {
        math::Clamp(cloud->minDistance, 0.0f, maxDistance),
        (std::max)(cloud->fadeDistance, 1.0f),
        math::Clamp01(cloud->horizonFade),
        0.0f
    };
    constants.cloudSunTint = { cloud->sunTint.x, cloud->sunTint.y, cloud->sunTint.z, 0.0f };
    constants.cloudAmbTint = { cloud->ambientTint.x, cloud->ambientTint.y, cloud->ambientTint.z, 0.0f };

    environment.cloud = constants;
    environment.cloudEnabled = true;
    environment.cloudHalfResolution = cloud->halfResolution;
}

} /// @note namespace fbzz::scene::environment_extraction_detail
