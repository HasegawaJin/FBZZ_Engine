/// @file    WaterDepthField.cpp
/// @brief   RGBA8 地形水深を不変の浅水波減衰 field へ準備する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <Engine/Scene/WaterDepthField.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <Engine/Scene/WaterTerrainInfo.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace fbzz::scene {
namespace {
constexpr size_t kSceneCacheCapacity = 4;
std::array<WaterTerrainInfoCache, kSceneCacheCapacity> g_terrainCaches;
std::array<uint64_t, kSceneCacheCapacity> g_cacheGenerations{};
std::array<uint64_t, kSceneCacheCapacity> g_cacheUse{};
uint64_t g_cacheClock = 0;

bool Finite(const math::Vector2& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Finite(const math::Quaternion& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z) && std::isfinite(value.w);
}

WaterTerrainInfoCache& TerrainCacheFor(const Scene& scene)
{
    const uint64_t generation = scene.GetRenderSceneGeneration();
    for (size_t i = 0; i < kSceneCacheCapacity; ++i) {
        if (g_cacheGenerations[i] == generation) {
            g_cacheUse[i] = ++g_cacheClock;
            return g_terrainCaches[i];
        }
    }
    size_t target = 0;
    for (size_t i = 1; i < kSceneCacheCapacity; ++i)
        if (g_cacheUse[i] < g_cacheUse[target]) target = i;
    g_cacheGenerations[target] = generation;
    g_cacheUse[target] = ++g_cacheClock;
    g_terrainCaches[target] = WaterTerrainInfoCache{};
    return g_terrainCaches[target];
}

float MaterialFloat(const asset::MaterialAssetAuthoring* material, const char* key, float fallback)
{
    if (!material) return fallback;
    const auto found = material->params.find(key);
    if (found == material->params.end() || found->second.empty()
        || !std::isfinite(found->second.front())) return fallback;
    return found->second.front();
}

/// @brief Smoothstep(0, threshold, depth) による浅瀬の振幅係数。
/// @see Docs/design/water-wave-optics-morph.md 「浅水減衰」。
float SmoothAttenuation(float depth, float threshold)
{
    if (depth <= 0.0f) return 0.0f;
    if (depth >= threshold) return 1.0f;
    const float t = depth / threshold;
    return t * t * (3.0f - 2.0f * t);
}

bool TexelFactor(const WaterDepthField& field, size_t pixel, float& out)
{
    const uint8_t flags = field.rgba[pixel + 3];
    if ((flags & 1u) == 0u) {
        out = 1.0f;
        return true;
    }
    if ((flags & 2u) != 0u) {
        out = 0.0f;
        return true;
    }
    float depth = 0.0f;
    if (!TryDecodeWaterTerrainDepth(field.rgba[pixel + 1], field.rgba[pixel + 2],
            field.depthRange, depth)) return false;
    if ((flags & 4u) != 0u) {
        out = depth > 0.0f ? 1.0f : 0.0f;
        return true;
    }
    out = SmoothAttenuation(depth, field.attenuationDepth);
    return std::isfinite(out);
}

bool ValidMapping(const WaterDepthWorldMapping& mapping)
{
    const double rotationLength = mapping.worldToLocalRotation.Length();
    return Finite(mapping.worldPosition) && Finite(mapping.worldToLocalRotation)
        && std::isfinite(mapping.signedScaleX) && std::isfinite(mapping.signedScaleZ)
        && std::abs(mapping.signedScaleX) >= 0.0001f && std::abs(mapping.signedScaleZ) >= 0.0001f
        && std::isfinite(mapping.extentX) && mapping.extentX > 0.0f
        && std::isfinite(mapping.extentZ) && mapping.extentZ > 0.0f
        && std::isfinite(rotationLength) && std::abs(rotationLength - 1.0) <= 0.001;
}
} /// @note namespace

bool TryBuildWaterDepthField(const WaterTerrainInfoAtlas& atlas, const WaterComponent& water,
    const Transform& transform, WaterDepthField& out, WaterSurfaceReadView view)
{
    const size_t pixelCount = static_cast<size_t>(atlas.width) * atlas.height;
    const float attenuationDepth = water.EffectiveWaveAttenuationDepth(view);
    if (atlas.width < 2 || atlas.height < 2 || atlas.width > 257 || atlas.height > 257
        || pixelCount > std::numeric_limits<size_t>::max() / 4u
        || atlas.rgba.size() != pixelCount * 4u
        || !std::isfinite(atlas.depthRange) || atlas.depthRange <= 0.0f
        || !std::isfinite(attenuationDepth) || attenuationDepth <= 0.0f
        || (UsesWaterSurfaceGeometry(water, view) && !WaterSurfaceGeometry(water, view))
        || !IsWaterSurfaceSupported(water, transform, view)) return false;

    WaterDepthField field;
    field.width = atlas.width;
    field.height = atlas.height;
    field.depthRange = atlas.depthRange;
    field.attenuationDepth = attenuationDepth;
    field.signature = atlas.signature;
    field.mapping.worldPosition = transform.worldPosition;
    field.mapping.worldToLocalRotation = transform.worldRotation.Inverse();
    field.mapping.signedScaleX = transform.worldScale.x;
    field.mapping.signedScaleZ = transform.worldScale.z;
    const auto extent = WaterSurfaceLocalExtent(water, view);
    field.mapping.extentX = extent.x;
    field.mapping.extentZ = extent.y;
    field.rgba = atlas.rgba;
    if (!ValidMapping(field.mapping)) return false;
    const float texelWidthX = std::abs(field.mapping.signedScaleX)
        * field.mapping.extentX / static_cast<float>(field.width);
    const float texelWidthZ = std::abs(field.mapping.signedScaleZ)
        * field.mapping.extentZ / static_cast<float>(field.height);
    field.outsideFadeWidth = (std::max)({texelWidthX, texelWidthZ, 1.0f});
    if (!std::isfinite(field.outsideFadeWidth)) return false;

    std::vector<float> factors(pixelCount);
    float minimumBoundaryFactor = 1.0f;
    for (size_t i = 0; i < pixelCount; ++i) {
        if (!TexelFactor(field, i * 4u, factors[i])) return false;
        const uint32_t x = static_cast<uint32_t>(i % field.width);
        const uint32_t z = static_cast<uint32_t>(i / field.width);
        if (x == 0 || z == 0 || x + 1 == field.width || z + 1 == field.height)
            minimumBoundaryFactor = (std::min)(minimumBoundaryFactor, factors[i]);
    }

    float maximumU = 0.0f;
    float maximumV = 0.0f;
    for (uint32_t z = 0; z < field.height; ++z) {
        for (uint32_t x = 0; x < field.width; ++x) {
            const size_t index = static_cast<size_t>(z) * field.width + x;
            if (x + 1 < field.width)
                maximumU = (std::max)(maximumU, std::abs(factors[index + 1] - factors[index]));
            if (z + 1 < field.height)
                maximumV = (std::max)(maximumV,
                    std::abs(factors[index + field.width] - factors[index]));
        }
    }
    const double gradientX = static_cast<double>(maximumU) * field.width
        / (std::abs(field.mapping.signedScaleX) * field.mapping.extentX);
    const double gradientZ = static_cast<double>(maximumV) * field.height
        / (std::abs(field.mapping.signedScaleZ) * field.mapping.extentZ);
    const double interiorGradient = std::sqrt(gradientX * gradientX + gradientZ * gradientZ);
    /// @note smoothstep の最大傾きは 1.5/width。最小境界 texel が外側の差分を上から抑える。
    const double outsideGradient = 1.5 * (1.0 - minimumBoundaryFactor) / field.outsideFadeWidth;
    field.maximumAttenuationGradient = static_cast<float>(interiorGradient + outsideGradient);
    if (!std::isfinite(field.maximumAttenuationGradient)) return false;
    out = std::move(field);
    return true;
}

bool TrySampleWaterDepthField(const WaterDepthField& field, math::Vector2 worldXZ, WaterDepthSample& out)
{
    const size_t pixelCount = static_cast<size_t>(field.width) * field.height;
    if (!Finite(worldXZ) || field.width < 2 || field.height < 2
        || field.width > 257 || field.height > 257
        || pixelCount > std::numeric_limits<size_t>::max() / 4u
        || field.rgba.size() != pixelCount * 4u
        || !std::isfinite(field.depthRange) || field.depthRange <= 0.0f
        || !std::isfinite(field.attenuationDepth) || field.attenuationDepth <= 0.0f
        || !std::isfinite(field.outsideFadeWidth) || field.outsideFadeWidth <= 0.0f
        || !std::isfinite(field.maximumAttenuationGradient) || field.maximumAttenuationGradient < 0.0f
        || !ValidMapping(field.mapping)) return false;

    const math::Vector3 relative{
        worldXZ.x - field.mapping.worldPosition.x, 0.0f,
        worldXZ.y - field.mapping.worldPosition.z};
    const math::Vector3 local = field.mapping.worldToLocalRotation * relative;
    const float u = local.x / field.mapping.signedScaleX / field.mapping.extentX + 0.5f;
    const float v = local.z / field.mapping.signedScaleZ / field.mapping.extentZ + 0.5f;
    if (!std::isfinite(u) || !std::isfinite(v)) return false;
    WaterDepthSample sample;
    const float outsideUvX = u < 0.0f ? -u : (u > 1.0f ? u - 1.0f : 0.0f);
    const float outsideUvZ = v < 0.0f ? -v : (v > 1.0f ? v - 1.0f : 0.0f);
    const float outsideX = outsideUvX * std::abs(field.mapping.signedScaleX) * field.mapping.extentX;
    const float outsideZ = outsideUvZ * std::abs(field.mapping.signedScaleZ) * field.mapping.extentZ;
    const float outsideDistance = std::sqrt(outsideX * outsideX + outsideZ * outsideZ);
    if (outsideDistance >= field.outsideFadeWidth) {
        out = sample;
        return true;
    }
    if (!std::isfinite(outsideDistance)) return false;

    const float clampedU = std::clamp(u, 0.0f, 1.0f);
    const float clampedV = std::clamp(v, 0.0f, 1.0f);
    const float x = clampedU * static_cast<float>(field.width) - 0.5f;
    const float z = clampedV * static_cast<float>(field.height) - 0.5f;
    const int rawX0 = static_cast<int>(std::floor(x));
    const int rawZ0 = static_cast<int>(std::floor(z));
    const int x0 = std::clamp(rawX0, 0, static_cast<int>(field.width) - 1);
    const int x1 = std::clamp(rawX0 + 1, 0, static_cast<int>(field.width) - 1);
    const int z0 = std::clamp(rawZ0, 0, static_cast<int>(field.height) - 1);
    const int z1 = std::clamp(rawZ0 + 1, 0, static_cast<int>(field.height) - 1);
    const float tx = x - static_cast<float>(rawX0);
    const float tz = z - static_cast<float>(rawZ0);
    const auto pixel = [&field](int px, int pz) {
        return (static_cast<size_t>(pz) * field.width + static_cast<size_t>(px)) * 4u;
    };
    float f00 = 0.0f, f10 = 0.0f, f01 = 0.0f, f11 = 0.0f;
    if (!TexelFactor(field, pixel(x0, z0), f00)
        || !TexelFactor(field, pixel(x1, z0), f10)
        || !TexelFactor(field, pixel(x0, z1), f01)
        || !TexelFactor(field, pixel(x1, z1), f11)) return false;

    const float lower = f00 + (f10 - f00) * tx;
    const float upper = f01 + (f11 - f01) * tx;
    sample.factor = std::clamp(lower + (upper - lower) * tz, 0.0f, 1.0f);
    const float derivativeU = u < 0.0f || u > 1.0f ? 0.0f
        : ((f10 - f00) * (1.0f - tz) + (f11 - f01) * tz) * static_cast<float>(field.width);
    const float derivativeV = v < 0.0f || v > 1.0f ? 0.0f
        : ((f01 - f00) * (1.0f - tx) + (f11 - f10) * tx) * static_cast<float>(field.height);
    const math::Vector3 worldXToLocal = field.mapping.worldToLocalRotation * math::Vector3::RIGHT;
    const math::Vector3 worldZToLocal = field.mapping.worldToLocalRotation * math::Vector3::FORWARD;
    const float duDx = worldXToLocal.x / field.mapping.signedScaleX / field.mapping.extentX;
    const float dvDx = worldXToLocal.z / field.mapping.signedScaleZ / field.mapping.extentZ;
    const float duDz = worldZToLocal.x / field.mapping.signedScaleX / field.mapping.extentX;
    const float dvDz = worldZToLocal.z / field.mapping.signedScaleZ / field.mapping.extentZ;
    sample.gradient = {
        derivativeU * duDx + derivativeV * dvDx,
        derivativeU * duDz + derivativeV * dvDz
    };
    if (outsideDistance > 0.0f) {
        const float t = outsideDistance / field.outsideFadeWidth;
        const float fade = t * t * (3.0f - 2.0f * t);
        const float fadeDerivative = 6.0f * t * (1.0f - t) / field.outsideFadeWidth;
        const float localOutsideX = std::copysign(outsideX,
            field.mapping.signedScaleX * (u < 0.0f ? -1.0f : 1.0f));
        const float localOutsideZ = std::copysign(outsideZ,
            field.mapping.signedScaleZ * (v < 0.0f ? -1.0f : 1.0f));
        const math::Vector3 outsideWorldUnit = field.mapping.worldToLocalRotation.Inverse()
            * math::Vector3{localOutsideX / outsideDistance, 0.0f, localOutsideZ / outsideDistance};
        const float innerFactor = sample.factor;
        sample.factor = innerFactor + (1.0f - innerFactor) * fade;
        sample.gradient = sample.gradient * (1.0f - fade)
            + math::Vector2{outsideWorldUnit.x, outsideWorldUnit.z}
                * ((1.0f - innerFactor) * fadeDerivative);
    }
    if (!std::isfinite(sample.factor) || !Finite(sample.gradient)) return false;
    out = sample;
    return true;
}

void PrepareWaterDepthFields(Scene& scene, WaterSurfaceReadView view)
{
    WaterTerrainInfoInputs inputs;
    if (!TerrainCacheFor(scene).TryCollect(scene, inputs)) {
        for (EntityID id : scene.GetEntities<WaterComponent>())
            if (auto* water = scene.GetComponent<WaterComponent>(id)) {
                const bool hasPublishedSimulation = view == WaterSurfaceReadView::SIMULATION
                    && water->simulationReadEnabled && water->simulationSurfacePublished;
                if (!hasPublishedSimulation) water->depthField.reset();
            }
        return;
    }

    std::unordered_map<std::string, asset::MaterialAsset::AuthoringSnapshotPtr> materials;
    for (EntityID id : scene.GetEntities<WaterComponent>()) {
        WaterComponent* water = scene.GetComponent<WaterComponent>(id);
        GameObject* object = scene.GetGameObject(id);
        const bool hasPublishedSimulation = water && view == WaterSurfaceReadView::SIMULATION
            && water->simulationReadEnabled && water->simulationSurfacePublished;
        if (hasPublishedSimulation) continue;
        if (water && WaterSurfaceZoneOwner(*water, view).IsValid()) {
            water->depthField.reset();
            continue;
        }
        const float attenuationDepth = water ? water->EffectiveWaveAttenuationDepth(view) : 0.0f;
        if (!water || !object || !water->enabled || !object->activeInHierarchy()
            || !std::isfinite(attenuationDepth) || attenuationDepth <= 0.0f
            || !IsWaterSurfaceSupported(*water, WaterSurfaceTransform(*water, object->transform, view), view)) {
            if (water) water->depthField.reset();
            continue;
        }

        asset::MaterialAsset::AuthoringSnapshotPtr materialSnapshot;
        if (!water->materialPath.empty()) {
            const auto found = materials.find(water->materialPath);
            if (found != materials.end()) {
                materialSnapshot = found->second;
            } else {
                const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(water->materialPath);
                materialSnapshot = asset::ReadMaterialAuthoring(handle);
                materials.emplace(water->materialPath, materialSnapshot);
            }
        }

        const auto* material = materialSnapshot ? &materialSnapshot->Data() : nullptr;
        WaterTerrainBakeSettings settings;
        const Transform& pose = WaterSurfaceTransform(*water, object->transform, view);
        const auto extent = WaterSurfaceLocalExtent(*water, view);
        settings.width = static_cast<uint32_t>((std::clamp)(
            std::ceil(static_cast<double>(extent.x) * std::abs(pose.worldScale.x)),
            64.0, 257.0));
        settings.height = static_cast<uint32_t>((std::clamp)(
            std::ceil(static_cast<double>(extent.y) * std::abs(pose.worldScale.z)),
            64.0, 257.0));
        settings.foamThreshold = (std::max)(MaterialFloat(material, "foamThreshold", 0.3f), 0.0f);
        settings.foamFade = (std::max)(MaterialFloat(material, "foamFade", 0.5f), 0.0f);
        settings.depthRange = (std::max)(water->buoyancyDepth, attenuationDepth);

        uint64_t signature = 0;
        if (!TryComputeWaterTerrainInfoSignature(inputs, *water, pose, settings, signature, view)) {
            water->depthField.reset();
            continue;
        }
        if (water->depthField && water->depthField->signature == signature
            && water->depthField->attenuationDepth == attenuationDepth) continue;

        WaterTerrainInfoAtlas atlas;
        WaterDepthField field;
        if (!TryBakeWaterTerrainInfo(inputs, *water, pose, settings, atlas, view)
            || !TryBuildWaterDepthField(atlas, *water, pose, field, view)) {
            water->depthField.reset();
            continue;
        }
        water->depthField = std::make_shared<const WaterDepthField>(std::move(field));
    }
}
} /// @note namespace fbzz::scene

