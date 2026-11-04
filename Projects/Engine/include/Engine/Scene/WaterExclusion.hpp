/// @file    WaterExclusion.hpp
/// @brief   Immutable numeric oriented-box exclusions shared by water queries and rendering.
/// @author  Hasegawa Jin
/// @date    2026-10-09
#pragma once
#include <Engine/Api.hpp>

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <vector>

namespace fbzz::scene {

class Scene;
struct WaterComponent;
enum class WaterSurfaceReadView : std::uint8_t;

/// @brief One oriented exclusion box in world space, stored as a numeric world-to-local transform.
struct WaterExclusionBox {
    math::Vector3 worldPosition = math::Vector3::ZERO;
    math::Quaternion inverseWorldRotation = math::Quaternion::Identity();
    math::Vector3 inverseWorldScale = math::Vector3::ONE;
    math::Vector3 halfExtent = math::Vector3::ZERO;

    /// @brief Test a world point against the box using the captured signed world scale.
    [[nodiscard]] FBZZ_ENGINE_API bool Contains(const math::Vector3& worldPoint) const;
};

/// @brief Per-water immutable exclusion set captured at a Scene read boundary.
struct WaterExclusionSnapshot {
    std::vector<WaterExclusionBox> boxes;
    std::uint64_t signature = 0;
    bool valid = true;
};

/// @brief Select the LIVE_QUERY or fixed-step exclusion snapshot for a Water consumer.
[[nodiscard]] FBZZ_ENGINE_API const WaterExclusionSnapshot* WaterSurfaceExclusions(
    const WaterComponent& water, WaterSurfaceReadView view);

/// @brief Return true when a captured exclusion set rejects the world point.
[[nodiscard]] FBZZ_ENGINE_API bool IsWaterPointExcluded(
    const WaterExclusionSnapshot* snapshot, const math::Vector3& worldPoint);

/// @brief Build/reuse numeric exclusion snapshots after water Zone ownership is resolved.
FBZZ_ENGINE_API void PrepareWaterExclusionSnapshots(Scene& scene);

} /// @note namespace fbzz::scene
