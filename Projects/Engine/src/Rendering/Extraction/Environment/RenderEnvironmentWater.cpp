/// @file    RenderEnvironmentWater.cpp
/// @brief   視点が入る水域とコースティクスの描画入力。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderEnvironmentExtraction.hpp"
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/Water/WaterComponent.hpp>
#include <Engine/Scene/Water/Surface/WaterSurfaceQuery.hpp>
#include <Engine/Scene/Water/Surface/WaterDepthField.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Material/MaterialAsset.hpp>
#include <Engine/Asset/Streaming/StreamedTextureResolver.hpp>
#include <Engine/Asset/Water/WaterOpticalSettings.hpp>
#include <Engine/Core/Timing/Time.hpp>
#include <Engine/Util/Math/Mathf.hpp>
#include "../../Passes/Geometry/GeometryPasses.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace fbzz::scene::environment_extraction_detail {

namespace {

float ReadBoundedMaterialFloat(const asset::MaterialAssetAuthoring* material, const char* key,
    float fallback, float minimum, float maximum)
{
    if (!material) {
        return fallback;
    }
    const auto found = material->params.find(key);
    if (found == material->params.end() || found->second.empty() || !std::isfinite(found->second[0])) {
        return fallback;
    }
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
    if (!water.enableGerstnerWaves) {
        return result;
    }
    double totalAmplitude = 0.0;
    double weightedFrequency = 0.0;
    double weightedSpeed = 0.0;
    const size_t count = (std::min)(static_cast<size_t>(water.waveCount), water.waves.size());
    for (size_t i = 0; i < count; ++i) {
        const auto& wave = water.waves[i];
        if (!std::isfinite(wave.amplitude) || !std::isfinite(wave.wavelength)
            || wave.amplitude <= 0.0f || wave.wavelength <= math::EPSILON) {
            continue;
        }
        const double frequency = static_cast<double>(math::TWO_PI) / wave.wavelength;
        totalAmplitude += wave.amplitude;
        weightedFrequency += frequency * wave.amplitude;
        weightedSpeed += std::sqrt(9.8 * frequency) * wave.amplitude;
    }
    if (totalAmplitude <= math::EPSILON) {
        return result;
    }

    // コースティクスの変位幅を保ちながら、有効な波から周波数と位相速度を求める。
    result.amplitude = static_cast<float>(std::clamp(totalAmplitude * 0.12, 0.015, 0.18));
    result.frequency = static_cast<float>(weightedFrequency / totalAmplitude);
    result.speed = static_cast<float>(weightedSpeed / totalAmplitude) * water.WaveSpeedMultiplier();
    return result;
}

bool GetWaterYaw(const Transform& transform, float& cosYaw, float& sinYaw)
{
    const math::Vector3 forward = transform.worldRotation * math::Vector3::FORWARD;
    const float horizontalLength = std::sqrt(forward.x * forward.x + forward.z * forward.z);
    if (!std::isfinite(horizontalLength) || horizontalLength <= 1.0e-6f) {
        return false;
    }
    cosYaw = forward.z / horizontalLength;
    sinYaw = forward.x / horizontalLength;
    return std::isfinite(cosYaw) && std::isfinite(sinYaw);
}

}

renderer::RenderUnderwaterInput EvaluateUnderwaterInfo(const RenderPassContext& context)
{
    renderer::RenderUnderwaterInput best{};
    const float time = Time::time;

    // 親ごと無効化された水面は拾わない。View は GameObject を返さないので実体から引く。
    for (const EntityID waterId : context.scene.GetEntities<WaterComponent>()) {
        const GameObject* waterObject = context.scene.GetGameObject(waterId);
        auto* waterPtr = context.scene.GetComponent<WaterComponent>(waterId);
        if (!waterObject || !waterPtr || !waterPtr->enabled
            || WaterSurfaceZoneOwner(*waterPtr, WaterSurfaceReadView::SIMULATION).IsValid()
            || !waterObject->activeInHierarchy()) {
            continue;
        }
        const uint32_t layer = static_cast<uint32_t>(waterObject->layer);
        if (layer >= 32u || (context.cullingMask & (1u << layer)) == 0u) {
            continue;
        }
        WaterComponent& water = *waterPtr;
        const Transform& transform = WaterSurfaceTransform(water, waterObject->transform,
            WaterSurfaceReadView::SIMULATION);

        WaterSurfaceHeightSample sample;
        if (!TrySampleWaterSurfaceHeightWorld(water, transform, context.camera.m_position, time, sample,
                WaterSurfaceReadView::SIMULATION)
            || !sample.underwater || sample.depth <= best.depth) {
            continue;
        }
        const float depth = sample.depth;

        // 視覚パラメータは fzmat から読む。未設定時は WaterComponent のデフォルト値と揃えた定数を使う。
        asset::MaterialAsset::AuthoringSnapshotPtr matAuthoringSnapshot;
        const asset::MaterialAssetAuthoring* material = nullptr;
        if (!water.materialPath.empty()) {
            const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(water.materialPath);
            {
                matAuthoringSnapshot = asset::ReadMaterialAuthoring(handle);
                material = matAuthoringSnapshot ? &matAuthoringSnapshot->Data() : nullptr;
            }
        }
        auto getF = [material](const char* name, float def) -> float {
            if (!material) {
                return def;
            }
            auto it = material->params.find(name);
            if (it != material->params.end() && !it->second.empty()) {
                return it->second[0];
            }
            return def;
        };
        auto getF3 = [material](const char* name, math::Vector3 def) -> math::Vector3 {
            if (!material) {
                return def;
            }
            auto it = material->params.find(name);
            if (it != material->params.end() && it->second.size() >= 3) {
                return { it->second[0], it->second[1], it->second[2] };
            }
            return def;
        };

        const float deepDepth = util::Mathf::Max(getF("deepDepth", 5.0f), 0.001f);
        const float t = util::Mathf::Clamp01(depth / deepDepth);
        const auto shallowColor = getF3("shallowColor", { 0.20f, 0.60f, 0.70f });
        const auto deepColor    = getF3("deepColor",    { 0.00f, 0.10f, 0.30f });
        const asset::WaterOpticalSettings optics = (material ? asset::ResolveWaterOpticalSettings(*material) : asset::WaterOpticalSettings{});
        const bool boundedRectangle = WaterSurfaceEffectiveShape(water, WaterSurfaceReadView::SIMULATION)
                == WaterSurfaceShape::RECTANGLE
            && !UsesWaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION)
            && !WaterSurfaceUsesHorizon(water, WaterSurfaceReadView::SIMULATION);
        float cosYaw = 1.0f;
        float sinYaw = 0.0f;
        if (optics.enabled && boundedRectangle && !GetWaterYaw(transform, cosYaw, sinYaw)) {
            continue;
        }
        best.enabled = true;
        best.depth = depth;
        best.opticalEnabled = optics.enabled;
        if (optics.enabled) {
            const float transitionDepth = ReadBoundedMaterialFloat(material, "underwaterTransitionDepth", 0.15f, 0.001f, 2.0f);
            const float transition = util::Mathf::Clamp01(depth / transitionDepth);
            best.strength = transition * transition * (3.0f - 2.0f * transition);
            best.absorption = optics.absorption;
            best.scattering = optics.scattering;
            best.phaseG = optics.phaseG;
            best.surfaceY = sample.position.y;
            best.bottomY = sample.position.y - sample.waterDepthLimit;
            best.maxDistance = ReadBoundedMaterialFloat(material, "underwaterMaxDistance", 120.0f, 1.0f, 10000.0f);
            best.lightStrength = ReadBoundedMaterialFloat(material, "underwaterLightStrength", 1.0f, 0.0f, 8.0f);
            const float steps = ReadBoundedMaterialFloat(material, "underwaterLightSteps", 48.0f, 16.0f, 64.0f);
            best.lightSteps = static_cast<int>(std::lround(steps));
            best.distortion = ReadBoundedMaterialFloat(material, "underwaterDistortion", 0.001f, 0.0f, 0.01f);
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

renderer::RenderCausticsInput FindCausticsSource(RenderPassContext& context)
{
    renderer::RenderCausticsInput result{};
    // 親ごと無効化された水面は拾わない。View は GameObject を返さないので実体から引く。
    for (const EntityID waterId : context.scene.GetEntities<WaterComponent>()) {
        const GameObject* waterObject = context.scene.GetGameObject(waterId);
        const auto* waterPtr = context.scene.GetComponent<WaterComponent>(waterId);
        if (!waterObject || !waterPtr || !waterPtr->enabled
            || WaterSurfaceZoneOwner(*waterPtr, WaterSurfaceReadView::SIMULATION).IsValid()
            || !waterObject->activeInHierarchy()) {
            continue;
        }
        const uint32_t layer = static_cast<uint32_t>(waterObject->layer);
        if (layer >= 32u || (context.cullingMask & (1u << layer)) == 0u) {
            continue;
        }
        const WaterComponent& water = *waterPtr;
        const Transform& transform = WaterSurfaceTransform(water, waterObject->transform,
            WaterSurfaceReadView::SIMULATION);
        // 旧Causticsは矩形マスクだけを持ち、Splineの輪郭外へ光を漏らすため対象にしない。
        if (WaterSurfaceEffectiveShape(water, WaterSurfaceReadView::SIMULATION) != WaterSurfaceShape::RECTANGLE
            || UsesWaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION)) {
            continue;
        }
        if (!IsWaterSurfaceSupported(water, transform, WaterSurfaceReadView::SIMULATION)) {
            continue;
        }

        asset::MaterialAsset::AuthoringSnapshotPtr matAuthoringSnapshot;
        const asset::MaterialAssetAuthoring* material = nullptr;
        if (!water.materialPath.empty()) {
            const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(water.materialPath);
            {
                matAuthoringSnapshot = asset::ReadMaterialAuthoring(handle);
                material = matAuthoringSnapshot ? &matAuthoringSnapshot->Data() : nullptr;
            }
        }

        auto getF = [material](const char* name, float def) -> float {
            if (!material) {
                return def;
            }
            const auto it = material->params.find(name);
            if (it != material->params.end() && !it->second.empty()) {
                return it->second[0];
            }
            return def;
        };
        auto getTex = [material](const char* name) -> std::string {
            if (!material) {
                return {};
            }
            const auto it = material->textures.find(name);
            if (it != material->textures.end()) {
                return it->second;
            }
            return {};
        };

        const asset::WaterOpticalSettings optics = (material ? asset::ResolveWaterOpticalSettings(*material) : asset::WaterOpticalSettings{});

        const float enableCaustics  = getF("enableCaustics", 0.0f);
        if (enableCaustics < 0.5f) {
            continue;
        }

        const float intensity = getF("causticsIntensity", 1.0f);
        if (intensity <= 0.0f) {
            continue;
        }

        float totalAmp = 0.0f;
        float weightedWaveFreq = 0.0f;
        for (size_t i = 0; i < (std::min)(static_cast<size_t>(water.waveCount), water.waves.size()); ++i) {
            const auto& wave = water.waves[i];
            if (wave.amplitude <= 0.0f || wave.wavelength <= math::EPSILON) {
                continue;
            }
            totalAmp += wave.amplitude;
            weightedWaveFreq += (math::TWO_PI / wave.wavelength) * wave.amplitude;
        }

        // 複数水面は最も強い設定を代表値として扱う。1 回のフルスクリーン加算で済ませるため、代表水面のみを投影元にする。
        if (result.enabled && intensity <= result.intensity) {
            continue;
        }
        float cosYaw = 1.0f;
        float sinYaw = 0.0f;
        if (!GetWaterYaw(transform, cosYaw, sinYaw)) {
            continue;
        }

        result.enabled     = true;
        result.intensity   = intensity;
        result.maxDistance = ReadBoundedMaterialFloat(material, "underwaterMaxDistance", 120.0f, 1.0f, 10000.0f);
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
        result.texture = texturePath.empty() ? renderer::ResourceHandle<renderer::TextureTag>{} : asset::StreamedTextureResolver::Engine().ResolveGpu(context.resources, texturePath);
    }
    return result;
}

}
