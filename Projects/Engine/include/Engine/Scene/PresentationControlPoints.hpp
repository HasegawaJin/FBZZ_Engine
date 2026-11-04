/// @file    PresentationControlPoints.hpp
/// @brief   Resolve presentation target references into immutable numeric inputs.
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/Components/MotionWarpComponent.hpp>
#include <Engine/Scene/ControlPointResolver.hpp>
#include <Engine/Scene/ControlPointModes.hpp>
#include <Math/Vector3.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::scene {

class Scene;
struct LineRendererComponent;
enum class LineSpace : int;

inline constexpr std::uint32_t LINE_CONTROL_POINT_ADAPTER_VERSION = 1;

/// @brief Line-specific result status for point source validation and resolution.
enum class LineControlPointStatus : std::uint8_t {
    SUCCESS,
    INVALID_SOURCE_MODE,
    INVALID_RUNTIME_MODE,
    INVALID_NUMERIC_DATA,
    MISSING_BAKE,
    STALE_BAKE,
    RESOLUTION_FAILED,
};

/// @brief Effective ordered point sequence consumed by Line geometry generation.
struct LineControlPointSnapshot {
    std::vector<math::Vector3> points;
    LineSpace space = static_cast<LineSpace>(0);
    LineControlPointStatus status = LineControlPointStatus::SUCCESS;
    ControlPointResolveStatus resolveStatus = ControlPointResolveStatus::SUCCESS;
    std::uint64_t inputSignature = 0;
    bool bakeStale = false;
    bool valid = false;
};

/// @brief Resolve the current authoring source and capture its canonical numeric signature.
/// @note Successful signatures use ordered stable ids and numeric coordinates, excluding object GUIDs and Scene caches.
[[nodiscard]] FBZZ_ENGINE_API LineControlPointSnapshot ResolveLineControlPointAuthoring(
    Scene& scene, EntityID ownerId, LineRendererComponent& line,
    ControlPointRuntimeMode signatureRuntimeMode);

/// @brief Resolve the live source or return the captured static values for a Line.
/// @note RUNTIME reads BAKED_STATIC numeric values without resolving target objects.
[[nodiscard]] FBZZ_ENGINE_API LineControlPointSnapshot ResolveLineControlPoints(
    Scene& scene, EntityID ownerId, LineRendererComponent& line,
    ControlPointReadContext context = ControlPointReadContext::AUTHORING);

/// @brief Capture a configured target pose and begin one object-backed MotionWarp.
/// @note LIVE targets must be outside the owner hierarchy; CAPTURED targets may be descendants.
[[nodiscard]] FBZZ_ENGINE_API bool BeginMotionWarpToTarget(Scene& scene, EntityID ownerId,
    MotionWarpComponent& warp, float duration);

/// @brief Refresh only the pose of an active LIVE object-backed MotionWarp.
/// @return False when the reference no longer resolves or enters the owner hierarchy; the active warp is canceled.
[[nodiscard]] FBZZ_ENGINE_API bool RefreshLiveMotionWarpTarget(Scene& scene, EntityID ownerId,
    MotionWarpComponent& warp);

} /// @note namespace fbzz::scene
