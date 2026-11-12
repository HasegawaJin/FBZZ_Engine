/// @file    Waterfall.hpp
/// @brief   Shared river cross-sections and procedural waterfall geometry.
/// @author  Hasegawa Jin
/// @date    2026-11-11
#pragma once
#include <Engine/Api.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <functional>

namespace fbzz::scene {
class MeshBuilder;
class Scene;
class GameObject;
struct WaterSplineSnapshot;

struct WaterfallSection {
    math::Vector3 left, right;
    math::Vector3 insideLeft, insideRight;
    math::Vector3 velocity;
    float depth = 0.0f;
    bool operator==(const WaterfallSection&) const = default;
};

struct WaterfallGeometry {
    WaterfallSection source, receiver;
    float breakup = 1.0f;
    bool rockAttached = false;
    bool operator==(const WaterfallGeometry&) const = default;
};

/// @brief Sample the original bank endpoints and a band inside the same Spline River.
/// @note Reversing the connection direction also reverses the bank order.
[[nodiscard]] FBZZ_ENGINE_API bool GetWaterfallRiverSection(const WaterSplineSnapshot& snapshot,
    bool atStart, bool incoming, WaterfallSection& out);

/// @brief Build a bounded curtain, impact foam, and optional world-space effect anchors.
/// @param ground Returns the highest terrain surface at the query XZ, or negative infinity outside terrain.
/// @return False when the connections are invalid or the sampled curtain cannot clear terrain.
[[nodiscard]] FBZZ_ENGINE_API bool BuildWaterfallGeometry(const WaterfallGeometry& geometry,
    const std::function<float(const math::Vector3&)>& ground, MeshBuilder& out,
    math::Vector3* midFall = nullptr, std::uint32_t* farIndexStart = nullptr);

/// @brief Resolve the authoring water references against the current scene.
[[nodiscard]] FBZZ_ENGINE_API bool ResolveWaterfallGeometry(Scene& scene, GameObject& owner, WaterfallGeometry& out);

} /// @note namespace fbzz::scene
