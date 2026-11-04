/// @file    NavigationControlPoints.hpp
/// @brief   Resolve navigation waypoint and Off-Mesh Link authoring into numeric snapshots.
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/ControlPointResolver.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <vector>

namespace fbzz::scene {

class Scene;

/// @brief Navigation ControlPoint adapter outcome.
enum class NavigationControlPointStatus : std::uint8_t {
    SUCCESS,
    INVALID_SOURCE_MODE,
    INVALID_RUNTIME_MODE,
    INVALID_NUMERIC_DATA,
    TOO_MANY_POINTS,
    MISSING_BAKE,
    STALE_BAKE,
    RESOLUTION_FAILED,
    SELF_REFERENTIAL_TARGET,
};

/// @brief Ordered numeric patrol waypoint detached from Scene objects.
struct NavMeshPatrolPointValue {
    std::uint32_t stableId = 0;
    math::Vector3 worldPosition = math::Vector3::ZERO;
    float waitTime = 0.0f;
    float speed = 0.0f;
    bool operator==(const NavMeshPatrolPointValue&) const = default;
};

/// @brief All-or-nothing patrol input resolved for one consumer boundary.
struct NavMeshPatrolPointSnapshot {
    std::vector<NavMeshPatrolPointValue> points;
    NavigationControlPointStatus status = NavigationControlPointStatus::SUCCESS;
    ControlPointResolveStatus resolveStatus = ControlPointResolveStatus::SUCCESS;
    std::uint64_t inputSignature = 0;
    bool bakeStale = false;
    bool valid = false;
};

/// @brief Numeric world endpoints captured for an Off-Mesh Link bake.
struct NavMeshOffMeshLinkSnapshot {
    math::Vector3 startPoint = math::Vector3::ZERO;
    math::Vector3 endPoint = math::Vector3::ZERO;
    NavigationControlPointStatus status = NavigationControlPointStatus::SUCCESS;
    ControlPointResolveStatus resolveStatus = ControlPointResolveStatus::SUCCESS;
    std::uint64_t inputSignature = 0;
    bool valid = false;
};

/// @brief Resolve source authoring values independently of the selected runtime policy.
/// @param patrol Authored patrol input.
/// @param signatureRuntimeMode Runtime mode to include in the resulting bake signature.
/// @pre Scene owner and target world transforms are synchronized.
/// @return Numeric route values, or an invalid snapshot with a nonzero raw-input signature.
/// @see Docs/design/control-point-usage-audit.md
[[nodiscard]] FBZZ_ENGINE_API NavMeshPatrolPointSnapshot ResolveNavMeshPatrolPointAuthoring(
    Scene& scene, EntityID ownerId, const NavMeshPatrolComponent& patrol,
    ControlPointRuntimeMode signatureRuntimeMode);

/// @brief Resolve the selected patrol source at an authoring or runtime boundary.
/// @note Runtime BAKED_STATIC reads only its already-validated numeric bake.
/// @param context AUTHORING resolves refs and checks bake freshness; RUNTIME uses the selected live or baked values.
/// @pre Scene owner and target world transforms are synchronized when LIVE or AUTHORING point-object inputs are resolved.
/// @return All ordered waypoints or an invalid snapshot; partial routes are never returned.
/// @see Docs/design/control-point-authoring.md
[[nodiscard]] FBZZ_ENGINE_API NavMeshPatrolPointSnapshot ResolveNavMeshPatrolPoints(
    Scene& scene, EntityID ownerId, const NavMeshPatrolComponent& patrol,
    ControlPointReadContext context = ControlPointReadContext::AUTHORING);

/// @brief Resolve Off-Mesh Link endpoints into numeric world positions for hashing and bake capture.
/// @param scene Scene containing the link owner and TargetPoint references.
/// @param ownerId GameObject carrying the link component.
/// @param link Authored numeric or object-backed endpoints.
/// @pre Scene owner and target world transforms are synchronized for POINT_OBJECTS.
/// @return Numeric endpoints or an invalid snapshot with a nonzero raw-input signature.
/// @see Docs/design/control-point-usage-audit.md
[[nodiscard]] FBZZ_ENGINE_API NavMeshOffMeshLinkSnapshot ResolveNavMeshOffMeshLinkEndpoints(
    Scene& scene, EntityID ownerId, const NavMeshOffMeshLinkComponent& link);

/// @brief Reject stale or malformed static Patrol bakes before the runtime starts.
/// @param scene Scene whose current authored patrol points are validated.
/// @return False when a static Patrol cannot run from its saved numeric values.
[[nodiscard]] FBZZ_ENGINE_API bool ValidateStaticNavMeshPatrolBakes(Scene& scene);

} /// @note namespace fbzz::scene
