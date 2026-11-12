/// @file    FiberContacts.hpp
/// @brief   Collider 接触の個体別履歴と草チャンクへの空間抽出。
/// @author  Hasegawa Jin
/// @date    2026-11-11
#pragma once
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/FiberInteractorComponent.hpp>
#include <Engine/Scene/Systems/ColliderSync.hpp>
#include <Graphics/Effects/RenderFiberInput.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

namespace fbzz::scene {
namespace {
constexpr uint32_t FIBER_CONTACT_LIMIT = 32;
constexpr float FIBER_CONTACT_CELL_SIZE = 8.0f;

struct FiberContactSample {
    math::Vector4 m_center{}, m_time{}, m_extent{}, m_rotation{0, 0, 0, 1};
};

math::Vector3 FiberContactPosition(const FiberContactSample& sample)
{
    return {sample.m_center.x, sample.m_center.y, sample.m_center.z};
}

float FiberContactRadius(const FiberContactSample& sample)
{
    const math::Vector3 extent{sample.m_extent.x, sample.m_extent.y, sample.m_extent.z};
    if (sample.m_time.w == 1.0f) return sample.m_center.w + extent.Length();
    return sample.m_center.w;
}

bool FiberContactFinite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

/// @note Reads authored settings without modifying Physics resources, including while Edit is paused.
bool ReadFiberContact(GameObject& go, const FiberInteractorComponent& actor, FiberContactSample& sample)
{
    if (!std::isfinite(actor.m_strength) || !std::isfinite(actor.m_recoverySeconds)
        || actor.m_strength <= 0.0f) return false;
    const auto scale = go.transform.worldScale;
    const math::Vector3 absolute{std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)};
    const auto rotation = go.transform.worldRotation;
    if (!FiberContactFinite(scale) || !std::isfinite(rotation.Length())
        || std::abs(rotation.Length() - 1.0f) > 0.001f) return false;
    math::Vector3 center = go.transform.worldPosition;
    sample.m_time = {Time::time, std::clamp(actor.m_strength, 0.0f, 1.0f),
        std::clamp(actor.m_recoverySeconds, 0.05f, 30.0f), 0.0f};
    bool hasCollider = false, resolved = false;
    const auto tryShape = [&]<class T>(T* collider) {
        if (!collider) return;
        hasCollider = true;
        if (resolved || !collider->enabled) return;
        resolved = true;
        center = ColliderWorldCenter(go, *collider);
        if constexpr (std::is_same_v<T, CapsuleColliderComponent>) {
            if (collider->halfHeight < 0.0f) return;
            sample.m_center.w = collider->radius * std::max(absolute.x, absolute.z);
            const auto axis = rotation * math::Vector3{0, collider->halfHeight * absolute.y, 0};
            sample.m_extent = {axis.x, axis.y, axis.z, 0};
            sample.m_time.w = 1.0f;
            sample.m_rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
        } else if constexpr (std::is_same_v<T, SphereColliderComponent>) {
            sample.m_center.w = collider->radius * std::max({absolute.x, absolute.y, absolute.z});
        } else {
            const math::Vector3 half{collider->size.x * absolute.x * 0.5f,
                collider->size.y * absolute.y * 0.5f, collider->size.z * absolute.z * 0.5f};
            if (half.x <= 0 || half.y <= 0 || half.z <= 0) { sample.m_center.w = 0; return; }
            sample.m_extent = {half.x, half.y, half.z, 0};
            sample.m_center.w = half.Length();
            sample.m_time.w = 2.0f;
            if constexpr (std::is_same_v<T, BoxColliderComponent>)
                sample.m_rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
        }
    };
    if (actor.m_useCollider) {
        tryShape(go.GetComponent<CapsuleColliderComponent>());
        tryShape(go.GetComponent<SphereColliderComponent>());
        tryShape(go.GetComponent<BoxColliderComponent>());
        tryShape(go.GetComponent<AabbColliderComponent>());
    }
    if (!resolved) {
        if (hasCollider || !std::isfinite(actor.m_radius)) return false;
        sample.m_center.w = std::clamp(actor.m_radius, 0.01f, 10.0f);
    }
    sample.m_center.x = center.x; sample.m_center.y = center.y; sample.m_center.z = center.z;
    const math::Vector3 extent{sample.m_extent.x, sample.m_extent.y, sample.m_extent.z};
    return FiberContactFinite(center) && FiberContactFinite(extent)
        && std::isfinite(FiberContactRadius(sample)) && sample.m_center.w > 0.0f;
}

struct FiberActorContacts {
    std::array<FiberContactSample, FIBER_CONTACT_LIMIT> m_samples{};
    FiberContactSample m_last{};
    uint64_t m_seen = UINT64_MAX;
    uint32_t m_cursor = 0, m_latest = 0;
    float m_travelSinceStamp = 0;
};

struct FiberContactCell {
    std::array<int, 3> m_key{};
    size_t m_sample = 0;
    bool operator<(const FiberContactCell& other) const
    { return m_key < other.m_key || (m_key == other.m_key && m_sample < other.m_sample); }
};

struct FiberContactIndex {
    std::vector<FiberContactCell> m_cells;
    std::vector<size_t> m_large;
};

struct FiberContactSurface {
    math::Vector3 m_center{}, m_previousCenter{};
    float m_radius = 0, m_previousRadius = 0;
    uint64_t m_frame = UINT64_MAX;
};

struct FiberContactCache {
    std::map<uint64_t, FiberActorContacts> m_actors;
    std::vector<FiberContactSample> m_current, m_previous;
    FiberContactIndex m_currentIndex, m_previousIndex;
    std::map<std::pair<uint64_t, uint64_t>, FiberContactSurface> m_surfaces;
    std::vector<size_t> m_candidates;
    std::vector<std::pair<float, size_t>> m_ranked;
    uint64_t m_frame = UINT64_MAX;
    float m_time = 0, m_previousTime = 0;
};

/// @note Bounds outside the integer grid, or spanning too many cells, use a bounded full scan instead.
bool FiberContactCells(const math::Vector3& center, float radius, int budget,
    std::array<int, 3>& low, std::array<int, 3>& high)
{
    if (!FiberContactFinite(center) || !std::isfinite(radius) || radius < 0) return false;
    const float coordinates[]{center.x, center.y, center.z};
    int64_t count = 1;
    for (int axis = 0; axis < 3; ++axis) {
        const double first = std::floor((static_cast<double>(coordinates[axis]) - radius) / FIBER_CONTACT_CELL_SIZE);
        const double last = std::floor((static_cast<double>(coordinates[axis]) + radius) / FIBER_CONTACT_CELL_SIZE);
        if (first < INT32_MIN || last >= INT32_MAX || last - first + 1 > budget) return false;
        low[axis] = static_cast<int>(first); high[axis] = static_cast<int>(last);
        count *= static_cast<int64_t>(high[axis]) - low[axis] + 1;
        if (count > budget) return false;
    }
    return true;
}

void BuildFiberContactIndex(FiberContactIndex& index, const std::vector<FiberContactSample>& samples)
{
    index.m_cells.clear(); index.m_large.clear();
    for (size_t i = 0; i < samples.size(); ++i) {
        std::array<int, 3> low{}, high{};
        if (!FiberContactCells(FiberContactPosition(samples[i]), FiberContactRadius(samples[i]), 64, low, high)) {
            index.m_large.push_back(i);
            continue;
        }
        for (int x = low[0]; x <= high[0]; ++x) for (int y = low[1]; y <= high[1]; ++y)
            for (int z = low[2]; z <= high[2]; ++z) index.m_cells.push_back({{x, y, z}, i});
    }
    std::sort(index.m_cells.begin(), index.m_cells.end());
}

/// @note Translation and angular travel are sampled at half the smallest shape thickness; long jumps never stamp a world-spanning trail.
void StampFiberContact(FiberActorContacts& state, const FiberContactSample& current, bool consecutive)
{
    float thickness = current.m_center.w;
    if (current.m_time.w == 2.0f)
        thickness = std::min({current.m_extent.x, current.m_extent.y, current.m_extent.z});
    const float spacing = std::max(thickness * 0.5f, 0.005f);
    const auto from = FiberContactPosition(state.m_last), to = FiberContactPosition(current);
    const auto& oldRotation = state.m_last.m_rotation;
    const auto& newRotation = current.m_rotation;
    const math::Quaternion oldQ{oldRotation.x, oldRotation.y, oldRotation.z, oldRotation.w};
    const math::Quaternion newQ{newRotation.x, newRotation.y, newRotation.z, newRotation.w};
    const float rotationDot = std::clamp(std::abs(math::Quaternion::Dot(oldQ, newQ)), 0.0f, 1.0f);
    /// @note Identical unit quaternions can have dot < 1 after float rounding; do not accumulate phantom angular travel while stationary.
    const float angle = rotationDot >= 1.0f - 1.0e-6f ? 0.0f : 2.0f * std::acos(rotationDot);
    const math::Vector3 oldExtent{state.m_last.m_extent.x, state.m_last.m_extent.y, state.m_last.m_extent.z};
    const math::Vector3 newExtent{current.m_extent.x, current.m_extent.y, current.m_extent.z};
    const float travel = (to - from).Length() + (newExtent - oldExtent).Length() + angle * FiberContactRadius(current);
    const bool compatible = consecutive && state.m_last.m_time.w == current.m_time.w
        && std::abs(state.m_last.m_center.w - current.m_center.w) < spacing;
    const int steps = compatible && std::isfinite(travel) && travel <= spacing * 16.0f
        ? std::max(1, static_cast<int>(std::ceil(travel / spacing))) : 1;
    state.m_travelSinceStamp += travel;
    if (compatible && state.m_travelSinceStamp < spacing) {
        state.m_samples[state.m_latest] = current;
    } else {
        for (int step = 1; step <= steps; ++step) {
            auto sample = current;
            if (steps > 1 && step < steps) {
                const float t = static_cast<float>(step) / static_cast<float>(steps);
                const auto center = from + (to - from) * t;
                sample.m_center = {center.x, center.y, center.z, current.m_center.w};
                const auto extent = oldExtent + (newExtent - oldExtent) * t;
                sample.m_extent = {extent.x, extent.y, extent.z, 0};
                const auto q = math::Quaternion::Slerp(oldQ, newQ, t);
                sample.m_rotation = {q.x, q.y, q.z, q.w};
                if (current.m_time.w == 1.0f) {
                    const float halfHeight = oldExtent.Length() + (newExtent.Length() - oldExtent.Length()) * t;
                    const auto axis = q * math::Vector3{0, halfHeight, 0};
                    sample.m_extent = {axis.x, axis.y, axis.z, 0};
                }
            }
            state.m_latest = state.m_cursor++ % FIBER_CONTACT_LIMIT;
            state.m_samples[state.m_latest] = sample;
        }
        state.m_travelSinceStamp = 0;
    }
    state.m_last = current;
    state.m_seen = Time::frameCount;
}

/// @note One immutable current/previous snapshot is shared by every view and shadow pass in the frame.
void UpdateFiberContacts(FiberContactCache& cache, Scene& scene)
{
    if (cache.m_frame == Time::frameCount) return;
    if (!std::isfinite(Time::time)) { cache = {}; return; }
    if (Time::time < cache.m_time || (cache.m_frame != UINT64_MAX && Time::frameCount < cache.m_frame)) cache = {};
    const bool consecutive = cache.m_frame != UINT64_MAX && Time::frameCount == cache.m_frame + 1;
    cache.m_previousTime = cache.m_time;
    cache.m_time = Time::time; cache.m_frame = Time::frameCount;
    std::swap(cache.m_previous, cache.m_current);
    std::swap(cache.m_previousIndex, cache.m_currentIndex);
    cache.m_current.clear();
    for (EntityID id : scene.GetEntities<FiberInteractorComponent>()) {
        auto* go = scene.GetGameObject(id);
        if (!go || !go->activeInHierarchy()) continue;
        const auto* actor = go->GetComponent<FiberInteractorComponent>();
        if (!actor || !actor->m_enabled) continue;
        FiberContactSample sample;
        if (!ReadFiberContact(*go, *actor, sample)) continue;
        const uint64_t key = (static_cast<uint64_t>(id.generation) << 32) | id.index;
        auto& state = cache.m_actors[key];
        StampFiberContact(state, sample, consecutive && state.m_seen + 1 == Time::frameCount);
    }
    for (auto it = cache.m_actors.begin(); it != cache.m_actors.end();) {
        bool alive = false;
        for (const auto& sample : it->second.m_samples) {
            const float age = Time::time - sample.m_time.x;
            if (sample.m_center.w <= 0 || age < 0 || age >= sample.m_time.z) continue;
            cache.m_current.push_back(sample); alive = true;
        }
        if (!alive) it = cache.m_actors.erase(it); else ++it;
    }
    BuildFiberContactIndex(cache.m_currentIndex, cache.m_current);
    if (!consecutive) {
        cache.m_previous = cache.m_current;
        cache.m_previousTime = cache.m_time;
        cache.m_previousIndex = cache.m_currentIndex;
    }
    std::erase_if(cache.m_surfaces, [&](const auto& item) {
        return Time::frameCount > item.second.m_frame && Time::frameCount - item.second.m_frame > 2;
    });
}

void SelectFiberContactSide(FiberContactCache& cache, const std::vector<FiberContactSample>& samples,
    const FiberContactIndex& index, const math::Vector3& center, float radius, bool hasBounds,
    float time, uint32_t first, renderer::FiberContactCB& output)
{
    auto& candidates = cache.m_candidates;
    auto& ranked = cache.m_ranked;
    candidates.clear(); ranked.clear();
    std::array<int, 3> low{}, high{};
    if (hasBounds && FiberContactCells(center, radius, 512, low, high)) {
        candidates.insert(candidates.end(), index.m_large.begin(), index.m_large.end());
        for (int x = low[0]; x <= high[0]; ++x) for (int y = low[1]; y <= high[1]; ++y)
            for (int z = low[2]; z <= high[2]; ++z) {
                const std::array<int, 3> key{x, y, z};
                auto found = std::lower_bound(index.m_cells.begin(), index.m_cells.end(), FiberContactCell{key, 0});
                for (; found != index.m_cells.end() && found->m_key == key; ++found) candidates.push_back(found->m_sample);
            }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    } else {
        for (size_t i = 0; i < samples.size(); ++i) candidates.push_back(i);
    }
    for (size_t i : candidates) {
        const auto& sample = samples[i];
        const float reach = FiberContactRadius(sample);
        const float distanceSq = (FiberContactPosition(sample) - center).LengthSq();
        if (hasBounds && distanceSq > (reach + radius) * (reach + radius)) continue;
        const float phase = std::clamp((time - sample.m_time.x) / sample.m_time.z, 0.0f, 1.0f);
        const float distance = hasBounds ? std::max(0.0f, std::sqrt(distanceSq) - radius) : 0.0f;
        const float score = sample.m_time.y * (1 - phase) * (1 - phase) / (1 + distance / std::max(reach, 0.01f));
        ranked.emplace_back(-score, i);
    }
    const size_t count = std::min<size_t>(FIBER_CONTACT_LIMIT, ranked.size());
    if (ranked.size() > count) {
        std::partial_sort(ranked.begin(), ranked.begin() + count, ranked.end());
        output.m_padding[first == 0 ? 0 : 1] = static_cast<uint32_t>(ranked.size() - count);
        ranked.resize(count);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    for (uint32_t slot = 0; slot < count; ++slot) {
        const auto& sample = samples[ranked[slot].second];
        output.m_centers[first + slot] = sample.m_center;
        output.m_times[first + slot] = sample.m_time;
        output.m_extents[first + slot] = sample.m_extent;
        output.m_rotations[first + slot] = sample.m_rotation;
    }
    (first == 0 ? output.m_count : output.m_previousCount) = static_cast<uint32_t>(count);
}

/// @note Current and previous selections use their own deformed surface bounds, including when a grass owner moves.
renderer::FiberContactCB SelectFiberContacts(FiberContactCache& cache, const renderer::RenderFiberInput& input)
{
    renderer::FiberContactCB output;
    if (input.material.m_planarShading <= 0 || (cache.m_current.empty() && cache.m_previous.empty())) return output;
    /// @note Terrain patches share their owner's render identity; the geometry generation separates their motion bounds.
    const auto geometry = input.blades.m_blades.IsValid()
        ? (static_cast<uint64_t>(input.blades.m_blades.gen) << 32) | input.blades.m_blades.id
        : (static_cast<uint64_t>(input.vertices.gen) << 32) | input.vertices.id;
    auto& surface = cache.m_surfaces[{input.identity, geometry}];
    if (surface.m_frame != Time::frameCount) {
        const bool consecutive = surface.m_frame != UINT64_MAX && surface.m_frame + 1 == Time::frameCount;
        surface.m_previousCenter = consecutive ? surface.m_center : input.boundsCenter;
        surface.m_previousRadius = consecutive ? surface.m_radius : input.boundsRadius;
        surface.m_center = input.boundsCenter; surface.m_radius = input.boundsRadius;
        surface.m_frame = Time::frameCount;
    }
    SelectFiberContactSide(cache, cache.m_current, cache.m_currentIndex, input.boundsCenter,
        input.boundsRadius, input.hasBounds, cache.m_time, 0, output);
    SelectFiberContactSide(cache, cache.m_previous, cache.m_previousIndex, surface.m_previousCenter,
        surface.m_previousRadius, input.hasBounds, cache.m_previousTime, FIBER_CONTACT_LIMIT, output);
    return output;
}
} /// @note namespace
} /// @note namespace fbzz::scene
