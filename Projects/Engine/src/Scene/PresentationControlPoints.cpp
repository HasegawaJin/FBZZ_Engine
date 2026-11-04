/// @file    PresentationControlPoints.cpp
/// @brief   Resolve Line and MotionWarp TargetPoint references into numeric runtime inputs.
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <Engine/Scene/Components/MotionWarpComponent.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/PresentationControlPoints.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Scene.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace fbzz::scene {
namespace {

constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ull;
constexpr std::uint64_t FNV_PRIME = 1099511628211ull;
constexpr std::size_t MAX_LINE_CONTROL_POINTS = MAX_CONTROL_POINT_BINDINGS;

void HashBytes(std::uint64_t& hash, const void* bytes, std::size_t size)
{
    const auto* values = static_cast<const std::uint8_t*>(bytes);
    for (std::size_t index = 0; index < size; ++index) {
        hash = (hash ^ values[index]) * FNV_PRIME;
    }
}

template<typename T>
void HashValue(std::uint64_t& hash, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    HashBytes(hash, &value, sizeof(value));
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

void HashString(std::uint64_t& hash, const std::string& value)
{
    HashValue(hash, static_cast<std::uint64_t>(value.size()));
    HashBytes(hash, value.data(), value.size());
}

bool IsFinite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool IsSelfOrDescendantTarget(Scene& scene, EntityID ownerId, EntityID targetId)
{
    auto* owner = scene.GetGameObject(ownerId);
    auto* target = scene.GetGameObject(targetId);
    return !owner || !target || target->GetID() == ownerId || target->IsDescendantOf(*owner);
}

bool IsKnownSourceMode(ControlPointSourceMode mode)
{
    return mode == ControlPointSourceMode::INLINE || mode == ControlPointSourceMode::POINT_OBJECTS;
}

bool IsKnownRuntimeMode(ControlPointRuntimeMode mode)
{
    return mode == ControlPointRuntimeMode::LIVE || mode == ControlPointRuntimeMode::BAKED_STATIC;
}

std::uint64_t RawLineSignature(const LineRendererComponent& line,
    ControlPointRuntimeMode runtimeMode)
{
    std::uint64_t hash = FNV_OFFSET;
    HashValue(hash, line.pointSourceMode);
    HashValue(hash, runtimeMode);
    HashValue(hash, line.space);
    HashValue(hash, LINE_CONTROL_POINT_ADAPTER_VERSION);
    if (line.pointSourceMode == ControlPointSourceMode::INLINE) {
        HashValue(hash, static_cast<std::uint64_t>(line.points.size()));
        for (std::size_t index = 0; index < line.points.size(); ++index) {
            HashValue(hash, static_cast<std::uint32_t>(index + 1));
            HashVector(hash, line.points[index]);
        }
    } else {
        HashValue(hash, static_cast<std::uint64_t>(line.pointBindings.size()));
        for (const LineControlPointBinding& binding : line.pointBindings) {
            HashValue(hash, binding.id);
            HashString(hash, binding.target.guid);
        }
    }
    return hash == 0 ? 1 : hash;
}

std::uint64_t CanonicalLineSignature(ControlPointSourceMode sourceMode,
    ControlPointRuntimeMode runtimeMode, LineSpace space,
    std::span<const std::uint32_t> ids, std::span<const math::Vector3> points)
{
    std::uint64_t hash = FNV_OFFSET;
    HashValue(hash, sourceMode);
    HashValue(hash, runtimeMode);
    HashValue(hash, space);
    HashValue(hash, LINE_CONTROL_POINT_ADAPTER_VERSION);
    HashValue(hash, static_cast<std::uint64_t>(points.size()));
    for (std::size_t index = 0; index < points.size(); ++index) {
        HashValue(hash, ids[index]);
        HashVector(hash, points[index]);
    }
    return hash == 0 ? 1 : hash;
}

bool ValidatePoints(std::span<const LineControlPointValue> points)
{
    if (points.size() < 2 || points.size() > MAX_LINE_CONTROL_POINTS) return false;
    std::unordered_set<std::uint32_t> ids;
    ids.reserve(points.size());
    for (const LineControlPointValue& point : points) {
        if (point.id == 0 || point.id > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())
            || !ids.insert(point.id).second || !IsFinite(point.position)) return false;
    }
    return true;
}

bool MatchesBakedPoints(const LineRendererComponent& line,
    std::span<const math::Vector3> current)
{
    if (current.size() != line.bakedPoints.size()) return false;
    for (std::size_t index = 0; index < current.size(); ++index) {
        const std::uint32_t expectedId = line.pointSourceMode == ControlPointSourceMode::INLINE
            ? static_cast<std::uint32_t>(index + 1) : line.pointBindings[index].id;
        const LineControlPointValue& baked = line.bakedPoints[index];
        if (baked.id != expectedId || baked.position.x != current[index].x
            || baked.position.y != current[index].y || baked.position.z != current[index].z) return false;
    }
    return true;
}

template<typename T, typename ReflectRow>
void ReflectRows(IReflector& reflector, const char* fieldName,
    std::vector<T>& values, ReflectRow reflectRow)
{
    const std::size_t requested = reflector.BeginObjectList(fieldName, values.size());
    if (requested != values.size())
        values.resize((std::min)(requested, MAX_LINE_CONTROL_POINTS + 1));
    for (std::size_t index = 0; index < values.size(); ++index) {
        reflector.BeginObjectElement(index);
        reflectRow(values[index]);
        reflector.EndObjectElement();
    }
    const std::size_t removed = reflector.EndObjectList();
    if (removed < values.size())
        values.erase(values.begin() + static_cast<std::ptrdiff_t>(removed));
    std::size_t from = 0;
    std::size_t to = 0;
    if (reflector.ObjectListMove(from, to) && from < values.size() && to < values.size() && from != to) {
        T moved = std::move(values[from]);
        values.erase(values.begin() + static_cast<std::ptrdiff_t>(from));
        values.insert(values.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    }
}

void FailResolution(LineControlPointSnapshot& snapshot,
    const LineRendererComponent& line, ControlPointRuntimeMode runtimeMode)
{
    snapshot.status = LineControlPointStatus::RESOLUTION_FAILED;
    snapshot.inputSignature = RawLineSignature(line, runtimeMode);
    HashValue(snapshot.inputSignature, snapshot.resolveStatus);
    if (snapshot.inputSignature == 0) snapshot.inputSignature = 1;
}

} /// @note namespace

void LineControlPointBinding::Reflect(IReflector& reflector)
{
    int value = static_cast<int>((std::min)(id, static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())));
    reflector.Field("id", value);
    id = value < 0 ? 0u : static_cast<std::uint32_t>(value);
    reflector.TypedObjectReferenceField("target", target, "TargetPointComponent");
}

bool LineControlPointBinding::operator==(const LineControlPointBinding& other) const
{
    return id == other.id && target.guid == other.target.guid;
}

void LineControlPointValue::Reflect(IReflector& reflector)
{
    int value = static_cast<int>((std::min)(id, static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())));
    reflector.Field("id", value);
    id = value < 0 ? 0u : static_cast<std::uint32_t>(value);
    reflector.Field("position", position);
}

void ReflectLineControlPointBindings(IReflector& reflector,
    std::vector<LineControlPointBinding>& bindings)
{
    ReflectRows(reflector, "pointBindings", bindings,
        [&reflector](LineControlPointBinding& binding) { binding.Reflect(reflector); });
}

void ReflectLineControlPointValues(IReflector& reflector, const char* fieldName,
    std::vector<LineControlPointValue>& points)
{
    ReflectRows(reflector, fieldName, points,
        [&reflector](LineControlPointValue& point) { point.Reflect(reflector); });
}

LineControlPointSnapshot ResolveLineControlPointAuthoring(Scene& scene, EntityID ownerId,
    LineRendererComponent& line, ControlPointRuntimeMode signatureRuntimeMode)
{
    LineControlPointSnapshot snapshot;
    snapshot.space = line.space;
    snapshot.inputSignature = RawLineSignature(line, signatureRuntimeMode);
    if (!IsKnownRuntimeMode(signatureRuntimeMode)) {
        snapshot.status = LineControlPointStatus::INVALID_RUNTIME_MODE;
        return snapshot;
    }
    if (!IsKnownSourceMode(line.pointSourceMode)) {
        snapshot.status = LineControlPointStatus::INVALID_SOURCE_MODE;
        return snapshot;
    }
    if (line.space != LineSpace::Local && line.space != LineSpace::World) {
        snapshot.status = LineControlPointStatus::INVALID_NUMERIC_DATA;
        return snapshot;
    }

    std::vector<std::uint32_t> ids;
    if (line.pointSourceMode == ControlPointSourceMode::INLINE) {
        if (line.points.size() > MAX_LINE_CONTROL_POINTS) {
            snapshot.status = LineControlPointStatus::INVALID_NUMERIC_DATA;
            return snapshot;
        }
        snapshot.points = line.points;
        ids.reserve(snapshot.points.size());
        for (std::size_t index = 0; index < snapshot.points.size(); ++index)
            ids.push_back(static_cast<std::uint32_t>(index + 1));
    } else {
        if (line.pointBindings.size() > MAX_LINE_CONTROL_POINTS) {
            snapshot.status = LineControlPointStatus::INVALID_NUMERIC_DATA;
            snapshot.resolveStatus = ControlPointResolveStatus::TOO_MANY_BINDINGS;
            return snapshot;
        }
        std::vector<ControlPointBinding> bindings;
        bindings.reserve(line.pointBindings.size());
        for (const LineControlPointBinding& binding : line.pointBindings)
            bindings.push_back({binding.id, &binding.target});
        std::vector<ResolvedControlPoint> resolved;
        std::uint64_t resolverSignature = 0;
        if (!ResolveControlPointBindings(scene, ownerId, bindings, true,
                resolved, snapshot.resolveStatus, resolverSignature)) {
            FailResolution(snapshot, line, signatureRuntimeMode);
            return snapshot;
        }
        snapshot.points.reserve(resolved.size());
        ids.reserve(resolved.size());
        for (std::size_t index = 0; index < resolved.size(); ++index) {
            ids.push_back(resolved[index].stableId);
            snapshot.points.push_back(line.space == LineSpace::World
                ? resolved[index].worldPosition : resolved[index].ownerLocalPosition);
        }
    }

    if (snapshot.points.size() < 2 || snapshot.points.size() > MAX_LINE_CONTROL_POINTS
        || !std::all_of(snapshot.points.begin(), snapshot.points.end(), IsFinite)) {
        snapshot.status = LineControlPointStatus::INVALID_NUMERIC_DATA;
        snapshot.inputSignature = RawLineSignature(line, signatureRuntimeMode);
        for (const math::Vector3& point : snapshot.points) HashVector(snapshot.inputSignature, point);
        if (snapshot.inputSignature == 0) snapshot.inputSignature = 1;
        return snapshot;
    }
    snapshot.inputSignature = CanonicalLineSignature(line.pointSourceMode,
        signatureRuntimeMode, line.space, ids, snapshot.points);
    snapshot.status = LineControlPointStatus::SUCCESS;
    snapshot.resolveStatus = ControlPointResolveStatus::SUCCESS;
    snapshot.valid = true;
    return snapshot;
}

LineControlPointSnapshot ResolveLineControlPoints(Scene& scene, EntityID ownerId,
    LineRendererComponent& line, ControlPointReadContext context)
{
    if (line.pointRuntimeMode == ControlPointRuntimeMode::LIVE)
        return ResolveLineControlPointAuthoring(scene, ownerId, line, ControlPointRuntimeMode::LIVE);

    LineControlPointSnapshot current;
    if (!IsKnownRuntimeMode(line.pointRuntimeMode)) {
        current.status = LineControlPointStatus::INVALID_RUNTIME_MODE;
        current.valid = false;
        return current;
    }
    if (context == ControlPointReadContext::AUTHORING)
        current = ResolveLineControlPointAuthoring(scene, ownerId, line, ControlPointRuntimeMode::BAKED_STATIC);
    else
        current.inputSignature = line.bakedInputSignature;

    if (!line.bakeValid || line.bakedPoints.empty()) {
        current.points.clear();
        current.status = LineControlPointStatus::MISSING_BAKE;
        current.valid = false;
        return current;
    }
    if (context == ControlPointReadContext::AUTHORING)
        current.bakeStale = !current.valid || current.inputSignature != line.bakedInputSignature
            || line.bakedSourceMode != line.pointSourceMode || line.bakedSpace != line.space
            || (current.valid && !MatchesBakedPoints(line, current.points));
    if (line.bakedInputSignature == 0 || !IsKnownSourceMode(line.bakedSourceMode)
        || line.bakedAdapterVersion != LINE_CONTROL_POINT_ADAPTER_VERSION
        || (line.bakedSpace != LineSpace::Local && line.bakedSpace != LineSpace::World)
        || !ValidatePoints(line.bakedPoints)) {
        current.bakeStale = true;
        current.status = LineControlPointStatus::STALE_BAKE;
        current.valid = false;
        return current;
    }

    current.space = line.bakedSpace;
    current.points.clear();
    current.points.reserve(line.bakedPoints.size());
    for (const LineControlPointValue& point : line.bakedPoints)
        current.points.push_back(point.position);
    current.valid = true;
    current.status = current.bakeStale ? LineControlPointStatus::STALE_BAKE
                                       : LineControlPointStatus::SUCCESS;
    return current;
}

bool BeginMotionWarpToTarget(Scene& scene, EntityID ownerId,
    MotionWarpComponent& warp, float duration)
{
    if (!std::isfinite(duration) || duration <= 0.0f
        || (warp.targetPointMode != MotionWarpPointMode::CAPTURED
            && warp.targetPointMode != MotionWarpPointMode::LIVE)) return false;
    const ControlPointBinding binding{1, &warp.targetReference};
    std::vector<ResolvedControlPoint> resolved;
    ControlPointResolveStatus status = ControlPointResolveStatus::SUCCESS;
    std::uint64_t signature = 0;
    if (!ResolveControlPointBindings(scene, ownerId, std::span{&binding, std::size_t{1}},
            true, resolved, status, signature) || resolved.size() != 1) return false;
    if (warp.targetPointMode == MotionWarpPointMode::LIVE
        && IsSelfOrDescendantTarget(scene, ownerId, resolved.front().targetId)) return false;

    MotionWarpTarget& target = warp.target;
    target.active = true;
    target.position = resolved.front().worldPosition;
    target.rotation = resolved.front().worldRotation.Normalized();
    target.warpPosition = true;
    target.warpRotation = true;
    target.duration = duration;
    target.remaining = duration;
    warp.runtimeTargetSource = MotionWarpTargetSource::POINT_OBJECT;
    warp.runtimeLastCorrection = math::Vector3::ZERO;
    warp.runtimeRemainingDistance = 0.0f;
    ++warp.runtimeWarpCount;
    return true;
}

bool RefreshLiveMotionWarpTarget(Scene& scene, EntityID ownerId,
    MotionWarpComponent& warp)
{
    if (!warp.target.active || warp.runtimeTargetSource != MotionWarpTargetSource::POINT_OBJECT) return true;
    if (warp.targetPointMode == MotionWarpPointMode::CAPTURED) return true;
    if (warp.targetPointMode != MotionWarpPointMode::LIVE) {
        warp.target.active = false;
        warp.target.remaining = 0.0f;
        warp.runtimeRemainingDistance = 0.0f;
        warp.runtimeLastCorrection = math::Vector3::ZERO;
        return false;
    }

    const ControlPointBinding binding{1, &warp.targetReference};
    std::vector<ResolvedControlPoint> resolved;
    ControlPointResolveStatus status = ControlPointResolveStatus::SUCCESS;
    std::uint64_t signature = 0;
    if (!ResolveControlPointBindings(scene, ownerId, std::span{&binding, std::size_t{1}},
            true, resolved, status, signature) || resolved.size() != 1
        || IsSelfOrDescendantTarget(scene, ownerId, resolved.front().targetId)) {
        warp.target.active = false;
        warp.target.remaining = 0.0f;
        warp.runtimeRemainingDistance = 0.0f;
        warp.runtimeLastCorrection = math::Vector3::ZERO;
        return false;
    }
    warp.target.position = resolved.front().worldPosition;
    warp.target.rotation = resolved.front().worldRotation.Normalized();
    return true;
}

} /// @note namespace fbzz::scene
