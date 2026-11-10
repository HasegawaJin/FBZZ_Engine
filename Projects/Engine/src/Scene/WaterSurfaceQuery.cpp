/// @file    WaterSurfaceQuery.cpp
/// @brief   水域の有限境界と解析波をワールド座標で共通評価する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Engine/Scene/WaterExclusion.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace fbzz::scene {
namespace {
bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Finite(const math::Vector2& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool ValidBakedShape(const math::Vector3& extents, const math::Quaternion& inverseRotation,
    const fluid::VectorFieldAsset& asset)
{
    const double rotationLength = inverseRotation.Length();
    return Finite(extents) && extents.x > 0.0f && extents.y > 0.0f && extents.z > 0.0f
        && std::isfinite(rotationLength) && std::abs(rotationLength - 1.0) <= 0.001
        && Finite(asset.boundsMin) && Finite(asset.boundsMax) && Finite(asset.boundsMax - asset.boundsMin)
        && std::isfinite(asset.maxMagnitude);
}

bool ValidSurfaceFlow(const WaterSurfaceFlow& flow)
{
    if (!std::isfinite(flow.height)) return false;
    if (flow.kind == FlowFieldType::Baked) {
        if (!flow.vectorField || flow.vectorField->Empty()) return true;
        return Finite(flow.center) && std::isfinite(flow.planeOffsetY) && std::isfinite(flow.speed)
            && ValidBakedShape(flow.extents, flow.inverseRotation, *flow.vectorField);
    }
    if (!std::isfinite(flow.radius)) return false;
    if (flow.radius <= 0.0001f) return true;
    return Finite(flow.center) && std::isfinite(flow.speed) && std::isfinite(flow.falloffPower)
        && std::isfinite(flow.chop) && (flow.kind != FlowFieldType::Uniform || Finite(flow.direction));
}

bool ValidFlowField(const ActiveFlowField& field)
{
    if (field.channels == 0 || field.strength == 0.0f || field.type == FlowFieldType::LegacyDrag) return true;
    if (field.type == FlowFieldType::Baked && (!field.vectorField || field.vectorField->Empty())) return true;
    if (!Finite(field.position) || !std::isfinite(field.strength) || !std::isfinite(field.radius)
        || (field.radius > 0.0f && !std::isfinite(field.falloffPower))) return false;
    switch (field.type) {
    case FlowFieldType::Uniform:
    case FlowFieldType::Vortex:
        return Finite(field.direction);
    case FlowFieldType::Curl:
        return std::isfinite(field.noiseFrequency) && std::isfinite(field.noiseSpeed);
    case FlowFieldType::Baked:
        return ValidBakedShape(field.extents, field.inverseRotation, *field.vectorField);
    default:
        return true;
    }
}

bool TryFootprint(const WaterComponent& water, const Transform& transform,
    float worldX, float worldZ, float time, bool& inside, WaterSurfaceReadView view)
{
    const Transform& pose = WaterSurfaceTransform(water, transform, view);
    if (!std::isfinite(worldX) || !std::isfinite(worldZ) || !std::isfinite(time)
        || !IsWaterSurfaceSupported(water, pose, view)) return false;
    const auto inverse = pose.worldRotation.Inverse();
    const auto localAt = [&](float x, float z) {
        const auto rotated = inverse * (math::Vector3{x, pose.worldPosition.y, z} - pose.worldPosition);
        return math::Vector2{rotated.x / pose.worldScale.x, rotated.z / pose.worldScale.z};
    };
    const auto displayedLocal = localAt(worldX, worldZ);
    if (!std::isfinite(displayedLocal.x) || !std::isfinite(displayedLocal.y)) return false;
    if (WaterSurfaceUsesHorizon(water, view)) {
        inside = true;
        return true;
    }
    if (UsesWaterSurfaceGeometry(water, view)) {
        const WaterSplineSnapshot* geometry = WaterSurfaceGeometry(water, view);
        if (!geometry) return false;
        const WaterSplineSnapshot& snapshot = *geometry;
        const float boundsScale = (std::max)({std::abs(snapshot.worldBoundsMin.x),
            std::abs(snapshot.worldBoundsMax.x), std::abs(snapshot.worldBoundsMin.z),
            std::abs(snapshot.worldBoundsMax.z), 1.0f});
        const float tolerance = (std::max)(1.0e-5f,
            boundsScale * 8.0f * std::numeric_limits<float>::epsilon());
        const float margin = water.SurfaceHeightBound(view);
        if (worldX < snapshot.worldBoundsMin.x - margin - tolerance
            || worldX > snapshot.worldBoundsMax.x + margin + tolerance
            || worldZ < snapshot.worldBoundsMin.z - margin - tolerance
            || worldZ > snapshot.worldBoundsMax.z + margin + tolerance) {
            inside = false;
            return true;
        }
        const math::Vector2 origin = water.SolveUndisplacedXZ(worldX, worldZ, time, view);
        if (!std::isfinite(origin.x) || !std::isfinite(origin.y)) return false;
        WaterSplineSample splineSample;
        inside = TrySampleWaterSpline(snapshot, origin.x, origin.y, splineSample);
        return true;
    }
    const float halfX = water.extentX * 0.5f, halfZ = water.extentZ * 0.5f;
    const float toleranceX = 8.0f * std::numeric_limits<float>::epsilon() * (std::max)(halfX, 1.0f);
    const float toleranceZ = 8.0f * std::numeric_limits<float>::epsilon() * (std::max)(halfZ, 1.0f);
    /// @note Q<=1 の水平変位は波高上限以内。境界からその距離以上離れた点は逆写像の三角関数を省ける。
    const float bound = water.SurfaceHeightBound(view);
    const float marginX = bound / std::abs(pose.worldScale.x);
    const float marginZ = bound / std::abs(pose.worldScale.z);
    if (std::abs(displayedLocal.x) <= halfX - marginX && std::abs(displayedLocal.y) <= halfZ - marginZ) {
        inside = true;
        return true;
    }
    if (std::abs(displayedLocal.x) > halfX + marginX + toleranceX
        || std::abs(displayedLocal.y) > halfZ + marginZ + toleranceZ) {
        inside = false;
        return true;
    }
    const math::Vector2 origin = water.SolveUndisplacedXZ(worldX, worldZ, time, view);
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y)) return false;
    const auto local = localAt(origin.x, origin.y);
    if (!std::isfinite(local.x) || !std::isfinite(local.y)) return false;
    inside = std::abs(local.x) <= halfX + toleranceX && std::abs(local.y) <= halfZ + toleranceZ;
    return true;
}
} /// @note namespace

bool WaterSurfaceUsesHorizon(const WaterComponent& water, WaterSurfaceReadView view)
{
    return water.farOceanEnabled && WaterSurfaceEffectiveShape(water, view) == WaterSurfaceShape::RECTANGLE
        && !UsesWaterSurfaceGeometry(water, view);
}

bool IsWaterSurfaceSupported(const WaterComponent& water, const Transform& transform,
    WaterSurfaceReadView view)
{
    const Transform& pose = WaterSurfaceTransform(water, transform, view);
    const auto scale = pose.worldScale;
    const double rotationLength = pose.worldRotation.Length();
    if (!Finite(pose.worldPosition) || !Finite(scale)
        || std::abs(scale.x) < 0.0001f || std::abs(scale.y) < 0.0001f || std::abs(scale.z) < 0.0001f
        || !std::isfinite(rotationLength) || std::abs(rotationLength - 1.0) > 0.001
        || !std::isfinite(water.buoyancyDepth) || water.buoyancyDepth < 0.0f) return false;
    const bool usesGeometry = UsesWaterSurfaceGeometry(water, view);
    const WaterSplineSnapshot* geometry = WaterSurfaceGeometry(water, view);
    if (!usesGeometry && WaterSurfaceEffectiveShape(water, view) == WaterSurfaceShape::RECTANGLE) {
        if (!std::isfinite(water.extentX) || water.extentX <= 0.0f
            || !std::isfinite(water.extentZ) || water.extentZ <= 0.0f
            || !std::isfinite(water.extentX * scale.x) || !std::isfinite(water.extentZ * scale.z)) return false;
    } else if (usesGeometry) {
        if (!geometry || geometry->vertices.empty() || geometry->triangles.empty()
            || geometry->bvhNodes.empty()
            || !Finite(geometry->worldBoundsMin) || !Finite(geometry->worldBoundsMax)
            || !std::isfinite(geometry->maximumDepth) || geometry->maximumDepth < 0.0f
            || !std::isfinite(geometry->minimumBaseHeightOffset)
            || !std::isfinite(geometry->maximumBaseHeightOffset)
            || !HasValidWaterSplineExtents(*geometry, pose)) return false;
    } else {
        return false;
    }
    if (!Finite(water.current) || (water.buoyancyEnabled && (!std::isfinite(water.buoyancy) || !std::isfinite(water.waterDrag)))) return false;
    const math::Vector3 up = pose.worldRotation * math::Vector3::UP;
    if (!Finite(up) || std::abs(up.x) > 0.00001f || std::abs(up.z) > 0.00001f
        || std::abs(std::abs(up.y) - 1.0f) > 0.00001f) return false;
    if (!std::isfinite(water.waveGrouping) || water.waveGrouping < 0.0f || water.waveGrouping > 1.0f
        || !std::isfinite(water.waveSpread) || water.waveSpread < 0.0f || water.waveSpread > 1.0f
        || water.waveCount > water.waves.size()) return false;
    for (size_t i = 0; i < water.waveCount; ++i) {
        const auto& wave = water.waves[i];
        if (!std::isfinite(wave.direction.x) || !std::isfinite(wave.direction.y)
            || !std::isfinite(wave.amplitude) || !std::isfinite(wave.wavelength)
            || !std::isfinite(wave.steepness) || !std::isfinite(wave.phaseOffsetRadians)) return false;
        if (water.enableGerstnerWaves && wave.amplitude > 0.0001f && wave.wavelength > 0.0001f) {
            const float directionLength = wave.direction.Length();
            if (!std::isfinite(directionLength) || directionLength <= 0.0001f) return false;
        }
    }
    const int flowCount = (std::clamp)(water.surfaceFlowCount, 0, kWaterSurfaceFlowCount);
    for (int i = 0; i < flowCount; ++i)
        if (!ValidSurfaceFlow(water.surfaceFlows[static_cast<size_t>(i)])) return false;
    for (const auto& ripple : water.ripples) {
        if (!std::isfinite(ripple.amplitude)) return false;
        if (ripple.amplitude <= 0.0f) continue;
        if (!Finite(ripple.center) || !std::isfinite(ripple.radius) || !std::isfinite(ripple.width)
            || !std::isfinite(ripple.meshFade) || !std::isfinite(ripple.speed) || !std::isfinite(ripple.decayRate)
            || !std::isfinite(ripple.maxRadius)) return false;
    }
    return std::isfinite(water.SurfaceHeightBound(view));
}

bool WaterSurfaceContainsXZ(const WaterComponent& water, const Transform& transform,
    float worldX, float worldZ, float time, WaterSurfaceReadView view)
{
    bool inside = false;
    return TryFootprint(water, transform, worldX, worldZ, time, inside, view) && inside;
}

bool TrySampleWaterSurfaceHeightWorld(const WaterComponent& water, const Transform& transform,
    const math::Vector3& worldPosition, float time, WaterSurfaceHeightSample& out,
    WaterSurfaceReadView view)
{
    if (!Finite(worldPosition)) return false;
    const Transform& pose = WaterSurfaceTransform(water, transform, view);
    WaterSurfaceHeightSample sample;
    const WaterExclusionSnapshot* exclusions = WaterSurfaceExclusions(water, view);
    if (!TryFootprint(water, pose, worldPosition.x, worldPosition.z, time,
            sample.insideSurface, view)) return false;
    sample.waterDepthLimit = water.buoyancyDepth;
    if (UsesWaterSurfaceGeometry(water, view)) {
        const WaterSplineSnapshot* geometry = WaterSurfaceGeometry(water, view);
        if (!geometry) return false;
        const math::Vector2 origin = water.SolveUndisplacedXZ(worldPosition.x, worldPosition.z, time, view);
        if (!std::isfinite(origin.x) || !std::isfinite(origin.y)) return false;
        WaterSplineSample splineSample;
        if (TrySampleWaterSpline(*geometry, origin.x, origin.y, splineSample)) {
            sample.baseHeightOffset = splineSample.baseHeightOffset;
            sample.baseHeightGradient = splineSample.baseHeightGradient;
            sample.waterDepthLimit = splineSample.depth;
        }
    }
    sample.displacement = water.GetSurfaceHeightAt(worldPosition.x, worldPosition.z, time, view);
    if (WaterSurfaceUsesHorizon(water, view)) {
        const WaterDepthField* field = water.EffectiveDepthField(view);
        float terrainDepth = 0.0f;
        if (field && TrySampleWaterTerrainColumn(*field, {worldPosition.x, worldPosition.z}, terrainDepth))
            sample.waterDepthLimit = (std::clamp)(terrainDepth + sample.displacement, 0.0f, sample.waterDepthLimit);
    }
    const double surfaceY = static_cast<double>(pose.worldPosition.y) + sample.displacement;
    sample.position = {worldPosition.x, static_cast<float>(surfaceY), worldPosition.z};
    sample.depth = static_cast<float>(surfaceY - static_cast<double>(worldPosition.y));
    if (!Finite(sample.position) || !std::isfinite(sample.depth) || !std::isfinite(sample.displacement)
        || !std::isfinite(sample.waterDepthLimit) || sample.waterDepthLimit < 0.0f
        || !std::isfinite(sample.baseHeightOffset) || !Finite(sample.baseHeightGradient)) return false;
    sample.excluded = IsWaterPointExcluded(exclusions, worldPosition);
    sample.underwater = sample.insideSurface && !sample.excluded && sample.depth > 0.0f
        && sample.depth <= sample.waterDepthLimit;
    out = sample;
    return true;
}

bool TrySampleWaterSurfaceWorld(const WaterComponent& water, const Transform& transform,
    const std::vector<ActiveFlowField>& fields, const math::Vector3& worldPosition, float time,
    WaterSurfaceSample& out, WaterSurfaceReadView view)
{
    WaterSurfaceSample sample;
    const Transform& pose = WaterSurfaceTransform(water, transform, view);
    if (!TrySampleWaterSurfaceHeightWorld(water, pose, worldPosition, time, sample.height, view)) return false;
    for (const auto& field : fields)
        if (!ValidFlowField(field)) return false;
    float step = 0.05f;
    for (size_t i = 0; i < water.waveCount; ++i) {
        const auto& wave = water.waves[i];
        if (wave.amplitude > 0.0f && wave.wavelength > 0.0f && std::isfinite(wave.wavelength))
            step = (std::min)(step, wave.wavelength * 0.01f);
    }
    step = (std::max)(step, 0.0001f);
    const math::Vector2 centerOrigin = water.SolveUndisplacedXZ(worldPosition.x, worldPosition.z, time, view);
    if (!std::isfinite(centerOrigin.x) || !std::isfinite(centerOrigin.y)) return false;
    const bool usesGeometry = UsesWaterSurfaceGeometry(water, view);
    const WaterSplineSnapshot* geometry = WaterSurfaceGeometry(water, view);
    if (usesGeometry && !geometry) return false;
    const auto derivative = [&](float coordinate, bool alongX, float& result) {
        float lower = coordinate - step, upper = coordinate + step;
        if (lower == coordinate) lower = std::nextafter(coordinate, -std::numeric_limits<float>::infinity());
        if (upper == coordinate) upper = std::nextafter(coordinate, std::numeric_limits<float>::infinity());
        if (!std::isfinite(lower) || !std::isfinite(upper) || lower >= upper) return false;
        const float lowX = alongX ? lower : worldPosition.x;
        const float lowZ = alongX ? worldPosition.z : lower;
        const float highX = alongX ? upper : worldPosition.x;
        const float highZ = alongX ? worldPosition.z : upper;
        float lowHeight = water.GetSurfaceHeightAt(lowX, lowZ, time, view);
        float highHeight = water.GetSurfaceHeightAt(highX, highZ, time, view);
        if (usesGeometry) {
            const math::Vector2 lowOrigin = water.SolveUndisplacedXZ(lowX, lowZ, time, view);
            const math::Vector2 highOrigin = water.SolveUndisplacedXZ(highX, highZ, time, view);
            if (!std::isfinite(lowOrigin.x) || !std::isfinite(lowOrigin.y)
                || !std::isfinite(highOrigin.x) || !std::isfinite(highOrigin.y)) return false;
            WaterSplineSample lowSpline;
            WaterSplineSample highSpline;
            const bool hasLowSpline = TrySampleWaterSpline(*geometry,
                lowOrigin.x, lowOrigin.y, lowSpline);
            const bool hasHighSpline = TrySampleWaterSpline(*geometry,
                highOrigin.x, highOrigin.y, highSpline);
            const float expectedLowBase = hasLowSpline ? lowSpline.baseHeightOffset
                : sample.height.baseHeightOffset
                    + sample.height.baseHeightGradient.x * (lowOrigin.x - centerOrigin.x)
                    + sample.height.baseHeightGradient.y * (lowOrigin.y - centerOrigin.y);
            const float expectedHighBase = hasHighSpline ? highSpline.baseHeightOffset
                : sample.height.baseHeightOffset
                    + sample.height.baseHeightGradient.x * (highOrigin.x - centerOrigin.x)
                    + sample.height.baseHeightGradient.y * (highOrigin.y - centerOrigin.y);
            lowHeight += expectedLowBase - (hasLowSpline ? lowSpline.baseHeightOffset : 0.0f);
            highHeight += expectedHighBase - (hasHighSpline ? highSpline.baseHeightOffset : 0.0f);
        }
        if (!std::isfinite(lowHeight) || !std::isfinite(highHeight)) return false;
        /// @note world 座標の ULP が指定幅を超えても標本を分け、加算前変位を実際の world 距離で割る。
        result = static_cast<float>((static_cast<double>(highHeight) - lowHeight)
            / (static_cast<double>(upper) - lower));
        return std::isfinite(result);
    };
    float dx = 0.0f, dz = 0.0f;
    if (!derivative(worldPosition.x, true, dx) || !derivative(worldPosition.z, false, dz)) return false;
    /// @note 高さ場 H(worldX,worldZ) の上向き法線は normalize(-dH/dX,1,-dH/dZ)。逆写像後の高さを差分し水平変位も含める。
    /// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models GPU Gems 1.2.2, Differentials of Height Functions。
    if (!std::isfinite(dx) || !std::isfinite(dz)) return false;
    const double scale = (std::max)({std::abs(static_cast<double>(dx)), 1.0, std::abs(static_cast<double>(dz))});
    const double nx = -dx / scale, ny = 1.0 / scale, nz = -dz / scale;
    const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
    sample.normal = {static_cast<float>(nx / length), static_cast<float>(ny / length), static_cast<float>(nz / length)};
    sample.flowVelocity = WaterFlowVelocityAt(water, fields, pose.worldPosition.y, worldPosition, time, view);
    if (!Finite(sample.normal) || !Finite(sample.flowVelocity)) return false;
    out = sample;
    return true;
}
} /// @note namespace fbzz::scene
