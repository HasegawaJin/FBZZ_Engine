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
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <utility>
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
void MixTerrainLayout(uint64_t& hash, const TerrainComponent& terrain)
{
    Mix(hash, static_cast<uint64_t>(terrain.columns)); Mix(hash, static_cast<uint64_t>(terrain.rows));
    MixFloat(hash, terrain.cellSize); MixFloat(hash, terrain.maxHeight);
    Mix(hash, terrain.heightData.size());
    Mix(hash, terrain.surfacePositions.size()); Mix(hash, terrain.surfaceIndices.size());
}
uint64_t ContentSignature(const TerrainComponent& terrain)
{
    uint64_t hash = HASH_BASIS;
    Mix(hash, terrain.surfaceRevision); Mix(hash, terrain.heightDirty);
    MixTerrainLayout(hash, terrain);
    Mix(hash, terrain.holeData.size());
    return hash;
}
bool IsUpright(const Transform& transform)
{
    const auto up = transform.worldRotation * math::Vector3::UP;
    return up.x == 0.0f && up.z == 0.0f;
}
bool SupportsLocalAtlasProjection(const Transform& transform)
{
    const auto rotation = transform.worldRotation;
    /// @note The height query uses the quaternion inverse. Restrict regional projection to normalized yaw poses.
    return rotation.x == 0.0f && rotation.z == 0.0f && std::abs(rotation.Length() - 1.0f) <= 0.000001f;
}
bool IsContiguousHeightPreview(const TerrainComponent& terrain, uint64_t previousRevision)
{
    const auto& preview = terrain.heightPreview;
    const auto& region = preview.region;
    return !terrain.heightDirty && preview.revision == terrain.surfaceRevision
        && preview.baseRevision == previousRevision && preview.baseRevision != preview.revision
        && !region.Empty() && region.minX >= 0 && region.minZ >= 0
        && region.maxX < terrain.columns && region.maxZ < terrain.rows;
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

bool ValidWaterTerrainSource(const WaterTerrainSource& source)
{
    return source.terrain && Finite(source.boundsMin) && Finite(source.boundsMax);
}

void ConsiderWaterTerrainBottom(const WaterTerrainSource& source, math::Vector2 worldXZ, WaterTerrainSample& sample)
{
    if (worldXZ.x < source.boundsMin.x || worldXZ.x > source.boundsMax.x
        || worldXZ.y < source.boundsMin.z || worldXZ.y > source.boundsMax.z) return;
    TerrainSurfaceSample bottom;
    if (TrySampleTerrainWorld(*source.terrain, source.transform, worldXZ.x, worldXZ.y, bottom)
        && (!sample.hasBottom || bottom.position.y > sample.bottomY)) {
        sample.hasBottom = true;
        sample.bottomY = bottom.position.y;
    }
}

void SetWaterTerrainDepth(WaterTerrainSample& sample, float meanWaterY, float depthLimit, float foamThreshold, float foamFade)
{
    sample.signedDepth = sample.depth = depthLimit;
    if (sample.hasBottom) {
        sample.signedDepth = static_cast<double>(meanWaterY) - sample.bottomY;
        sample.dry = sample.signedDepth <= 0.0;
        sample.depth = static_cast<float>((std::clamp)(sample.signedDepth, 0.0, static_cast<double>(depthLimit)));
        sample.shoreFoam = sample.dry ? 0.0f : ShoreFoam(sample.signedDepth, foamThreshold, foamFade);
    }
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
        const bool upright = IsUpright(object->transform);
        if (found == m_validations.end() || found->second.signature != content
            || (found->second.conservative && !upright)) {
            uint64_t layout = HASH_BASIS;
            MixTerrainLayout(layout, *terrain);
            Validation validation;
            validation.signature = content;
            validation.revision = terrain->surfaceRevision;
            validation.layout = layout;
            const bool local = found != m_validations.end() && found->second.valid
                && found->second.layout == layout && upright
                && IsContiguousHeightPreview(*terrain, found->second.revision);
            const auto validate = [&](std::span<const float> values) {
                for (const float value : values) {
                    const float height = value * terrain->maxHeight;
                    if (!std::isfinite(height)) { validation.valid = false; return; }
                    validation.minHeight = (std::min)(validation.minHeight, height);
                    validation.maxHeight = (std::max)(validation.maxHeight, height);
                }
            };
            if (local) {
                /// @note Previous extrema remain conservative; upright terrain XZ bounds do not depend on height.
                validation.valid = true;
                validation.conservative = true;
                validation.minHeight = found->second.minHeight;
                validation.maxHeight = found->second.maxHeight;
                if (terrain->heightPreview.heightChanged) {
                    const auto& region = terrain->heightPreview.region;
                    const auto heights = std::span<const float>(terrain->heightData);
                    const auto width = static_cast<size_t>(region.maxX - region.minX + 1);
                    for (int z = region.minZ; z <= region.maxZ && validation.valid; ++z)
                        validate(heights.subspan(static_cast<size_t>(z) * terrain->columns + region.minX, width));
                }
            } else {
                validation.valid = IsValidTerrainSurfaceGeometry(terrain->surfacePositions, terrain->surfaceIndices);
                validation.minHeight = std::numeric_limits<float>::infinity();
                validation.maxHeight = -validation.minHeight;
                validate(terrain->heightData);
            }
            found = m_validations.insert_or_assign(key, validation).first;
        }
        if (!found->second.valid) return false;
        WaterTerrainSource source{terrain, object->transform, {}, {}, key};
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
    for (const auto& source : inputs.terrains) {
        if (!ValidWaterTerrainSource(source)) return false;
        ConsiderWaterTerrainBottom(source, worldXZ, sample);
    }
    SetWaterTerrainDepth(sample, meanWaterY, depthLimit, foamThreshold, foamFade);
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

namespace {
bool TryWaterTerrainSignature(uint64_t sourceSignature, const WaterComponent& water,
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
    uint64_t signature = sourceSignature;
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

struct AtlasMapping {
    math::Vector2 center;
    math::Vector2 extent;
};

bool PrepareWaterTerrainAtlas(const WaterTerrainInfoInputs& inputs, const WaterComponent& water,
    const Transform& transform, const WaterTerrainBakeSettings& settings, WaterTerrainInfoAtlas& atlas, AtlasMapping& mapping,
    WaterSurfaceReadView view)
{
    if (!TryWaterTerrainSignature(inputs.signature, water, transform, settings, atlas.signature, view)) return false;
    atlas.width = settings.width; atlas.height = settings.height;
    atlas.depthRange = settings.depthRange == 0.0f ? water.buoyancyDepth : settings.depthRange;
    mapping.extent = WaterSurfaceLocalExtent(water, view);
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
        mapping.center = {center.x / scaleX, center.y / scaleZ};
        mapping.extent = {padded.x / scaleX, padded.y / scaleZ};
    }
    return true;
}

bool BakeWaterTerrainAtlasRegion(const WaterTerrainInfoInputs& inputs, const WaterComponent& water,
    const Transform& transform, const WaterTerrainBakeSettings& settings, const AtlasMapping& mapping,
    const TerrainHeightRegion& region, WaterTerrainInfoAtlas& atlas, WaterSurfaceReadView view)
{
    if (region.Empty()) return true;
    const auto world = transform.GetWorldMatrix();
    const bool usesGeometry = UsesWaterSurfaceGeometry(water, view);
    const WaterSplineSnapshot* geometry = WaterSurfaceGeometry(water, view);
    const auto worldAt = [&](int x, int z) {
        return world * math::Vector4{
            mapping.center.x + ((static_cast<float>(x) + 0.5f) / atlas.width - 0.5f) * mapping.extent.x, 0.0f,
            mapping.center.y + ((static_cast<float>(z) + 0.5f) / atlas.height - 0.5f) * mapping.extent.y, 1.0f};
    };
    const bool validSources = std::all_of(inputs.terrains.begin(), inputs.terrains.end(), ValidWaterTerrainSource);
    using SourceList = std::vector<const WaterTerrainSource*>;
    const auto bake = [&](const TerrainHeightRegion& area, std::span<const WaterTerrainSource* const> sources) {
        for (int z = area.minZ; z <= area.maxZ; ++z) for (int x = area.minX; x <= area.maxX; ++x) {
            const auto point = worldAt(x, z);
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) return false;
            float meanWaterY = point.y;
            WaterSplineSample spline;
            if (usesGeometry) {
                if (!geometry || !TrySampleWaterSpline(*geometry, point.x, point.z, spline)) continue;
                meanWaterY = spline.baseHeight;
            }
            WaterTerrainSample sample;
            if (!validSources || !std::isfinite(meanWaterY)) return false;
            for (const auto* source : sources) ConsiderWaterTerrainBottom(*source, {point.x, point.z}, sample);
            SetWaterTerrainDepth(sample, meanWaterY, atlas.depthRange, settings.foamThreshold, settings.foamFade);
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
        return true;
    };
    if (!validSources || inputs.terrains.empty()) return bake(region, {});
    if (inputs.terrains.size() == 1u) {
        const std::array source{inputs.terrains.data()};
        return bake(region, source);
    }

    SourceList sources;
    sources.reserve(inputs.terrains.size());
    for (const auto& source : inputs.terrains) sources.push_back(&source);
    constexpr int TILE_SIZE = 16;
    constexpr size_t MAX_TILE_DEPTH = std::bit_width(MAX_HORIZON_WATER_TERRAIN_RESOLUTION);
    std::array<SourceList, MAX_TILE_DEPTH> candidatesAtDepth;
    const auto visit = [&](const auto& self, const TerrainHeightRegion& area,
                           std::span<const WaterTerrainSource* const> parent, size_t depth) -> bool {
        const auto topLeft = worldAt(area.minX, area.minZ);
        const auto topRight = worldAt(area.maxX, area.minZ);
        const auto bottomLeft = worldAt(area.minX, area.maxZ);
        const auto bottomRight = worldAt(area.maxX, area.maxZ);
        for (const auto& point : {topLeft, topRight, bottomLeft, bottomRight})
            if (!std::isfinite(point.x) || !std::isfinite(point.z)) return bake(area, parent);

        /// @note The canonical texel mapping is monotonic per world component in each atlas axis, including float rounding.
        const float minX = (std::min)({topLeft.x, topRight.x, bottomLeft.x, bottomRight.x});
        const float maxX = (std::max)({topLeft.x, topRight.x, bottomLeft.x, bottomRight.x});
        const float minZ = (std::min)({topLeft.z, topRight.z, bottomLeft.z, bottomRight.z});
        const float maxZ = (std::max)({topLeft.z, topRight.z, bottomLeft.z, bottomRight.z});
        auto& candidates = candidatesAtDepth[depth];
        candidates.clear();
        bool covered = true;
        for (const auto* source : parent) {
            if (minX <= source->boundsMax.x && maxX >= source->boundsMin.x
                && minZ <= source->boundsMax.z && maxZ >= source->boundsMin.z) {
                candidates.push_back(source);
                covered &= minX >= source->boundsMin.x && maxX <= source->boundsMax.x
                    && minZ >= source->boundsMin.z && maxZ <= source->boundsMax.z;
            }
        }
        const bool splitX = area.maxX - area.minX >= TILE_SIZE;
        const bool splitZ = area.maxZ - area.minZ >= TILE_SIZE;
        if (covered || candidates.size() <= 1u || (!splitX && !splitZ)) return bake(area, candidates);
        const int midX = splitX ? (area.minX + area.maxX) / 2 : area.maxX;
        const int midZ = splitZ ? (area.minZ + area.maxZ) / 2 : area.maxZ;
        return self(self, {area.minX, area.minZ, midX, midZ}, candidates, depth + 1u)
            && (!splitX || self(self, {midX + 1, area.minZ, area.maxX, midZ}, candidates, depth + 1u))
            && (!splitZ || self(self, {area.minX, midZ + 1, midX, area.maxZ}, candidates, depth + 1u))
            && (!(splitX && splitZ) || self(self, {midX + 1, midZ + 1, area.maxX, area.maxZ}, candidates, depth + 1u));
    };
    return visit(visit, region, sources, 0u);
}

TerrainHeightRegion FullAtlasRegion(const WaterTerrainInfoAtlas& atlas)
{
    return {0, 0, static_cast<int>(atlas.width) - 1, static_cast<int>(atlas.height) - 1};
}

uint64_t SourceLayout(const WaterTerrainSource& source)
{
    uint64_t layout = HASH_BASIS;
    Mix(layout, source.lifetime);
    MixTerrainLayout(layout, *source.terrain);
    MixTransform(layout, source.transform);
    MixFloat(layout, source.boundsMin.x); MixFloat(layout, source.boundsMin.z);
    MixFloat(layout, source.boundsMax.x); MixFloat(layout, source.boundsMax.z);
    return layout;
}

/// @note A changed vertex contributes to height triangles and the interpolated normals of neighboring cells.
bool TryAtlasEditRegion(const WaterTerrainSource& source, const Transform& waterTransform, const AtlasMapping& mapping,
    const WaterTerrainInfoAtlas& atlas, TerrainHeightRegion& out)
{
    const auto& terrain = *source.terrain;
    const auto& edit = terrain.heightPreview.region;
    const int minX = (std::max)(0, edit.minX - 2), minZ = (std::max)(0, edit.minZ - 2);
    const int maxX = (std::min)(terrain.columns - 1, edit.maxX + 2);
    const int maxZ = (std::min)(terrain.rows - 1, edit.maxZ + 2);
    const auto& pose = source.transform;
    const auto terrainInverse = pose.worldRotation.Inverse();
    const auto terrainRight = terrainInverse * math::Vector3{1.0f, 0.0f, 0.0f};
    const auto terrainForward = terrainInverse * math::Vector3{0.0f, 0.0f, 1.0f};
    const double terrainDeterminant = static_cast<double>(terrainRight.x) * terrainForward.z
        - static_cast<double>(terrainForward.x) * terrainRight.z;
    if (!std::isfinite(terrainDeterminant) || terrainDeterminant == 0.0) return false;
    const auto waterWorld = waterTransform.GetWorldMatrix();
    const double a = waterWorld.m[0][0], b = waterWorld.m[0][2];
    const double c = waterWorld.m[2][0], d = waterWorld.m[2][2];
    const double determinant = a * d - b * c;
    if (!std::isfinite(determinant) || determinant == 0.0) return false;
    const auto coordinateScale = (std::max)({std::abs(source.boundsMin.x), std::abs(source.boundsMin.z),
        std::abs(source.boundsMax.x), std::abs(source.boundsMax.z), std::abs(waterTransform.worldPosition.x),
        std::abs(waterTransform.worldPosition.z), 1.0f});
    const double worldMargin = coordinateScale * 32.0 * std::numeric_limits<float>::epsilon();
    const double texelWorldX = std::abs(static_cast<double>(mapping.extent.x) * waterTransform.worldScale.x) / atlas.width;
    const double texelWorldZ = std::abs(static_cast<double>(mapping.extent.y) * waterTransform.worldScale.z) / atlas.height;
    if (worldMargin >= (std::min)(texelWorldX, texelWorldZ)) return false;
    double lowX = std::numeric_limits<double>::infinity(), lowZ = lowX;
    double highX = -lowX, highZ = -lowX;
    for (const int x : {minX, maxX}) for (const int z : {minZ, maxZ}) {
        /// @note Invert the same XZ mapping that the vertical heightfield query uses to find its source cell.
        const double terrainX = static_cast<double>(x) * terrain.cellSize * pose.worldScale.x;
        const double terrainZ = static_cast<double>(z) * terrain.cellSize * pose.worldScale.z;
        const double worldX = (terrainX * terrainForward.z - terrainForward.x * terrainZ) / terrainDeterminant + pose.worldPosition.x;
        const double worldZ = (terrainRight.x * terrainZ - terrainX * terrainRight.z) / terrainDeterminant + pose.worldPosition.z;
        const double dx = worldX - waterWorld.m[0][3];
        const double dz = worldZ - waterWorld.m[2][3];
        const double localX = (dx * d - b * dz) / determinant;
        const double localZ = (a * dz - dx * c) / determinant;
        const double u = (localX - mapping.center.x) / mapping.extent.x;
        const double v = (localZ - mapping.center.y) / mapping.extent.y;
        if (!std::isfinite(u) || !std::isfinite(v)) return false;
        lowX = (std::min)(lowX, (u + 0.5) * atlas.width);
        highX = (std::max)(highX, (u + 0.5) * atlas.width);
        lowZ = (std::min)(lowZ, (v + 0.5) * atlas.height);
        highZ = (std::max)(highZ, (v + 0.5) * atlas.height);
    }
    /// @note One extra texel absorbs roundoff differences between forward and inverse world mappings.
    const auto low = [](double value, uint32_t limit) {
        return static_cast<int>((std::clamp)(std::floor(value) - 1.0, 0.0, static_cast<double>(limit)));
    };
    const auto high = [](double value, uint32_t limit) {
        return static_cast<int>((std::clamp)(std::ceil(value) + 1.0, -1.0, static_cast<double>(limit - 1u)));
    };
    out.Include({low(lowX, atlas.width), low(lowZ, atlas.height),
        high(highX, atlas.width), high(highZ, atlas.height)});
    return true;
}
} /// @note namespace

bool TryComputeWaterTerrainInfoSignature(const WaterTerrainInfoInputs& inputs, const WaterComponent& water,
    const Transform& transform, const WaterTerrainBakeSettings& settings, uint64_t& out,
    WaterSurfaceReadView view)
{
    return TryWaterTerrainSignature(inputs.signature, water, transform, settings, out, view);
}

bool TryBakeWaterTerrainInfo(const WaterTerrainInfoInputs& inputs, const WaterComponent& water,
    const Transform& transform, const WaterTerrainBakeSettings& settings, WaterTerrainInfoAtlas& out,
    WaterSurfaceReadView view)
{
    WaterTerrainInfoAtlas atlas;
    AtlasMapping mapping;
    if (!PrepareWaterTerrainAtlas(inputs, water, transform, settings, atlas, mapping, view)) return false;
    atlas.rgba.resize(static_cast<size_t>(atlas.width) * atlas.height * 4u);
    if (!BakeWaterTerrainAtlasRegion(inputs, water, transform, settings, mapping, FullAtlasRegion(atlas), atlas, view))
        return false;
    out = std::move(atlas);
    return true;
}

bool WaterTerrainInfoAtlasCache::TryBake(const WaterTerrainInfoInputs& inputs, const WaterComponent& water,
    const Transform& transform, const WaterTerrainBakeSettings& settings, WaterTerrainInfoAtlas& out,
    WaterSurfaceReadView view)
{
    std::shared_ptr<const WaterTerrainInfoAtlas> snapshot;
    if (!TryBakeSnapshot(inputs, water, transform, settings, snapshot, view)) return false;
    out = *snapshot;
    return true;
}

bool WaterTerrainInfoAtlasCache::TryBakeSnapshot(const WaterTerrainInfoInputs& inputs, const WaterComponent& water,
    const Transform& transform, const WaterTerrainBakeSettings& settings,
    std::shared_ptr<const WaterTerrainInfoAtlas>& out, WaterSurfaceReadView view)
{
    WaterTerrainInfoAtlas atlas;
    AtlasMapping mapping;
    if (!PrepareWaterTerrainAtlas(inputs, water, transform, settings, atlas, mapping, view)) return false;
    uint64_t layout = 0;
    if (!TryWaterTerrainSignature(HASH_BASIS, water, transform, settings, layout, view)) return false;
    for (const float coordinate : {mapping.center.x, mapping.center.y, mapping.extent.x, mapping.extent.y})
        MixFloat(layout, coordinate);

    std::vector<SourceVersion> sources;
    sources.reserve(inputs.terrains.size());
    bool local = m_atlas && m_layout == layout && m_sources.size() == inputs.terrains.size()
        && SupportsLocalAtlasProjection(transform);
    TerrainHeightRegion changed;
    for (size_t i = 0; i < inputs.terrains.size(); ++i) {
        const auto& source = inputs.terrains[i];
        if (!source.terrain || !Finite(source.boundsMin) || !Finite(source.boundsMax)) return false;
        const auto& terrain = *source.terrain;
        const auto& current = sources.emplace_back(SourceVersion{
            SourceLayout(source), ContentSignature(terrain), terrain.surfaceRevision});
        if (!local) continue;
        const auto& previous = m_sources[i];
        if (source.lifetime == 0 || current.layout != previous.layout) {
            local = false;
        } else if (current.signature != previous.signature) {
            local = SupportsLocalAtlasProjection(source.transform) && IsContiguousHeightPreview(terrain, previous.revision)
                && TryAtlasEditRegion(source, transform, mapping, atlas, changed);
        }
    }
    if (local) {
        atlas.rgba = m_atlas->rgba;
    } else {
        atlas.rgba.resize(static_cast<size_t>(atlas.width) * atlas.height * 4u);
        changed = FullAtlasRegion(atlas);
    }
    if (!BakeWaterTerrainAtlasRegion(inputs, water, transform, settings, mapping, changed, atlas, view)) return false;
    auto snapshot = std::make_shared<const WaterTerrainInfoAtlas>(std::move(atlas));
    m_layout = layout;
    m_sources = std::move(sources);
    m_atlas = snapshot;
    out = std::move(snapshot);
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
