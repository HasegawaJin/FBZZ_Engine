/// @file    ControlPointAdapters.cpp
/// @brief   Water と Spline の制御点入力を不変な owner-local 数値列へ解決する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <Engine/Scene/ControlPointAdapters.hpp>
#include <Engine/Scene/NavigationControlPoints.hpp>
#include <Engine/Scene/PresentationControlPoints.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Asset/ClothAsset.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Core/Logger.hpp>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_set>

namespace fbzz::scene {
namespace {

constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ull;
constexpr std::uint64_t FNV_PRIME = 1099511628211ull;
constexpr float LAKE_PLANE_TOLERANCE = 1.0e-4f;

void HashByte(std::uint64_t& hash, std::uint8_t value)
{
    hash = (hash ^ value) * FNV_PRIME;
}

template<typename T>
void HashValue(std::uint64_t& hash, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    for (const std::byte valueByte : std::as_bytes(std::span{&value, std::size_t{1}}))
        HashByte(hash, std::to_integer<std::uint8_t>(valueByte));
}

void HashFloat(std::uint64_t& hash, float value)
{
    HashValue(hash, std::bit_cast<std::uint32_t>(value));
}

void HashVector(std::uint64_t& hash, const math::Vector3& value)
{
    HashFloat(hash, value.x);
    HashFloat(hash, value.y);
    HashFloat(hash, value.z);
}

void HashMode(std::uint64_t& hash, ControlPointSourceMode sourceMode,
    ControlPointRuntimeMode runtimeMode)
{
    HashValue(hash, sourceMode);
    HashValue(hash, runtimeMode);
    HashValue(hash, CONTROL_POINT_ADAPTER_VERSION);
}

bool IsKnownSourceMode(ControlPointSourceMode mode)
{
    return mode == ControlPointSourceMode::INLINE || mode == ControlPointSourceMode::POINT_OBJECTS;
}

bool IsKnownRuntimeMode(ControlPointRuntimeMode mode)
{
    return mode == ControlPointRuntimeMode::LIVE || mode == ControlPointRuntimeMode::BAKED_STATIC;
}

bool IsFinite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

std::uint64_t WaterRawSignature(const WaterComponent& water,
    ControlPointRuntimeMode signatureRuntimeMode)
{
    std::uint64_t hash = FNV_OFFSET;
    HashMode(hash, water.pointSourceMode, signatureRuntimeMode);
    HashValue(hash, water.surfaceShape);
    HashFloat(hash, water.lakePlaneHeight);
    if (water.pointSourceMode == ControlPointSourceMode::INLINE) {
        HashValue(hash, static_cast<std::uint64_t>(water.splinePoints.size()));
        for (const WaterSplinePoint& point : water.splinePoints) {
            HashValue(hash, point.id);
            HashVector(hash, point.localPosition);
            HashFloat(hash, point.width);
            HashFloat(hash, point.depth);
            HashFloat(hash, point.flowSpeed);
        }
    } else {
        HashValue(hash, static_cast<std::uint64_t>(water.pointBindings.size()));
        for (const WaterControlPointBinding& binding : water.pointBindings) {
            HashValue(hash, binding.id);
            HashFloat(hash, binding.width);
            HashFloat(hash, binding.depth);
            HashFloat(hash, binding.flowSpeed);
        }
    }
    return hash == 0 ? 1 : hash;
}

std::uint64_t SplineRawSignature(const SplineComponent& spline,
    ControlPointRuntimeMode signatureRuntimeMode)
{
    std::uint64_t hash = FNV_OFFSET;
    HashMode(hash, spline.pointSourceMode, signatureRuntimeMode);
    HashValue(hash, spline.closed);
    if (spline.pointSourceMode == ControlPointSourceMode::INLINE) {
        HashValue(hash, static_cast<std::uint64_t>(spline.points.size()));
        for (std::size_t index = 0; index < spline.points.size(); ++index) {
            HashValue(hash, static_cast<std::uint32_t>(index + 1));
            HashVector(hash, spline.points[index]);
        }
    } else {
        HashValue(hash, static_cast<std::uint64_t>(spline.pointBindings.size()));
        for (const SplineControlPointBinding& binding : spline.pointBindings) {
            HashValue(hash, binding.id);
        }
    }
    return hash == 0 ? 1 : hash;
}

template<typename T, typename ReflectRow>
void ReflectRows(IReflector& reflector, const char* fieldName,
    std::vector<T>& values, std::size_t maximum, ReflectRow reflectRow)
{
    const std::size_t requested = reflector.BeginObjectList(fieldName, values.size());
    if (requested != values.size()) values.resize((std::min)(requested, maximum + 1));
    for (std::size_t index = 0; index < values.size(); ++index) {
        reflector.BeginObjectElement(index);
        reflectRow(values[index]);
        reflector.EndObjectElement();
    }
    const std::size_t removed = reflector.EndObjectList();
    if (removed < values.size()) values.erase(values.begin() + static_cast<std::ptrdiff_t>(removed));
    std::size_t from = 0;
    std::size_t to = 0;
    if (reflector.ObjectListMove(from, to) && from < values.size() && to < values.size() && from != to) {
        T moved = std::move(values[from]);
        values.erase(values.begin() + static_cast<std::ptrdiff_t>(from));
        values.insert(values.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    }
}

void ReflectSignature(IReflector& reflector, const char* fieldName, std::uint64_t& signature)
{
    std::string text = std::to_string(signature);
    reflector.Field(fieldName, text);
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec == std::errc{} && result.ptr == text.data() + text.size()) signature = parsed;
}

void ReflectVersion(IReflector& reflector, const char* fieldName, std::uint32_t& version)
{
    int value = static_cast<int>((std::min)(version,
        static_cast<std::uint32_t>((std::numeric_limits<int>::max)())));
    reflector.Field(fieldName, value);
    if (value >= 0) version = static_cast<std::uint32_t>(value);
}

void ReflectPointSourceMode(IReflector& reflector, const char* fieldName,
    ControlPointSourceMode& mode)
{
    int value = static_cast<int>(mode);
    static constexpr const char* MODES[] = { "Inline", "Point Objects" };
    reflector.Enum(fieldName, value, MODES);
    mode = static_cast<ControlPointSourceMode>(value < 0 || value > 1 ? 0xff : value);
}

void ReflectPointRuntimeMode(IReflector& reflector, const char* fieldName,
    ControlPointRuntimeMode& mode)
{
    int value = static_cast<int>(mode);
    static constexpr const char* MODES[] = { "Live", "Baked Static" };
    reflector.Enum(fieldName, value, MODES);
    mode = static_cast<ControlPointRuntimeMode>(value < 0 || value > 1 ? 0xff : value);
}

void ReflectSplineValue(IReflector& reflector, SplineControlPointValue& point)
{
    int id = static_cast<int>((std::min)(point.id, 0x7fffffffu));
    reflector.Field("id", id);
    point.id = id < 0 ? 0u : static_cast<std::uint32_t>(id);
    reflector.Field("localPosition", point.localPosition);
}

bool ValidateWaterPoints(WaterSurfaceShape shape, float lakePlaneHeight, bool requireLakePlane,
    std::span<const WaterSplinePoint> points, ControlPointAdapterStatus& status)
{
    const bool lake = shape == WaterSurfaceShape::SPLINE_LAKE;
    const bool river = shape == WaterSurfaceShape::SPLINE_RIVER;
    if (!lake && !river) {
        status = ControlPointAdapterStatus::INVALID_NUMERIC_DATA;
        return false;
    }
    if (lake && requireLakePlane && !std::isfinite(lakePlaneHeight)) {
        status = ControlPointAdapterStatus::INVALID_LAKE_PLANE;
        return false;
    }
    if (points.size() < (lake ? 3u : 2u) || points.size() > WATER_SPLINE_MAX_CONTROL_POINTS) {
        status = ControlPointAdapterStatus::INVALID_NUMERIC_DATA;
        return false;
    }
    std::unordered_set<std::uint32_t> ids;
    ids.reserve(points.size());
    for (const WaterSplinePoint& point : points) {
        if (point.id == 0 || point.id > WATER_SPLINE_MAX_POINT_ID || !ids.insert(point.id).second
            || !IsFinite(point.localPosition) || !std::isfinite(point.width)
            || !std::isfinite(point.depth) || !std::isfinite(point.flowSpeed)) {
            status = ControlPointAdapterStatus::INVALID_NUMERIC_DATA;
            return false;
        }
        if (river && (point.width <= 0.0f || point.depth < 0.0f)) {
            status = ControlPointAdapterStatus::INVALID_NUMERIC_DATA;
            return false;
        }
        if (lake && requireLakePlane
            && std::abs(point.localPosition.y - lakePlaneHeight) > LAKE_PLANE_TOLERANCE) {
            status = ControlPointAdapterStatus::INVALID_LAKE_PLANE;
            return false;
        }
    }
    return true;
}

bool ValidateSplinePoints(std::span<const SplineControlPointValue> points,
    ControlPointAdapterStatus& status)
{
    if (points.size() < 2 || points.size() > MAX_CONTROL_POINT_BINDINGS) {
        status = ControlPointAdapterStatus::INVALID_NUMERIC_DATA;
        return false;
    }
    std::unordered_set<std::uint32_t> ids;
    ids.reserve(points.size());
    for (const SplineControlPointValue& point : points) {
        if (point.id == 0 || point.id > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())
            || !ids.insert(point.id).second || !IsFinite(point.localPosition)) {
            status = ControlPointAdapterStatus::INVALID_NUMERIC_DATA;
            return false;
        }
    }
    return true;
}

std::uint64_t ComputeClothTopologySignature(const asset::ClothAsset* source,
    std::uint32_t generatedSegments, std::uint32_t& particleCount)
{
    std::uint64_t hash = FNV_OFFSET;
    if (!source) {
        if (generatedSegments < 2 || generatedSegments > 64) return 0;
        const std::uint64_t side = static_cast<std::uint64_t>(generatedSegments) + 1;
        const std::uint64_t count = side * side;
        if (count > (std::numeric_limits<std::uint32_t>::max)()) return 0;
        particleCount = static_cast<std::uint32_t>(count);
        HashByte(hash, 0u);
        HashValue(hash, generatedSegments);
        HashValue(hash, particleCount);
        for (std::uint32_t z = 0; z < generatedSegments; ++z) {
            for (std::uint32_t x = 0; x < generatedSegments; ++x) {
                const std::uint32_t a = z * (generatedSegments + 1) + x;
                const std::uint32_t b = a + 1;
                const std::uint32_t c = a + generatedSegments + 1;
                const std::uint32_t d = c + 1;
                HashValue(hash, a);
                HashValue(hash, c);
                HashValue(hash, b);
                HashValue(hash, b);
                HashValue(hash, c);
                HashValue(hash, d);
            }
        }
        return hash == 0 ? 1 : hash;
    }

    if (source->particles.empty()
        || source->particles.size() > (std::numeric_limits<std::uint32_t>::max)()) return 0;
    particleCount = static_cast<std::uint32_t>(source->particles.size());
    const std::vector<std::uint32_t>& simulationIndices = source->simulationIndices.empty()
        ? source->indices : source->simulationIndices;
    if (simulationIndices.size() % 3 != 0) return 0;
    HashByte(hash, source->simulationIndices.empty() ? 1u : 2u);
    HashValue(hash, particleCount);
    HashValue(hash, static_cast<std::uint64_t>(simulationIndices.size()));
    for (const std::uint32_t index : simulationIndices) {
        if (index >= particleCount) return 0;
        HashValue(hash, index);
    }
    HashValue(hash, static_cast<std::uint64_t>(source->renderToParticle.size()));
    for (const std::uint32_t particle : source->renderToParticle) {
        if (particle >= particleCount) return 0;
        HashValue(hash, particle);
    }
    HashValue(hash, static_cast<std::uint64_t>(source->renderBindings.size()));
    for (const asset::ClothRenderBinding& binding : source->renderBindings) {
        for (const std::uint32_t particle : binding.particles) {
            if (particle >= particleCount) return 0;
            HashValue(hash, particle);
        }
    }
    return hash == 0 ? 1 : hash;
}

std::uint64_t HashWaterValues(std::uint64_t hash, std::span<const WaterSplinePoint> points)
{
    for (const WaterSplinePoint& point : points) {
        HashValue(hash, point.id);
        HashVector(hash, point.localPosition);
        HashFloat(hash, point.width);
        HashFloat(hash, point.depth);
        HashFloat(hash, point.flowSpeed);
    }
    return hash == 0 ? 1 : hash;
}

std::uint64_t HashSplineValues(std::uint64_t hash, std::span<const SplineControlPointValue> points)
{
    for (const SplineControlPointValue& point : points) {
        HashValue(hash, point.id);
        HashVector(hash, point.localPosition);
    }
    return hash == 0 ? 1 : hash;
}

} /// @note namespace

void WaterControlPointBinding::Reflect(IReflector& reflector)
{
    int pointId = static_cast<int>((std::min)(id, WATER_SPLINE_MAX_POINT_ID));
    reflector.Field("id", pointId);
    id = pointId < 0 ? 0u : static_cast<std::uint32_t>(pointId);
    reflector.TypedObjectReferenceField("target", target, "TargetPointComponent");
    reflector.Field("width", width);
    reflector.Field("depth", depth);
    reflector.Field("flowSpeed", flowSpeed);
}

bool WaterControlPointBinding::operator==(const WaterControlPointBinding& other) const
{
    return id == other.id && target.guid == other.target.guid && target.id == other.target.id
        && target.sceneGeneration == other.target.sceneGeneration && width == other.width
        && depth == other.depth && flowSpeed == other.flowSpeed;
}

void SplineControlPointBinding::Reflect(IReflector& reflector)
{
    int pointId = static_cast<int>((std::min)(id, 0x7fffffffu));
    reflector.Field("id", pointId);
    id = pointId < 0 ? 0u : static_cast<std::uint32_t>(pointId);
    reflector.TypedObjectReferenceField("target", target, "TargetPointComponent");
}

bool SplineControlPointBinding::operator==(const SplineControlPointBinding& other) const
{
    return id == other.id && target.guid == other.target.guid && target.id == other.target.id
        && target.sceneGeneration == other.target.sceneGeneration;
}

void ReflectWaterControlPointBindings(IReflector& reflector,
    std::vector<WaterControlPointBinding>& bindings)
{
    ReflectRows(reflector, "pointBindings", bindings, WATER_SPLINE_MAX_CONTROL_POINTS,
        [&reflector](WaterControlPointBinding& binding) { binding.Reflect(reflector); });
}

void ReflectSplineControlPointBindings(IReflector& reflector,
    std::vector<SplineControlPointBinding>& bindings)
{
    ReflectRows(reflector, "pointBindings", bindings, MAX_CONTROL_POINT_BINDINGS,
        [&reflector](SplineControlPointBinding& binding) { binding.Reflect(reflector); });
}

void ReflectSplineControlPointValues(IReflector& reflector,
    const char* fieldName, std::vector<SplineControlPointValue>& points)
{
    ReflectRows(reflector, fieldName, points, MAX_CONTROL_POINT_BINDINGS,
        [&reflector](SplineControlPointValue& point) { ReflectSplineValue(reflector, point); });
}

void ReflectWaterControlPointValues(IReflector& reflector,
    const char* fieldName, std::vector<WaterSplinePoint>& points)
{
    ReflectRows(reflector, fieldName, points, WATER_SPLINE_MAX_CONTROL_POINTS,
        [&reflector](WaterSplinePoint& point) { point.Reflect(reflector); });
}

void ReflectControlPointSourceMode(IReflector& reflector,
    const char* fieldName, ControlPointSourceMode& mode)
{
    ReflectPointSourceMode(reflector, fieldName, mode);
}

void ReflectControlPointRuntimeMode(IReflector& reflector,
    const char* fieldName, ControlPointRuntimeMode& mode)
{
    ReflectPointRuntimeMode(reflector, fieldName, mode);
}

void ReflectControlPointSignature(IReflector& reflector,
    const char* fieldName, std::uint64_t& signature)
{
    ReflectSignature(reflector, fieldName, signature);
}

void ReflectControlPointVersion(IReflector& reflector,
    const char* fieldName, std::uint32_t& version)
{
    ReflectVersion(reflector, fieldName, version);
}

std::uint64_t ClothTopologySignature(const asset::ClothAsset* source,
    std::uint32_t generatedSegments)
{
    std::uint32_t particleCount = 0;
    return ComputeClothTopologySignature(source, generatedSegments, particleCount);
}

bool ResolveClothTopologyForParticle(const asset::ClothAsset* source,
    std::uint32_t generatedSegments, int particle, std::uint64_t& signature,
    std::uint32_t& particleCount)
{
    std::uint32_t currentCount = 0;
    const std::uint64_t currentSignature = ComputeClothTopologySignature(
        source, generatedSegments, currentCount);
    if (currentSignature == 0 || particle < 0
        || static_cast<std::uint32_t>(particle) >= currentCount) return false;
    signature = currentSignature;
    particleCount = currentCount;
    return true;
}

WaterControlPointSnapshot ResolveWaterControlPointAuthoring(
    Scene& scene, EntityID ownerId, WaterComponent& water,
    ControlPointRuntimeMode signatureRuntimeMode)
{
    WaterControlPointSnapshot snapshot;
    snapshot.inputSignature = WaterRawSignature(water, signatureRuntimeMode);
    snapshot.surfaceShape = water.surfaceShape;
    snapshot.lakePlaneHeight = water.lakePlaneHeight;
    if (!IsKnownRuntimeMode(signatureRuntimeMode)) {
        snapshot.status = ControlPointAdapterStatus::INVALID_RUNTIME_MODE;
        return snapshot;
    }
    if (!IsKnownSourceMode(water.pointSourceMode)) {
        snapshot.status = ControlPointAdapterStatus::INVALID_SOURCE_MODE;
        return snapshot;
    }

    if (water.pointSourceMode == ControlPointSourceMode::INLINE) {
        snapshot.points = water.splinePoints;
    } else {
        if (water.pointBindings.size() > WATER_SPLINE_MAX_CONTROL_POINTS) {
            snapshot.status = ControlPointAdapterStatus::INVALID_NUMERIC_DATA;
            snapshot.resolveStatus = ControlPointResolveStatus::TOO_MANY_BINDINGS;
            return snapshot;
        }
        std::vector<ControlPointBinding> bindings;
        bindings.reserve(water.pointBindings.size());
        for (WaterControlPointBinding& binding : water.pointBindings)
            bindings.push_back({binding.id, &binding.target});
        std::vector<ResolvedControlPoint> resolved;
        std::uint64_t resolverSignature = 0;
        if (!ResolveControlPointBindings(scene, ownerId, bindings, true,
                resolved, snapshot.resolveStatus, resolverSignature)) {
            snapshot.status = ControlPointAdapterStatus::RESOLUTION_FAILED;
            snapshot.inputSignature = WaterRawSignature(water, signatureRuntimeMode);
            HashValue(snapshot.inputSignature, snapshot.resolveStatus);
            if (snapshot.inputSignature == 0) snapshot.inputSignature = 1;
            return snapshot;
        }
        snapshot.points.reserve(resolved.size());
        for (std::size_t index = 0; index < resolved.size(); ++index) {
            const WaterControlPointBinding& binding = water.pointBindings[index];
            snapshot.points.push_back({binding.id, resolved[index].ownerLocalPosition,
                binding.width, binding.depth, binding.flowSpeed});
        }
        snapshot.inputSignature = WaterRawSignature(water, signatureRuntimeMode);
    }

    if (snapshot.surfaceShape == WaterSurfaceShape::SPLINE_LAKE
        && water.pointSourceMode == ControlPointSourceMode::INLINE && !snapshot.points.empty())
        snapshot.lakePlaneHeight = snapshot.points.front().localPosition.y;
    if (!ValidateWaterPoints(snapshot.surfaceShape, snapshot.lakePlaneHeight, true,
            snapshot.points, snapshot.status)) {
        snapshot.inputSignature = HashWaterValues(snapshot.inputSignature, snapshot.points);
        return snapshot;
    }
    snapshot.inputSignature = HashWaterValues(snapshot.inputSignature, snapshot.points);
    snapshot.status = ControlPointAdapterStatus::SUCCESS;
    snapshot.resolveStatus = ControlPointResolveStatus::SUCCESS;
    snapshot.valid = true;
    return snapshot;
}

WaterControlPointSnapshot ResolveWaterControlPoints(
    Scene& scene, EntityID ownerId, WaterComponent& water, ControlPointReadContext context)
{
    if (water.pointRuntimeMode == ControlPointRuntimeMode::LIVE)
        return ResolveWaterControlPointAuthoring(scene, ownerId, water, ControlPointRuntimeMode::LIVE);

    WaterControlPointSnapshot current;
    if (!IsKnownRuntimeMode(water.pointRuntimeMode)) {
        current.status = ControlPointAdapterStatus::INVALID_RUNTIME_MODE;
        current.valid = false;
        return current;
    }
    if (context == ControlPointReadContext::AUTHORING)
        current = ResolveWaterControlPointAuthoring(
            scene, ownerId, water, ControlPointRuntimeMode::BAKED_STATIC);
    else
        current.inputSignature = water.bakedInputSignature;
    if (!water.bakeValid || water.bakedSplinePoints.empty()) {
        current.points.clear();
        current.status = ControlPointAdapterStatus::MISSING_BAKE;
        current.valid = false;
        return current;
    }

    if (context == ControlPointReadContext::AUTHORING)
        current.bakeStale = !current.valid || current.inputSignature != water.bakedInputSignature
            || water.bakedSourceMode != water.pointSourceMode
            || current.points != water.bakedSplinePoints
            || current.surfaceShape != water.bakedSurfaceShape
            || current.lakePlaneHeight != water.bakedLakePlaneHeight;
    if (water.bakedInputSignature == 0 || !IsKnownSourceMode(water.bakedSourceMode)
        || water.bakedAdapterVersion != CONTROL_POINT_ADAPTER_VERSION) {
        current.bakeStale = true;
        current.status = ControlPointAdapterStatus::STALE_BAKE;
        current.valid = false;
        return current;
    }
    current.points = water.bakedSplinePoints;
    ControlPointAdapterStatus bakedStatus = ControlPointAdapterStatus::SUCCESS;
    current.surfaceShape = water.bakedSurfaceShape;
    current.lakePlaneHeight = water.bakedLakePlaneHeight;
    if (!ValidateWaterPoints(current.surfaceShape, current.lakePlaneHeight, true,
            current.points, bakedStatus)) {
        current.status = bakedStatus;
        current.valid = false;
        return current;
    }
    current.valid = true;
    current.status = current.bakeStale ? ControlPointAdapterStatus::STALE_BAKE
                                       : ControlPointAdapterStatus::SUCCESS;
    return current;
}

SplineControlPointSnapshot ResolveSplinePointAuthoring(
    Scene& scene, EntityID ownerId, SplineComponent& spline,
    ControlPointRuntimeMode signatureRuntimeMode)
{
    SplineControlPointSnapshot snapshot;
    snapshot.inputSignature = SplineRawSignature(spline, signatureRuntimeMode);
    snapshot.closed = spline.closed;
    if (!IsKnownRuntimeMode(signatureRuntimeMode)) {
        snapshot.status = ControlPointAdapterStatus::INVALID_RUNTIME_MODE;
        return snapshot;
    }
    if (!IsKnownSourceMode(spline.pointSourceMode)) {
        snapshot.status = ControlPointAdapterStatus::INVALID_SOURCE_MODE;
        return snapshot;
    }

    if (spline.pointSourceMode == ControlPointSourceMode::INLINE) {
        snapshot.points.reserve(spline.points.size());
        for (std::size_t index = 0; index < spline.points.size(); ++index)
            snapshot.points.push_back({static_cast<std::uint32_t>(index + 1), spline.points[index]});
    } else {
        if (spline.pointBindings.size() > MAX_CONTROL_POINT_BINDINGS) {
            snapshot.status = ControlPointAdapterStatus::INVALID_NUMERIC_DATA;
            snapshot.resolveStatus = ControlPointResolveStatus::TOO_MANY_BINDINGS;
            return snapshot;
        }
        std::vector<ControlPointBinding> bindings;
        bindings.reserve(spline.pointBindings.size());
        for (SplineControlPointBinding& binding : spline.pointBindings)
            bindings.push_back({binding.id, &binding.target});
        std::vector<ResolvedControlPoint> resolved;
        std::uint64_t resolverSignature = 0;
        if (!ResolveControlPointBindings(scene, ownerId, bindings, true,
                resolved, snapshot.resolveStatus, resolverSignature)) {
            snapshot.status = ControlPointAdapterStatus::RESOLUTION_FAILED;
            snapshot.inputSignature = SplineRawSignature(spline, signatureRuntimeMode);
            HashValue(snapshot.inputSignature, snapshot.resolveStatus);
            if (snapshot.inputSignature == 0) snapshot.inputSignature = 1;
            return snapshot;
        }
        snapshot.points.reserve(resolved.size());
        for (const ResolvedControlPoint& point : resolved)
            snapshot.points.push_back({point.stableId, point.ownerLocalPosition});
        snapshot.inputSignature = SplineRawSignature(spline, signatureRuntimeMode);
    }

    if (!ValidateSplinePoints(snapshot.points, snapshot.status)) {
        snapshot.inputSignature = HashSplineValues(snapshot.inputSignature, snapshot.points);
        return snapshot;
    }
    snapshot.inputSignature = HashSplineValues(snapshot.inputSignature, snapshot.points);
    snapshot.status = ControlPointAdapterStatus::SUCCESS;
    snapshot.resolveStatus = ControlPointResolveStatus::SUCCESS;
    snapshot.valid = true;
    return snapshot;
}

SplineControlPointSnapshot ResolveSplinePoints(
    Scene& scene, EntityID ownerId, SplineComponent& spline, ControlPointReadContext context)
{
    if (spline.pointRuntimeMode == ControlPointRuntimeMode::LIVE)
        return ResolveSplinePointAuthoring(scene, ownerId, spline, ControlPointRuntimeMode::LIVE);

    SplineControlPointSnapshot current;
    if (!IsKnownRuntimeMode(spline.pointRuntimeMode)) {
        current.status = ControlPointAdapterStatus::INVALID_RUNTIME_MODE;
        current.valid = false;
        return current;
    }
    if (context == ControlPointReadContext::AUTHORING)
        current = ResolveSplinePointAuthoring(
            scene, ownerId, spline, ControlPointRuntimeMode::BAKED_STATIC);
    else
        current.inputSignature = spline.bakedInputSignature;
    if (!spline.bakeValid || spline.bakedPoints.empty()) {
        current.points.clear();
        current.status = ControlPointAdapterStatus::MISSING_BAKE;
        current.valid = false;
        return current;
    }

    if (context == ControlPointReadContext::AUTHORING)
        current.bakeStale = !current.valid || current.inputSignature != spline.bakedInputSignature
            || spline.bakedSourceMode != spline.pointSourceMode
            || current.points != spline.bakedPoints || current.closed != spline.bakedClosed;
    if (spline.bakedInputSignature == 0 || !IsKnownSourceMode(spline.bakedSourceMode)
        || spline.bakedAdapterVersion != CONTROL_POINT_ADAPTER_VERSION) {
        current.bakeStale = true;
        current.status = ControlPointAdapterStatus::STALE_BAKE;
        current.valid = false;
        return current;
    }
    current.points = spline.bakedPoints;
    current.closed = spline.bakedClosed;
    if (!ValidateSplinePoints(current.points, current.status)) {
        current.valid = false;
        return current;
    }
    current.valid = true;
    current.status = current.bakeStale ? ControlPointAdapterStatus::STALE_BAKE
                                       : ControlPointAdapterStatus::SUCCESS;
    return current;
}

bool ValidateStaticControlPointBakes(Scene& scene)
{
    bool valid = ValidateStaticNavMeshPatrolBakes(scene);
    for (const EntityID id : scene.GetEntities<WaterComponent>()) {
        WaterComponent* water = scene.GetComponent<WaterComponent>(id);
        if (!water || water->pointRuntimeMode == ControlPointRuntimeMode::LIVE) continue;
        const WaterControlPointSnapshot snapshot = ResolveWaterControlPoints(scene, id, *water);
        if (snapshot.valid && !snapshot.bakeStale
            && snapshot.status == ControlPointAdapterStatus::SUCCESS) continue;
        FBZZ_LOG_ERROR("ControlPointAdapters: Water static bake is not ready on entity %u:%u (status=%u, resolve=%u).",
            id.index, id.generation, static_cast<unsigned>(snapshot.status),
            static_cast<unsigned>(snapshot.resolveStatus));
        valid = false;
    }
    for (const EntityID id : scene.GetEntities<SplineComponent>()) {
        SplineComponent* spline = scene.GetComponent<SplineComponent>(id);
        if (!spline || spline->pointRuntimeMode == ControlPointRuntimeMode::LIVE) continue;
        const SplineControlPointSnapshot snapshot = ResolveSplinePoints(scene, id, *spline);
        if (snapshot.valid && !snapshot.bakeStale
            && snapshot.status == ControlPointAdapterStatus::SUCCESS) continue;
        FBZZ_LOG_ERROR("ControlPointAdapters: Spline static bake is not ready on entity %u:%u (status=%u, resolve=%u).",
            id.index, id.generation, static_cast<unsigned>(snapshot.status),
            static_cast<unsigned>(snapshot.resolveStatus));
        valid = false;
    }
    for (const EntityID id : scene.GetEntities<LineRendererComponent>()) {
        auto* line = scene.GetComponent<LineRendererComponent>(id);
        if (!line || line->pointRuntimeMode == ControlPointRuntimeMode::LIVE) continue;
        const auto snapshot = ResolveLineControlPoints(scene, id, *line);
        if (snapshot.valid && !snapshot.bakeStale) continue;
        FBZZ_LOG_ERROR("ControlPointAdapters: Line static bake is not ready on entity %u:%u.", id.index, id.generation);
        valid = false;
    }
    return valid;
}

} /// @note namespace fbzz::scene
