/// @file    RenderViewSettings.cpp
/// @brief   シーンの環境光・ボリューム・一時演出からビュー設定を解決する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "RenderViewSettings.hpp"
#include <Graphics/Renderer/View/PostProcessBlend.hpp>
#include <Engine/Scene/Components/Rendering/PostProcessVolumeComponent.hpp>
#include <Engine/Scene/Components/VFX/VFXScreenEffect.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Math/LinearAlgebra/Quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace fbzz::scene::render_view_detail {
namespace {

/// @note カリングと描画に同じ揺れを使い、入力カメラの姿勢は保持する。
void ApplyCameraShake(Scene& scene, const renderer::Camera& inputCamera, renderer::Camera& shakenCamera)
{
    FBZZ_PROFILE_SCOPE("RenderSystem::CameraShake");
    math::Vector3 offset = math::Vector3::ZERO;
    float rollDegrees = 0.0f;
    /// @note 方向性キックはカメラの向きにかかわらずワールド空間で合成する。
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
        /// @note 経過時間と軸ごとの位相差で、フレームレートに依存しない揺れを作る。
        const float phase = shake.elapsed * shake.frequency;
        offset.x += std::sin(phase * 1.00f) * shake.amplitude * amount;
        offset.y += std::sin(phase * 1.37f + 1.7f) * shake.amplitude * amount;
        offset.z += std::sin(phase * 0.83f + 3.1f) * shake.amplitude * amount * 0.5f;
        rollDegrees += std::sin(phase * 1.11f + 0.6f) * shake.rotationAmplitude * amount;

        if (shake.kick != 0.0f) {
            math::Vector3 direction = shake.kickDirection;
            if (direction.LengthSq() <= 0.0001f)
                direction = inputCamera.m_position - go->transform.worldPosition;
            /// @note 発生源とカメラが同じ位置なら、方向のないキックは加えない。
            worldKick += direction.NormalizedOr(math::Vector3::ZERO) * (shake.kick * amount);
        }
    }
    if (offset.LengthSq() > 0.0f || worldKick.LengthSq() > 0.0f || rollDegrees != 0.0f) {
        constexpr float DEG_TO_RAD = 0.01745329251994329577f;
        shakenCamera.m_position = inputCamera.m_position
            + inputCamera.GetRight() * offset.x
            + inputCamera.GetUp() * offset.y
            + inputCamera.GetForward() * offset.z
            + worldKick;
        shakenCamera.m_rotation = inputCamera.m_rotation
            * math::Quaternion::FromEuler({ 0.0f, 0.0f, rollDegrees * DEG_TO_RAD });
    }
}

/// @note 最初に見つかった有効な環境光をビュー全体に適用する。
IblSource ResolveEnvironmentLight(Scene& scene, renderer::RenderSettings& effectiveSettings)
{
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
    return activeIblSource;
}

void ApplyPostProcessVolumes(Scene& scene, const renderer::Camera& camera,
    renderer::RenderSettings& effectiveSettings)
{
    renderer::VolumeSettings volumeSettings;
    if (const auto* runtimePostProcess = scene.TryGetRuntimePostProcessSettings())
        volumeSettings.post = *runtimePostProcess;
    FBZZ_PROFILE_SCOPE("RenderSystem::PostProcessVolumes");
    struct VolumeEntry {
        const PostProcessVolumeComponent* volume  = nullptr;
        const asset::PostProcessProfile*  profile = nullptr;
        float weight = 0.0f;
    };
    std::vector<VolumeEntry> entries;

    const math::Vector3 viewPosition = camera.m_position;

    for (EntityID id : scene.GetEntities<PostProcessVolumeComponent>()) {
        GameObject* go     = scene.GetGameObject(id);
        auto*       ppvPtr = scene.GetComponent<PostProcessVolumeComponent>(id);
        if (!go || !ppvPtr || !go->activeInHierarchy() || !ppvPtr->enabled) continue;
        const PostProcessVolumeComponent& ppv = *ppvPtr;

        /// @note 欠損プロファイルは無視し、既定値で他のボリュームの効果を上書きしない。
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

    /// @note 同じ優先度のボリュームは走査順で合成する。
    std::stable_sort(entries.begin(), entries.end(),
        [](const VolumeEntry& lhs, const VolumeEntry& rhs) {
            return lhs.volume->priority < rhs.volume->priority;
        });

    for (const VolumeEntry& entry : entries) {
        entry.profile->ApplyTo(volumeSettings, entry.weight);
    }

    renderer::ApplyVolumeSettings(volumeSettings, effectiveSettings);
}

void ApplyScreenEffects(Scene& scene, renderer::RenderSettings& effectiveSettings)
{
    FBZZ_PROFILE_SCOPE("RenderSystem::ScreenEffects");
    renderer::PostProcessSettings& pp = effectiveSettings.postProcess;
    float strongestFlash = 0.0f;
    /// @note 衝撃波の中心と半径を維持するため、強度が最大のリングを採用する。
    float strongestRing = 0.0f;
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
        /// @note 放射ブラーの強度がゼロならシェーダー側で処理を省く。
        if (effect.radialBlur > 0.0f)
            pp.lens.radialBlur += effect.radialBlur * weight;
        /// @note 開始時の半径ゼロのリングを描かないよう、進捗が正のときだけ適用する。
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
        /// @note 無効だったカラーグレーディングを再有効化するときは、以前の値が戻らないよう既定値から始める。
        if (!pp.colorGrading.enabled) {
            pp.colorGrading = renderer::ColorGradingSettings{};
            pp.colorGrading.enabled = true;
        }
        pp.colorGrading.saturation = (std::max)(pp.colorGrading.saturation + saturationOffset, 0.0f);
        pp.colorGrading.contrast += contrastOffset;
    }
}

}

ResolvedViewSettings ResolveViewSettings(
    Scene& scene, const renderer::Camera& inputCamera, const renderer::RenderSettings* settings)
{
    ResolvedViewSettings result{inputCamera, {}, IblSource::StaticDDS};
    ApplyCameraShake(scene, inputCamera, result.camera);
    if (settings) result.settings = *settings;
    result.iblSource = ResolveEnvironmentLight(scene, result.settings);
    ApplyPostProcessVolumes(scene, result.camera, result.settings);
    ApplyScreenEffects(scene, result.settings);
    return result;
}

}
