/// @file    WaterSpline.hpp
/// @brief   Water spline authoring values and immutable CPU surface snapshots.
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Engine/Api.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fbzz::scene {
struct IReflector;
struct EntityID;
struct WaterComponent;
struct Transform;

/// @brief Water geometry read boundary for current queries or fixed-step consumers.
enum class WaterSurfaceReadView : std::uint8_t {
    SIMULATION,
    LIVE_QUERY,
};

/// @brief Maximum accepted control-point count for a water spline.
inline constexpr std::uint32_t WATER_SPLINE_MAX_CONTROL_POINTS = 256;
/// @brief Maximum accepted Catmull-Rom samples per control-point segment.
inline constexpr std::uint32_t WATER_SPLINE_MAX_SAMPLES_PER_SEGMENT = 64;
/// @brief Maximum accepted generated vertex count for a water spline snapshot.
inline constexpr std::size_t WATER_SPLINE_MAX_VERTICES = 32768;
/// @brief Maximum ID that fits the reflection system's signed integer field.
inline constexpr std::uint32_t WATER_SPLINE_MAX_POINT_ID = 0x7fffffffu;
/// @brief Maximum sampled lake outline vertices accepted before quadratic validation.
inline constexpr std::size_t WATER_SPLINE_MAX_LAKE_BOUNDARY_VERTICES = 2048;
/// @brief Maximum sampled river bank outline vertices accepted before quadratic validation.
inline constexpr std::size_t WATER_SPLINE_MAX_RIVER_BOUNDARY_VERTICES = 4096;

/// @brief Water footprint geometry selected by WaterComponent.
enum class WaterSurfaceShape : std::uint8_t {
    RECTANGLE = 0,
    SPLINE_LAKE = 1,
    SPLINE_RIVER = 2,
};

/// @brief One stable-ID control point for a lake outline or river centerline.
struct WaterSplinePoint {
    /// @brief Stable, nonzero identity preserved when points are reordered.
    std::uint32_t id = 0;
    /// @brief Control position in component-local metres.
    math::Vector3 localPosition = math::Vector3::ZERO;
    /// @brief Full river width in local metres; lake values are unused but must be finite.
    float width = 0.0f;
    /// @brief Water-column depth in world metres before the component cap.
    float depth = 0.0f;
    /// @brief Signed flow speed along the spline tangent in world metres per second.
    float flowSpeed = 0.0f;

    /// @brief Reflect the stable point fields for authoring and scene persistence.
    void Reflect(IReflector& reflector);
    bool operator==(const WaterSplinePoint& other) const = default;
};

/// @brief A sampled spline vertex shared by rendering and water queries.
struct WaterSplineVertex {
    math::Vector3 localPosition = math::Vector3::ZERO;
    math::Vector3 worldPosition = math::Vector3::ZERO;
    math::Vector3 worldVelocity = math::Vector3::ZERO;
    float baseHeightOffset = 0.0f;
    float depth = 0.0f;
};

/// @brief A triangle over the common water surface snapshot.
struct WaterSplineTriangle {
    std::array<std::uint32_t, 3> indices = {};
    math::Vector2 baseHeightGradient = math::Vector2::ZERO;
};

/// @brief One node in the snapshot's exact XZ triangle lookup hierarchy.
struct WaterSplineBvhNode {
    math::Vector2 boundsMin = math::Vector2::ZERO;
    math::Vector2 boundsMax = math::Vector2::ZERO;
    std::uint32_t left = 0;
    std::uint32_t right = 0;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

/// @brief Immutable numeric geometry used by water rendering, queries, and physics.
struct WaterSplineSnapshot {
    WaterSurfaceShape shape = WaterSurfaceShape::RECTANGLE;
    std::vector<WaterSplineVertex> vertices;
    std::vector<WaterSplineTriangle> triangles;
    /// @brief Original footprint triangulation retained before shared-cell refinement.
    std::vector<WaterSplineTriangle> footprintTriangles;
    std::vector<std::uint32_t> outlineIndices;
    std::vector<WaterSplineBvhNode> bvhNodes;
    std::vector<std::uint32_t> triangleOrder;
    math::Vector3 localBoundsMin = math::Vector3::ZERO;
    math::Vector3 localBoundsMax = math::Vector3::ZERO;
    math::Vector3 worldBoundsMin = math::Vector3::ZERO;
    math::Vector3 worldBoundsMax = math::Vector3::ZERO;
    float worldBaseY = 0.0f;
    float minimumBaseHeightOffset = 0.0f;
    float maximumBaseHeightOffset = 0.0f;
    float maximumDepth = 0.0f;
    float maximumWorldEdgeLength = 0.0f;
    bool meshQualityLimited = false;
    std::uint64_t signature = 0;
};

/// @brief Interpolated spline surface values at one world XZ position.
struct WaterSplineSample {
    float baseHeight = 0.0f;
    float baseHeightOffset = 0.0f;
    float depth = 0.0f;
    math::Vector3 worldVelocity = math::Vector3::ZERO;
    math::Vector2 baseHeightGradient = math::Vector2::ZERO;
};

/// @brief Return the component-centered local XZ extent used by water geometry and UV mapping.
/// @param water Water surface whose selected shape determines the footprint extent.
/// @return Authored rectangle extent or the spline's symmetric local bounds extent.
[[nodiscard]] FBZZ_ENGINE_API math::Vector2 WaterSurfaceLocalExtent(const WaterComponent& water,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);

/// @brief Return the shape interpretation captured by the selected read view.
[[nodiscard]] FBZZ_ENGINE_API WaterSurfaceShape WaterSurfaceEffectiveShape(const WaterComponent& water,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);

/// @brief Whether water geometry replaces the authored Rectangle grid.
/// @note Invalid authored splines remain geometry-selected so callers never fall back to a rectangle.
[[nodiscard]] FBZZ_ENGINE_API bool UsesWaterSurfaceGeometry(const WaterComponent& water,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);

/// @brief Return the immutable effective geometry, or null when selected geometry is invalid.
/// @note A Zone owner exposes its shared snapshot without changing the authored surface shape.
[[nodiscard]] FBZZ_ENGINE_API const WaterSplineSnapshot* WaterSurfaceGeometry(const WaterComponent& water,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);

/// @brief Return the owner pose captured with the selected water geometry.
/// @param water Water component holding the simulation pose.
/// @param currentTransform Current pose used by LiveQuery or before the first simulation publication.
/// @param view Geometry boundary whose pose is requested.
[[nodiscard]] FBZZ_ENGINE_API const Transform& WaterSurfaceTransform(const WaterComponent& water,
    const Transform& currentTransform, WaterSurfaceReadView view);

/// @brief Return the Zone membership captured with the selected water geometry.
/// @param water Water component holding current and simulation membership.
/// @param view Geometry boundary whose Zone owner is requested.
[[nodiscard]] FBZZ_ENGINE_API EntityID WaterSurfaceZoneOwner(const WaterComponent& water,
    WaterSurfaceReadView view);

/// @brief Validate local, scaled, and rotated shared extents used by spline UV and atlas mapping.
/// @param snapshot Baked spline bounds.
/// @param transform World pose used to map the centered bounds.
/// @return True only when every extent remains finite and representable as a float.
[[nodiscard]] FBZZ_ENGINE_API bool HasValidWaterSplineExtents(const WaterSplineSnapshot& snapshot, const Transform& transform);

/// @brief Hash all spline authoring and world-transform values used to build a snapshot.
/// @param shape Selected lake or river spline shape.
/// @param points Stable-ID control points in component-local space.
/// @param samplesPerSegment Catmull-Rom samples per control-point segment.
/// @param transform Current world transform used to bake positions and flow vectors.
/// @param maximumWaterDepth Global depth cap shared by physics and rendering.
/// @param maximumWorldCellSize Requested maximum world edge length after shared midpoint refinement.
/// @return Stable signature for cache invalidation.
[[nodiscard]] FBZZ_ENGINE_API std::uint64_t WaterSplineSnapshotSignature(WaterSurfaceShape shape,
    std::span<const WaterSplinePoint> points, std::uint32_t samplesPerSegment,
    const Transform& transform, float maximumWaterDepth = 10.0f,
    float maximumWorldCellSize = 2.0f);

/// @brief Build the bounded triangle snapshot for the selected lake or river, shared by rendering, queries, and physics.
/// @param points Stable-ID control points in component-local space.
/// @param samplesPerSegment Catmull-Rom subdivisions, checked without truncation.
/// @param transform Upright world pose used for positions and horizontal flow velocities.
/// @param maximumWaterDepth World depth cap shared by physics and rendering.
/// @param maximumWorldCellSize Refined world edge target shared by the mesh and wave sampling.
/// @return False for invalid or over-budget input with out unchanged; success replaces out atomically.
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/water-body-actors-in-unreal-engine Epic Games, Water Body Actors.
[[nodiscard]] FBZZ_ENGINE_API bool BuildWaterSplineSnapshot(WaterSurfaceShape shape,
    std::span<const WaterSplinePoint> points, std::uint32_t samplesPerSegment,
    const Transform& transform, WaterSplineSnapshot& out, float maximumWaterDepth = 10.0f,
    float maximumWorldCellSize = 2.0f);

/// @brief Recompute bounds, refine shared edges, gradients, and the BVH for an authored numeric mesh.
/// @return False for invalid or over-budget geometry; out may be partially updated.
[[nodiscard]] FBZZ_ENGINE_API bool FinalizeWaterSplineSnapshot(WaterSplineSnapshot& snapshot,
    float maximumWorldCellSize);

/// @brief Recompute snapshot bounds, triangle gradients, and lookup hierarchy without refining its vertices.
[[nodiscard]] FBZZ_ENGINE_API bool RebuildWaterSplineSnapshotDerivedData(WaterSplineSnapshot& snapshot);

/// @brief Sample the same triangle surface consumed by the renderer.
/// @param snapshot Immutable world-space spline geometry.
/// @param worldX,worldZ Query location in world space.
/// @param out Receives interpolated height, depth, and velocity on success.
/// @return False outside the spline footprint or when the snapshot/query is invalid; out remains unchanged.
[[nodiscard]] FBZZ_ENGINE_API bool TrySampleWaterSpline(const WaterSplineSnapshot& snapshot,
    float worldX, float worldZ, WaterSplineSample& out);
} /// @note namespace fbzz::scene
