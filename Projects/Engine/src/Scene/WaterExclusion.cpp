/// @file    WaterExclusion.cpp
/// @brief   Resolve authored Water exclusion boxes into shared immutable numeric snapshots.
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include <Engine/Scene/WaterExclusion.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/WaterExclusionComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/Transform.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fbzz::scene {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

struct ExclusionSource {
    WaterExclusionBox box;
    std::vector<EntityID> waterOwners;
    std::uint64_t signature = kFnvOffset;
    bool excludeAll = true;
    bool valid = true;
};

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Finite(const math::Quaternion& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z) && std::isfinite(value.w);
}

void HashU32(std::uint64_t& hash, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        hash ^= static_cast<std::uint8_t>(value >> (i * 8));
        hash *= kFnvPrime;
    }
}

void HashU64(std::uint64_t& hash, std::uint64_t value)
{
    HashU32(hash, static_cast<std::uint32_t>(value));
    HashU32(hash, static_cast<std::uint32_t>(value >> 32));
}

void HashFloat(std::uint64_t& hash, float value)
{
    HashU32(hash, std::bit_cast<std::uint32_t>(value));
}

void HashVector(std::uint64_t& hash, const math::Vector3& value)
{
    HashFloat(hash, value.x);
    HashFloat(hash, value.y);
    HashFloat(hash, value.z);
}

void HashTransform(std::uint64_t& hash, const Transform& transform)
{
    HashVector(hash, transform.worldPosition);
    HashFloat(hash, transform.worldRotation.x);
    HashFloat(hash, transform.worldRotation.y);
    HashFloat(hash, transform.worldRotation.z);
    HashFloat(hash, transform.worldRotation.w);
    HashVector(hash, transform.worldScale);
}

void HashEntity(std::uint64_t& hash, EntityID entity)
{
    HashU32(hash, entity.index);
    HashU32(hash, entity.generation);
}

bool ValidTransform(const Transform& transform)
{
    const double rotationLength = transform.worldRotation.Length();
    return Finite(transform.worldPosition) && Finite(transform.worldScale)
        && Finite(transform.worldRotation)
        && std::abs(transform.worldScale.x) > 1.0e-6f
        && std::abs(transform.worldScale.y) > 1.0e-6f
        && std::abs(transform.worldScale.z) > 1.0e-6f
        && std::isfinite(rotationLength) && std::abs(rotationLength - 1.0) <= 0.001;
}

bool ValidSize(const math::Vector3& size)
{
    return Finite(size) && size.x > 0.0f && size.y > 0.0f && size.z > 0.0f;
}

void ResolveSourceWaterOwners(Scene& scene, WaterExclusionComponent& component,
    ExclusionSource& source)
{
    std::unordered_set<std::uint64_t> seen;
    for (const EntityRef& reference : component.waters) {
        if (!reference.IsAssigned()) continue;
        HashU64(source.signature, reference.guid.size());
        GameObject* target = reference.Resolve(scene);
        if (!target) continue;
        const EntityID targetID = target->GetID();
        WaterComponent* targetWater = scene.GetComponent<WaterComponent>(targetID);
        if (!targetWater) continue;
        const EntityID ownerID = targetWater->zoneOwner.IsValid()
            ? targetWater->zoneOwner : targetID;
        if (seen.insert((static_cast<std::uint64_t>(ownerID.generation) << 32) | ownerID.index).second)
            source.waterOwners.push_back(ownerID);
        for (const unsigned char character : reference.guid) {
            source.signature ^= character;
            source.signature *= kFnvPrime;
        }
        HashEntity(source.signature, ownerID);
    }
}

std::uint64_t SnapshotSignature(const std::vector<const ExclusionSource*>& sources)
{
    std::uint64_t signature = kFnvOffset;
    HashU64(signature, sources.size());
    for (const ExclusionSource* source : sources)
        HashU64(signature, source->signature);
    return signature == 0 ? 1 : signature;
}

} /// @note namespace

bool WaterExclusionBox::Contains(const math::Vector3& worldPoint) const
{
    if (!Finite(worldPoint)) return false;
    const math::Vector3 rotated = inverseWorldRotation * (worldPoint - worldPosition);
    const math::Vector3 local{
        rotated.x * inverseWorldScale.x,
        rotated.y * inverseWorldScale.y,
        rotated.z * inverseWorldScale.z
    };
    return Finite(local) && std::abs(local.x) <= halfExtent.x
        && std::abs(local.y) <= halfExtent.y && std::abs(local.z) <= halfExtent.z;
}

const WaterExclusionSnapshot* WaterSurfaceExclusions(
    const WaterComponent& water, WaterSurfaceReadView view)
{
    if (view == WaterSurfaceReadView::SIMULATION && water.simulationReadEnabled
        && water.simulationSurfacePublished)
        return water.simulationExclusionSnapshot.get();
    return water.exclusionSnapshot.get();
}

bool IsWaterPointExcluded(
    const WaterExclusionSnapshot* snapshot, const math::Vector3& worldPoint)
{
    if (!snapshot) return false;
    if (!snapshot->valid) return true;
    for (const WaterExclusionBox& box : snapshot->boxes)
        if (box.Contains(worldPoint)) return true;
    return false;
}

void PrepareWaterExclusionSnapshots(Scene& scene)
{
    std::vector<ExclusionSource> sources;
    sources.reserve(scene.GetEntities<WaterExclusionComponent>().size());
    for (const EntityID sourceID : scene.GetEntities<WaterExclusionComponent>()) {
        WaterExclusionComponent* component = scene.GetComponent<WaterExclusionComponent>(sourceID);
        GameObject* go = scene.GetGameObject(sourceID);
        if (!component || !go) continue;
        if (!component->enabled || !go->activeInHierarchy()) {
            component->diagnostic = component->enabled ? "Inactive GameObject" : "Disabled";
            continue;
        }

        ExclusionSource source;
        source.excludeAll = component->excludeAll;
        source.signature = kFnvOffset;
        HashEntity(source.signature, sourceID);
        HashU32(source.signature, component->enabled ? 1u : 0u);
        HashU32(source.signature, component->excludeAll ? 1u : 0u);
        HashVector(source.signature, component->size);
        HashTransform(source.signature, go->transform);

        source.valid = ValidTransform(go->transform) && ValidSize(component->size);
        if (source.valid) {
            source.box.worldPosition = go->transform.worldPosition;
            source.box.inverseWorldRotation = go->transform.worldRotation.Inverse();
            source.box.inverseWorldScale = {
                1.0f / go->transform.worldScale.x,
                1.0f / go->transform.worldScale.y,
                1.0f / go->transform.worldScale.z
            };
            source.box.halfExtent = component->size * 0.5f;
            source.valid = Finite(source.box.inverseWorldScale)
                && Finite(source.box.inverseWorldRotation)
                && Finite(source.box.halfExtent);
        }
        if (!component->excludeAll) ResolveSourceWaterOwners(scene, *component, source);
        component->diagnostic = !source.valid
            ? "Invalid box transform or size; matching Water surfaces are masked."
            : (component->excludeAll || !source.waterOwners.empty()
                ? "Ready" : "No matching Water reference.");
        sources.push_back(std::move(source));
    }

    std::vector<EntityID> ownerIDs;
    ownerIDs.reserve(scene.GetEntities<WaterComponent>().size());
    std::unordered_set<std::uint64_t> seenOwners;
    for (const EntityID waterID : scene.GetEntities<WaterComponent>()) {
        WaterComponent* water = scene.GetComponent<WaterComponent>(waterID);
        if (!water) continue;
        const EntityID ownerID = water->zoneOwner.IsValid() ? water->zoneOwner : waterID;
        if (seenOwners.insert((static_cast<std::uint64_t>(ownerID.generation) << 32) | ownerID.index).second)
            ownerIDs.push_back(ownerID);
    }

    std::unordered_map<std::uint64_t, std::shared_ptr<const WaterExclusionSnapshot>> snapshotsBySignature;
    snapshotsBySignature.reserve(ownerIDs.size());
    for (const EntityID ownerID : ownerIDs) {
        WaterComponent* owner = scene.GetComponent<WaterComponent>(ownerID);
        if (!owner) continue;
        std::vector<const ExclusionSource*> selected;
        selected.reserve(sources.size());
        for (const ExclusionSource& source : sources) {
            if (source.excludeAll || std::find(source.waterOwners.begin(), source.waterOwners.end(), ownerID)
                != source.waterOwners.end())
                selected.push_back(&source);
        }
        const std::uint64_t signature = SnapshotSignature(selected);
        if (owner->exclusionSnapshotAttempted
            && owner->exclusionSnapshotAttemptSignature == signature) continue;
        if (selected.empty()) {
            owner->exclusionSnapshot.reset();
            owner->exclusionSnapshotAttemptSignature = signature;
            owner->exclusionSnapshotAttempted = true;
            continue;
        }

        if (const auto cached = snapshotsBySignature.find(signature); cached != snapshotsBySignature.end()) {
            owner->exclusionSnapshot = cached->second;
            owner->exclusionSnapshotAttemptSignature = signature;
            owner->exclusionSnapshotAttempted = true;
            continue;
        }

        auto snapshot = std::make_shared<WaterExclusionSnapshot>();
        snapshot->signature = signature;
        snapshot->boxes.reserve(selected.size());
        for (const ExclusionSource* source : selected) {
            if (!source->valid) {
                snapshot->valid = false;
                continue;
            }
            snapshot->boxes.push_back(source->box);
        }
        owner->exclusionSnapshot = snapshot->boxes.empty() && snapshot->valid
            ? std::shared_ptr<const WaterExclusionSnapshot>{}
            : std::shared_ptr<const WaterExclusionSnapshot>{std::move(snapshot)};
        if (owner->exclusionSnapshot)
            snapshotsBySignature.emplace(signature, owner->exclusionSnapshot);
        owner->exclusionSnapshotAttemptSignature = signature;
        owner->exclusionSnapshotAttempted = true;
    }

    for (const EntityID waterID : scene.GetEntities<WaterComponent>()) {
        WaterComponent* water = scene.GetComponent<WaterComponent>(waterID);
        if (!water || !water->zoneOwner.IsValid()) continue;
        WaterComponent* owner = scene.GetComponent<WaterComponent>(water->zoneOwner);
        if (owner) {
            water->exclusionSnapshot = owner->exclusionSnapshot;
            water->exclusionSnapshotAttemptSignature = 0;
            water->exclusionSnapshotAttempted = false;
        } else {
            water->exclusionSnapshot.reset();
            water->exclusionSnapshotAttempted = false;
            water->exclusionSnapshotAttemptSignature = 0;
        }
    }
}

} /// @note namespace fbzz::scene
