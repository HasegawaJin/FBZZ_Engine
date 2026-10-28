/// @file    ControlPointResolver.cpp
/// @brief   ControlPoint 参照の owner-relative 数値解決。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <Engine/Scene/ControlPointResolver.hpp>

#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fbzz::scene {
namespace {

constexpr double MIN_ABS_SCALE = 1.0e-6;
constexpr double QUATERNION_LENGTH_TOLERANCE = 1.0e-3;
constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
constexpr uint64_t FNV_PRIME = 1099511628211ull;

struct Double3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

bool IsFinite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool IsFinite(const math::Quaternion& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z) && std::isfinite(value.w);
}

bool IsUnitQuaternion(const math::Quaternion& value)
{
    if (!IsFinite(value)) return false;
    const double length = std::sqrt(static_cast<double>(value.x) * value.x
                                  + static_cast<double>(value.y) * value.y
                                  + static_cast<double>(value.z) * value.z
                                  + static_cast<double>(value.w) * value.w);
    return std::isfinite(length) && std::abs(length - 1.0) <= QUATERNION_LENGTH_TOLERANCE;
}

bool IsInvertibleScale(const math::Vector3& value)
{
    return IsFinite(value) && std::abs(value.x) > MIN_ABS_SCALE
        && std::abs(value.y) > MIN_ABS_SCALE && std::abs(value.z) > MIN_ABS_SCALE;
}

bool IsFiniteTransform(const Transform& transform, bool requireInvertibleScale)
{
    return IsFinite(transform.position) && IsFinite(transform.worldPosition)
        && IsFinite(transform.scale) && IsFinite(transform.worldScale)
        && IsUnitQuaternion(transform.rotation) && IsUnitQuaternion(transform.worldRotation)
        && (!requireInvertibleScale || (IsInvertibleScale(transform.scale)
                                        && IsInvertibleScale(transform.worldScale)));
}

Double3 Add(Double3 lhs, Double3 rhs)
{
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

Double3 Hadamard(Double3 lhs, const math::Vector3& rhs)
{
    return {lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z};
}

Double3 Hadamard(Double3 lhs, Double3 rhs)
{
    return {lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z};
}

Double3 Rotate(const math::Quaternion& rotation, Double3 value)
{
    const Double3 axis{rotation.x, rotation.y, rotation.z};
    const Double3 cross1{
        axis.y * value.z - axis.z * value.y,
        axis.z * value.x - axis.x * value.z,
        axis.x * value.y - axis.y * value.x};
    const Double3 cross2{
        axis.y * cross1.z - axis.z * cross1.y,
        axis.z * cross1.x - axis.x * cross1.z,
        axis.x * cross1.y - axis.y * cross1.x};
    return {value.x + 2.0 * (rotation.w * cross1.x + cross2.x),
            value.y + 2.0 * (rotation.w * cross1.y + cross2.y),
            value.z + 2.0 * (rotation.w * cross1.z + cross2.z)};
}

bool ToVector3(Double3 value, math::Vector3& out)
{
    if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)
        || std::abs(value.x) > static_cast<double>(std::numeric_limits<float>::max())
        || std::abs(value.y) > static_cast<double>(std::numeric_limits<float>::max())
        || std::abs(value.z) > static_cast<double>(std::numeric_limits<float>::max()))
        return false;
    out = {static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z)};
    return IsFinite(out);
}

bool OwnerLocalPosition(const GameObject& owner, const GameObject& target, math::Vector3& out)
{
    if (&owner == &target) {
        out = math::Vector3::ZERO;
        return true;
    }

    if (target.IsDescendantOf(owner)) {
        std::vector<const GameObject*> chain;
        for (const GameObject* current = &target; current && current != &owner; current = current->GetParent()) {
            if (!IsFiniteTransform(current->transform, true)) return false;
            chain.push_back(current);
        }
        if (chain.empty() || chain.back()->GetParent() != &owner) return false;

        Double3 relativeWorld{};
        for (auto current = chain.rbegin(); current != chain.rend(); ++current) {
            const Transform& transform = (*current)->transform;
            const GameObject* parent = (*current)->GetParent();
            if (!parent || !IsFiniteTransform(parent->transform, true)) return false;
            const Double3 localPosition{transform.position.x, transform.position.y, transform.position.z};
            relativeWorld = Add(relativeWorld, Rotate(parent->transform.worldRotation,
                Hadamard(localPosition, parent->transform.worldScale)));
        }
        const math::Quaternion inverseOwnerRotation{
            -owner.transform.worldRotation.x,
            -owner.transform.worldRotation.y,
            -owner.transform.worldRotation.z,
             owner.transform.worldRotation.w};
        const Double3 ownerLocal = Rotate(inverseOwnerRotation, relativeWorld);
        return ToVector3({ownerLocal.x / owner.transform.worldScale.x,
                          ownerLocal.y / owner.transform.worldScale.y,
                          ownerLocal.z / owner.transform.worldScale.z}, out);
    }

    const Double3 worldDelta{
        static_cast<double>(target.transform.worldPosition.x) - owner.transform.worldPosition.x,
        static_cast<double>(target.transform.worldPosition.y) - owner.transform.worldPosition.y,
        static_cast<double>(target.transform.worldPosition.z) - owner.transform.worldPosition.z};
    /// @note The conjugate is the inverse for the already-validated unit owner rotation.
    const math::Quaternion inverse{-owner.transform.worldRotation.x,
                                   -owner.transform.worldRotation.y,
                                   -owner.transform.worldRotation.z,
                                    owner.transform.worldRotation.w};
    const Double3 rotated = Rotate(inverse, worldDelta);
    return ToVector3({rotated.x / owner.transform.worldScale.x,
                      rotated.y / owner.transform.worldScale.y,
                      rotated.z / owner.transform.worldScale.z}, out);
}

void HashBytes(uint64_t& hash, const void* bytes, std::size_t size)
{
    const auto* values = static_cast<const uint8_t*>(bytes);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= values[index];
        hash *= FNV_PRIME;
    }
}

template<typename T>
void HashValue(uint64_t& hash, const T& value)
{
    HashBytes(hash, &value, sizeof(value));
}

void HashString(uint64_t& hash, const std::string& value)
{
    HashValue(hash, static_cast<uint64_t>(value.size()));
    HashBytes(hash, value.data(), value.size());
}

void HashVector(uint64_t& hash, const math::Vector3& value)
{
    HashValue(hash, std::bit_cast<uint32_t>(value.x));
    HashValue(hash, std::bit_cast<uint32_t>(value.y));
    HashValue(hash, std::bit_cast<uint32_t>(value.z));
}

void HashQuaternion(uint64_t& hash, const math::Quaternion& value)
{
    HashValue(hash, std::bit_cast<uint32_t>(value.x));
    HashValue(hash, std::bit_cast<uint32_t>(value.y));
    HashValue(hash, std::bit_cast<uint32_t>(value.z));
    HashValue(hash, std::bit_cast<uint32_t>(value.w));
}

} /// @note namespace

bool ResolveControlPointBindings(Scene& scene,
                                 EntityID ownerId,
                                 std::span<const ControlPointBinding> bindings,
                                 bool requireTargetPointMarker,
                                 std::vector<ResolvedControlPoint>& out,
                                 ControlPointResolveStatus& status,
                                 uint64_t& signature)
{
    status = ControlPointResolveStatus::SUCCESS;
    signature = 0;
    GameObject* owner = scene.GetGameObject(ownerId);
    if (!owner || !scene.IsValid(ownerId)) {
        status = ControlPointResolveStatus::INVALID_OWNER;
        return false;
    }
    if (!IsFiniteTransform(owner->transform, true)) {
        status = ControlPointResolveStatus::INVALID_OWNER_TRANSFORM;
        return false;
    }
    if (bindings.size() > MAX_CONTROL_POINT_BINDINGS) {
        status = ControlPointResolveStatus::TOO_MANY_BINDINGS;
        return false;
    }

    std::unordered_set<uint32_t> bindingIds;
    bindingIds.reserve(bindings.size());
    std::unordered_set<uint64_t> targetIds;
    if (requireTargetPointMarker) targetIds.reserve(bindings.size());
    std::unordered_map<std::string, GameObject*> slowGuidLookup;
    bool needsGuidLookup = false;
    const uint64_t sceneGeneration = scene.GetRenderSceneGeneration();

    for (const ControlPointBinding& binding : bindings) {
        if (binding.stableId == 0 || binding.stableId > static_cast<uint32_t>((std::numeric_limits<int32_t>::max)())) {
            status = ControlPointResolveStatus::INVALID_BINDING_ID;
            return false;
        }
        if (!bindingIds.insert(binding.stableId).second) {
            status = ControlPointResolveStatus::DUPLICATE_BINDING_ID;
            return false;
        }
        if (!binding.target) {
            status = ControlPointResolveStatus::NULL_REFERENCE;
            return false;
        }
        const ObjectReference& reference = *binding.target;
        if (reference.guid.empty()) {
            reference.id = EntityID::INVALID;
            reference.sceneGeneration = sceneGeneration;
            status = ControlPointResolveStatus::NULL_REFERENCE;
            return false;
        }
        const GameObject* cached = reference.sceneGeneration == sceneGeneration && scene.IsValid(reference.id)
            ? scene.GetGameObject(reference.id) : nullptr;
        if (!cached || cached->instanceId != reference.guid)
            needsGuidLookup = true;
        if (needsGuidLookup)
            slowGuidLookup.try_emplace(reference.guid, nullptr);
    }

    if (needsGuidLookup && !slowGuidLookup.empty()) {
        for (GameObject& object : scene.GameObjects()) {
            const auto found = slowGuidLookup.find(object.instanceId);
            if (found != slowGuidLookup.end() && !found->second)
                found->second = &object;
        }
    }

    std::vector<ResolvedControlPoint> resolved;
    resolved.reserve(bindings.size());
    uint64_t currentSignature = FNV_OFFSET;
    HashValue(currentSignature, sceneGeneration);
    HashValue(currentSignature, ownerId.index);
    HashValue(currentSignature, ownerId.generation);
    HashValue(currentSignature, static_cast<uint64_t>(bindings.size()));

    for (const ControlPointBinding& binding : bindings) {
        const ObjectReference& reference = *binding.target;
        if (reference.guid.empty()) {
            reference.id = EntityID::INVALID;
            reference.sceneGeneration = sceneGeneration;
            status = ControlPointResolveStatus::NULL_REFERENCE;
            return false;
        }
        GameObject* target = nullptr;
        if (reference.sceneGeneration == sceneGeneration && scene.IsValid(reference.id)) {
            GameObject* cached = scene.GetGameObject(reference.id);
            if (cached && cached->instanceId == reference.guid) target = cached;
        }
        if (!target) {
            const auto found = slowGuidLookup.find(reference.guid);
            if (found != slowGuidLookup.end()) target = found->second;
        }
        if (!target) {
            reference.id = EntityID::INVALID;
            reference.sceneGeneration = sceneGeneration;
            status = ControlPointResolveStatus::UNRESOLVED_REFERENCE;
            return false;
        }
        reference.id = target->GetID();
        reference.sceneGeneration = sceneGeneration;
        if (!target->activeInHierarchy()) {
            status = ControlPointResolveStatus::INACTIVE_TARGET;
            return false;
        }
        if (requireTargetPointMarker) {
            const EntityID targetId = target->GetID();
            const uint64_t key = (static_cast<uint64_t>(targetId.generation) << 32) | targetId.index;
            if (!targetIds.insert(key).second) {
                status = ControlPointResolveStatus::DUPLICATE_TARGET;
                return false;
            }
        }
        if (!IsFiniteTransform(target->transform, true)) {
            status = ControlPointResolveStatus::INVALID_TARGET_TRANSFORM;
            return false;
        }

        uint64_t targetLifetime = 0;
        if (requireTargetPointMarker) {
            if (!target->GetComponent<TargetPointComponent>()) {
                status = ControlPointResolveStatus::MISSING_TARGET_POINT;
                return false;
            }
            targetLifetime = scene.GetComponentLifetime<TargetPointComponent>(target->GetID());
            if (targetLifetime == 0) {
                status = ControlPointResolveStatus::MISSING_TARGET_POINT;
                return false;
            }
        }

        ResolvedControlPoint point;
        point.stableId = binding.stableId;
        point.targetId = target->GetID();
        point.targetLifetime = targetLifetime;
        point.worldPosition = target->transform.worldPosition;
        point.worldRotation = target->transform.worldRotation;
        point.worldScale = target->transform.worldScale;
        if (!OwnerLocalPosition(*owner, *target, point.ownerLocalPosition)) {
            status = ControlPointResolveStatus::INVALID_TARGET_TRANSFORM;
            return false;
        }
        resolved.push_back(point);

        HashValue(currentSignature, binding.stableId);
        HashString(currentSignature, reference.guid);
        HashValue(currentSignature, point.targetId.index);
        HashValue(currentSignature, point.targetId.generation);
        HashValue(currentSignature, point.targetLifetime);
        HashVector(currentSignature, point.ownerLocalPosition);
        HashVector(currentSignature, point.worldPosition);
        HashQuaternion(currentSignature, point.worldRotation);
        HashVector(currentSignature, point.worldScale);
    }

    if (currentSignature == 0) currentSignature = 1;
    out = std::move(resolved);
    signature = currentSignature;
    return true;
}

} /// @note namespace fbzz::scene
