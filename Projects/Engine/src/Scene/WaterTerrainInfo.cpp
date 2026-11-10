/// @file    WaterTerrainInfo.cpp
/// @brief   複数 Terrain の鉛直水深・岸泡を検証し、水域 UV へ焼く。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <Engine/Scene/WaterTerrainInfo.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/TerrainSurfaceGeometry.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace fbzz::scene {
namespace {
constexpr uint64_t HASH_BASIS = 14695981039346656037ull;

/// @see https://www.ietf.org/archive/id/draft-eastlake-fnv-25.html FNV Basics, section 2（作業草案）。
void Mix(uint64_t& hash, uint64_t value)
{
    for (unsigned byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8u)) & 255u;
        hash *= 1099511628211ull;
    }
}
void MixFloat(uint64_t& hash, float value) { Mix(hash, std::bit_cast<uint32_t>(value)); }
void MixVector(uint64_t& hash, const math::Vector3& value)
{
    MixFloat(hash, value.x); MixFloat(hash, value.y); MixFloat(hash, value.z);
}
void MixTransform(uint64_t& hash, const Transform& transform)
{
    MixVector(hash, transform.worldPosition); MixVector(hash, transform.worldScale);
    MixFloat(hash, transform.worldRotation.x); MixFloat(hash, transform.worldRotation.y);
    MixFloat(hash, transform.worldRotation.z); MixFloat(hash, transform.worldRotation.w);
}
bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool ValidParameters(float waterY, float depthLimit, float threshold, float fade)
{
    return std::isfinite(waterY) && std::isfinite(depthLimit) && depthLimit > 0.0f
        && std::isfinite(threshold) && std::isfinite(fade) && fade >= 0.0f;
}
bool ValidTerrainShape(const TerrainComponent& terrain, const Transform& transform)
{
    if (terrain.columns < 2 || terrain.rows < 2 || !std::isfinite(terrain.cellSize) || terrain.cellSize <= 0.0f
        || !std::isfinite(terrain.maxHeight)) return false;
    const size_t columns = static_cast<size_t>(terrain.columns), rows = static_cast<size_t>(terrain.rows);
    if (rows > std::numeric_limits<size_t>::max() / columns || terrain.heightData.size() != columns * rows
        || (!terrain.holeData.empty() && terrain.holeData.size() != (columns - 1) * (rows - 1))) return false;
    math::Vector3 normal;
    return Finite(transform.worldPosition) && TryTerrainNormalToWorld(transform, math::Vector3::UP, normal);
}
uint64_t ContentSignature(const TerrainComponent& terrain)
{
    uint64_t hash = HASH_BASIS;
    Mix(hash, terrain.surfaceRevision); Mix(hash, terrain.heightDirty);
    Mix(hash, static_cast<uint64_t>(terrain.columns)); Mix(hash, static_cast<uint64_t>(terrain.rows));
    MixFloat(hash, terrain.cellSize); MixFloat(hash, terrain.maxHeight);
    Mix(hash, terrain.heightData.size()); Mix(hash, terrain.holeData.size());
    Mix(hash, terrain.surfacePositions.size()); Mix(hash, terrain.surfaceIndices.size());
    return hash;
}
bool SourceBounds(WaterTerrainSource& source, float minHeight, float maxHeight)
{
    const auto& terrain = *source.terrain;
    const float width = static_cast<float>(terrain.columns - 1) * terrain.cellSize;
    const float depth = static_cast<float>(terrain.rows - 1) * terrain.cellSize;
    bool first = true;
    for (const float x : {0.0f, width}) for (const float z : {0.0f, depth}) for (const float y : {minHeight, maxHeight}) {
        const auto& pose = source.transform;
        const auto world = pose.worldPosition + pose.worldRotation * math::Vector3{
            x * pose.worldScale.x, y * pose.worldScale.y, z * pose.worldScale.z};
        if (!Finite(world)) return false;
        if (first) { source.boundsMin = source.boundsMax = world; first = false; }
        else {
            source.boundsMin = {(std::min)(source.boundsMin.x, world.x), (std::min)(source.boundsMin.y, world.y), (std::min)(source.boundsMin.z, world.z)};
            source.boundsMax = {(std::max)(source.boundsMax.x, world.x), (std::max)(source.boundsMax.y, world.y), (std::max)(source.boundsMax.z, world.z)};
        }
    }
    for (const auto& local : terrain.surfacePositions) {
        const auto& pose = source.transform;
        const auto world = pose.worldPosition + pose.worldRotation * math::Vector3{
            local.x * pose.worldScale.x, local.y * pose.worldScale.y, local.z * pose.worldScale.z};
        if (!Finite(world)) return false;
        source.boundsMin = {(std::min)(source.boundsMin.x, world.x), (std::min)(source.boundsMin.y, world.y), (std::min)(source.boundsMin.z, world.z)};
        source.boundsMax = {(std::max)(source.boundsMax.x, world.x), (std::max)(source.boundsMax.y, world.y), (std::max)(source.boundsMax.z, world.z)};
    }
    /// @note TRS と鉛直 ray の丸め差で、水際の境界セルを早期除外しないよう外側へ広げる。
    const float marginX = (std::max)(0.0001f, 8.0f * std::numeric_limits<float>::epsilon()
        * (std::max)(std::abs(source.boundsMin.x), std::abs(source.boundsMax.x)));
    const float marginZ = (std::max)(0.0001f, 8.0f * std::numeric_limits<float>::epsilon()
        * (std::max)(std::abs(source.boundsMin.z), std::abs(source.boundsMax.z)));
    source.boundsMin.x -= marginX; source.boundsMax.x += marginX;
    source.boundsMin.z -= marginZ; source.boundsMax.z += marginZ;
    return Finite(source.boundsMin) && Finite(source.boundsMax);
}
float ShoreFoam(double depth, float threshold, float fade)
{
    if (fade == 0.0f) return depth < threshold ? 1.0f : 0.0f;
    const double t = (std::clamp)((depth - (static_cast<double>(threshold) - fade)) / (2.0 * fade), 0.0, 1.0);
    return static_cast<float>(1.0 - t * t * (3.0 - 2.0 * t));
}
} /// @note namespace

bool WaterTerrainInfoCache::TryCollect(const Scene& scene, WaterTerrainInfoInputs& out)
{
    if (m_sceneGeneration != scene.GetRenderSceneGeneration()) {
        m_sceneGeneration = scene.GetRenderSceneGeneration();
        m_validations.clear();
    }
    WaterTerrainInfoInputs inputs;
    inputs.signature = HASH_BASIS;
    Mix(inputs.signature, m_sceneGeneration);
    std::unordered_set<uint64_t> live;
    for (const EntityID id : scene.GetEntities<TerrainComponent>()) {
        auto* object = scene.GetGameObject(id);
        const auto* terrain = object ? object->GetComponent<TerrainComponent>() : nullptr;
        if (!object || !terrain || !terrain->enabled || !object->activeInHierarchy() || terrain->heightData.empty()) continue;
        if (!ValidTerrainShape(*terrain, object->transform)) return false;
        const uint64_t entityKey = (static_cast<uint64_t>(id.generation) << 32u) | id.index;
        const uint64_t key = scene.GetComponentLifetime<TerrainComponent>(id);
        if (key == 0) return false;
        live.insert(key);
        const uint64_t content = ContentSignature(*terrain);
        auto found = m_validations.find(key);
        if (found == m_validations.end() || found->second.signature != content) {
            Validation validation;
            validation.signature = content;
            validation.valid = IsValidTerrainSurfaceGeometry(terrain->surfacePositions, terrain->surfaceIndices);
            bool first = true;
            for (const float value : terrain->heightData) {
                const float height = value * terrain->maxHeight;
                if (!std::isfinite(height)) { validation.valid = false; break; }
                if (first) { validation.minHeight = validation.maxHeight = height; first = false; }
                else {
                    validation.minHeight = (std::min)(validation.minHeight, height);
                    validation.maxHeight = (std::max)(validation.maxHeight, height);
                }
            }
            found = m_validations.insert_or_assign(key, validation).first;
        }
        if (!found->second.valid) return false;
        WaterTerrainSource source{terrain, object->transform, {}, {}};
        if (!SourceBounds(source, found->second.minHeight, found->second.maxHeight)) return false;
        inputs.terrains.push_back(source);
        Mix(inputs.signature, entityKey); Mix(inputs.signature, key);
        Mix(inputs.signature, content); MixTransform(inputs.signature, source.transform);
    }
    std::erase_if(m_validations, [&live](const auto& entry) { return !live.contains(entry.first); });
    out = std::move(inputs);
    return true;
}

bool TrySampleWaterTerrainInfo(const WaterTerrainInfoInputs& inputs, math::Vector2 worldXZ,
    float meanWaterY, float depthLimit, float foamThreshold, float foamFade, WaterTerrainSample& out)
{
    if (!std::isfinite(worldXZ.x) || !std::isfinite(worldXZ.y)
        || !ValidParameters(meanWaterY, depthLimit, foamThreshold, foamFade)) return false;
    WaterTerrainSample sample;
    sample.signedDepth = sample.depth = depthLimit;
    for (const auto& source : inputs.terrains) {
        if (!source.terrain || !Finite(source.boundsMin) || !Finite(source.boundsMax)) return false;
        if (worldXZ.x < source.boundsMin.x || worldXZ.x > source.boundsMax.x
            || worldXZ.y < source.boundsMin.z || worldXZ.y > source.boundsMax.z) continue;
        TerrainSurfaceSample bottom;
        if (!TrySampleTerrainWorld(*source.terrain, source.transform, worldXZ.x, worldXZ.y, bottom)) continue;
        if (!sample.hasBottom || bottom.position.y > sample.bottomY) {
            sample.hasBottom = true;
            sample.bottomY = bottom.position.y;
        }
    }
    if (sample.hasBottom) {
        sample.signedDepth = static_cast<double>(meanWaterY) - sample.bottomY;
        sample.dry = sample.signedDepth <= 0.0;
        sample.depth = static_cast<float>((std::clamp)(sample.signedDepth, 0.0, static_cast<double>(depthLimit)));
        sample.shoreFoam = sample.dry ? 0.0f : ShoreFoam(sample.signedDepth, foamThreshold, foamFade);
    }
    out = sample;
    return true;
}

math::Vector2 ResolveWaterTerrainBakeResolution(const WaterTerrainInfoInputs& inputs,
    const WaterComponent& water, const Transform& transform, WaterSurfaceReadView view)
{
    if (!inputs.terrains.empty() && WaterSurfaceUsesHorizon(water, view)) {
        constexpr float resolution = static_cast<float>(MAX_HORIZON_WATER_TERRAIN_RESOLUTION);
        return {resolution, resolution};
    }
    const auto extent = WaterSurfaceLocalExtent(water, view);
    constexpr float maxResolution = static_cast<float>(MAX_FINITE_WATER_TERRAIN_RESOLUTION);
    const auto dimension = [maxResolution](float size, float scale) {
        const float meters = size * std::abs(scale);
        return std::isfinite(meters) ? (std::clamp)(std::ceil(meters), 64.0f, maxResolution) : 64.0f;
    };
    return {dimension(extent.x, transform.worldScale.x), dimension(extent.y, transform.worldScale.z)};
}

bool TryComputeWaterTerrainInfoSignature(const WaterTerrainInfoInputs& inputs, const WaterComponent& water,
    const Transform& transform, const WaterTerrainBakeSettings& settings, uint64_t& out,
    WaterSurfaceReadView view)
{
    const float depthRange = settings.depthRange == 0.0f ? water.buoyancyDepth : settings.depthRange;
    const float attenuationDepth = water.EffectiveWaveAttenuationDepth(view);
    const bool horizon = WaterSurfaceUsesHorizon(water, view);
    const uint32_t maxResolution = horizon ? MAX_HORIZON_WATER_TERRAIN_RESOLUTION : MAX_FINITE_WATER_TERRAIN_RESOLUTION;
    if (!IsWaterSurfaceSupported(water, transform, view)
        || !std::isfinite(water.buoyancyDepth) || water.buoyancyDepth < 0.0f
        || !std::isfinite(attenuationDepth) || attenuationDepth < 0.0f
        || !std::isfinite(settings.depthRange) || settings.depthRange < 0.0f
        || !ValidParameters(transform.worldPosition.y, depthRange, settings.foamThreshold, settings.foamFade)
        || settings.width == 0 || settings.height == 0
        || settings.width > maxResolution || settings.height > maxResolution) return false;
    uint64_t signature = inputs.signature;
    MixTransform(signature, transform);
    MixFloat(signature, water.extentX); MixFloat(signature, water.extentZ); MixFloat(signature, water.buoyancyDepth);
    MixFloat(signature, attenuationDepth);
    Mix(signature, horizon);
    Mix(signature, static_cast<uint64_t>(WaterSurfaceEffectiveShape(water, view)));
    if (const WaterSplineSnapshot* geometry = WaterSurfaceGeometry(water, view))
        Mix(signature, geometry->signature);
    MixFloat(signature, settings.foamThreshold); MixFloat(signature, settings.foamFade);
    MixFloat(signature, settings.depthRange);
    Mix(signature, settings.width); Mix(signature, settings.height);
    out = signature;
    return true;
}

bool TryBakeWaterTerrainInfo(const WaterTerrainInfoInputs& inputs, const WaterComponent& water,
    const Transform& transform, const WaterTerrainBakeSettings& settings, WaterTerrainInfoAtlas& out,
    WaterSurfaceReadView view)
{
    WaterTerrainInfoAtlas atlas;
    if (!TryComputeWaterTerrainInfoSignature(inputs, water, transform, settings, atlas.signature, view)) return false;
    atlas.width = settings.width; atlas.height = settings.height;
    atlas.depthRange = settings.depthRange == 0.0f ? water.buoyancyDepth : settings.depthRange;
    atlas.rgba.resize(static_cast<size_t>(atlas.width) * atlas.height * 4u);
    const auto world = transform.GetWorldMatrix();
    const auto extent = WaterSurfaceLocalExtent(water, view);
    math::Vector2 localCenter{};
    math::Vector2 localExtent = extent;
    if (WaterSurfaceUsesHorizon(water, view) && !inputs.terrains.empty()) {
        const auto inverseRotation = transform.worldRotation.Inverse();
        const float signX = std::copysign(1.0f, transform.worldScale.x);
        const float signZ = std::copysign(1.0f, transform.worldScale.z);
        const float largest = std::numeric_limits<float>::max();
        math::Vector2 minimum{largest, largest}, maximum{-largest, -largest};
        for (const auto& terrain : inputs.terrains) {
            for (float x : {terrain.boundsMin.x, terrain.boundsMax.x}) {
                for (float z : {terrain.boundsMin.z, terrain.boundsMax.z}) {
                    const auto local = inverseRotation * math::Vector3{
                        x - transform.worldPosition.x, 0.0f, z - transform.worldPosition.z};
                    minimum.x = (std::min)(minimum.x, local.x * signX);
                    minimum.y = (std::min)(minimum.y, local.z * signZ);
                    maximum.x = (std::max)(maximum.x, local.x * signX);
                    maximum.y = (std::max)(maximum.y, local.z * signZ);
                }
            }
        }
        const auto center = minimum * 0.5f + maximum * 0.5f;
        const auto span = maximum - minimum;
        /// @note One empty texel at each side prevents the boundary clamp from extending the last Terrain tile into open water.
        const math::Vector2 padded{
            span.x + 2.0f * (std::max)(1.0f, span.x / (std::max)(1.0f, static_cast<float>(atlas.width) - 2.0f)),
            span.y + 2.0f * (std::max)(1.0f, span.y / (std::max)(1.0f, static_cast<float>(atlas.height) - 2.0f))};
        if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(padded.x)
            || !std::isfinite(padded.y) || padded.x <= 0.0f || padded.y <= 0.0f) return false;
        const float scaleX = std::abs(transform.worldScale.x), scaleZ = std::abs(transform.worldScale.z);
        atlas.domain = {center.x, center.y, padded.x, padded.y};
        localCenter = {center.x / scaleX, center.y / scaleZ};
        localExtent = {padded.x / scaleX, padded.y / scaleZ};
    }
    const bool usesGeometry = UsesWaterSurfaceGeometry(water, view);
    const WaterSplineSnapshot* geometry = WaterSurfaceGeometry(water, view);
    for (uint32_t z = 0; z < atlas.height; ++z) for (uint32_t x = 0; x < atlas.width; ++x) {
        const auto point = world * math::Vector4{
            localCenter.x + ((static_cast<float>(x) + 0.5f) / atlas.width - 0.5f) * localExtent.x, 0.0f,
            localCenter.y + ((static_cast<float>(z) + 0.5f) / atlas.height - 0.5f) * localExtent.y, 1.0f};
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) return false;
        float meanWaterY = point.y;
        WaterSplineSample spline;
        if (usesGeometry) {
            if (!geometry || !TrySampleWaterSpline(*geometry, point.x, point.z, spline)) continue;
            meanWaterY = spline.baseHeight;
        }
        WaterTerrainSample sample;
        if (!TrySampleWaterTerrainInfo(inputs, {point.x, point.z}, meanWaterY, atlas.depthRange,
            settings.foamThreshold, settings.foamFade, sample)) return false;
        if (usesGeometry) {
            /// @note Splineの指定水深は仮想の底を持つ。地形の底がさらに浅ければ、そちらを水深情報へ使う。
            sample.signedDepth = sample.hasBottom
                ? (std::min)(sample.signedDepth, static_cast<double>(spline.depth))
                : static_cast<double>(spline.depth);
            sample.hasBottom = true;
            sample.dry = sample.signedDepth <= 0.0;
            sample.depth = static_cast<float>((std::clamp)(sample.signedDepth, 0.0, static_cast<double>(atlas.depthRange)));
            sample.shoreFoam = sample.dry ? 0.0f : ShoreFoam(sample.signedDepth, settings.foamThreshold, settings.foamFade);
        }
        const double normalized = (std::clamp)(sample.signedDepth / atlas.depthRange, -1.0, 1.0) * 0.5 + 0.5;
        const uint32_t word = static_cast<uint32_t>(std::round(normalized * 65535.0));
        const size_t pixel = (static_cast<size_t>(z) * atlas.width + x) * 4u;
        atlas.rgba[pixel] = static_cast<uint8_t>(std::round(sample.shoreFoam * 255.0f));
        atlas.rgba[pixel + 1] = static_cast<uint8_t>(word >> 8u);
        atlas.rgba[pixel + 2] = static_cast<uint8_t>(word & 255u);
        const bool clipped = sample.hasBottom
            && (sample.signedDepth < -static_cast<double>(atlas.depthRange) || sample.signedDepth > atlas.depthRange);
        atlas.rgba[pixel + 3] = static_cast<uint8_t>((sample.hasBottom ? 1u : 0u)
            | (sample.dry ? 2u : 0u) | (clipped ? 4u : 0u));
    }
    out = std::move(atlas);
    return true;
}

bool TryDecodeWaterTerrainDepth(uint8_t highByte, uint8_t lowByte, float depthRange, float& out)
{
    if (!std::isfinite(depthRange) || depthRange <= 0.0f) return false;
    const uint32_t word = (static_cast<uint32_t>(highByte) << 8u) | lowByte;
    const double depth = (static_cast<double>(word) / 65535.0 * 2.0 - 1.0) * depthRange;
    const float result = static_cast<float>(depth);
    if (!std::isfinite(result)) return false;
    out = result;
    return true;
}
} /// @note namespace fbzz::scene
