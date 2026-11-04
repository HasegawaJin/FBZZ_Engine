/// @file    ControlPointAdapters.hpp
/// @brief   Water と Spline の点参照を数値 authoring snapshot へ変換する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/ControlPointResolver.hpp>
#include <Engine/Scene/ControlPointModes.hpp>
#include <Engine/Scene/ObjectReference.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <vector>

namespace fbzz::asset { struct ClothAsset; }

namespace fbzz::scene {

struct IReflector;
class Scene;
struct SplineComponent;
struct WaterComponent;

inline constexpr std::uint32_t CONTROL_POINT_ADAPTER_VERSION = 1;

/// @brief Water-specific stable binding row and per-point surface attributes.
struct WaterControlPointBinding {
    std::uint32_t id = 0;
    ObjectReference target;
    float width = 0.0f;
    float depth = 0.0f;
    float flowSpeed = 0.0f;

    /// @brief Reflect a stable binding row without changing its order or attributes.
    FBZZ_ENGINE_API void Reflect(IReflector& reflector);
    FBZZ_ENGINE_API bool operator==(const WaterControlPointBinding& other) const;
};

/// @brief Spline-specific stable binding row.
struct SplineControlPointBinding {
    std::uint32_t id = 0;
    ObjectReference target;

    /// @brief Reflect a stable binding row without changing its order.
    FBZZ_ENGINE_API void Reflect(IReflector& reflector);
    FBZZ_ENGINE_API bool operator==(const SplineControlPointBinding& other) const;
};

/// @brief Owner-local numeric Spline point stored by a static bake.
struct SplineControlPointValue {
    std::uint32_t id = 0;
    math::Vector3 localPosition = math::Vector3::ZERO;
    bool operator==(const SplineControlPointValue& other) const = default;
};

/// @brief Adapter-level outcomes for source selection, bake validity, and numeric constraints.
enum class ControlPointAdapterStatus : std::uint8_t {
    SUCCESS,
    INVALID_SOURCE_MODE,
    INVALID_RUNTIME_MODE,
    INVALID_NUMERIC_DATA,
    INVALID_LAKE_PLANE,
    MISSING_BAKE,
    STALE_BAKE,
    RESOLUTION_FAILED,
};

/// @brief Result of resolving Water source rows before footprint generation.
struct WaterControlPointSnapshot {
    std::vector<WaterSplinePoint> points;
    WaterSurfaceShape surfaceShape = WaterSurfaceShape::RECTANGLE;
    float lakePlaneHeight = 0.0f;
    ControlPointAdapterStatus status = ControlPointAdapterStatus::SUCCESS;
    ControlPointResolveStatus resolveStatus = ControlPointResolveStatus::SUCCESS;
    std::uint64_t inputSignature = 0;
    bool bakeStale = false;
    bool valid = false;
};

/// @brief Result of resolving Spline source rows before follower sampling.
struct SplineControlPointSnapshot {
    std::vector<SplineControlPointValue> points;
    /// @brief Effective open/closed interpretation associated with the resolved point sequence.
    bool closed = false;
    ControlPointAdapterStatus status = ControlPointAdapterStatus::SUCCESS;
    ControlPointResolveStatus resolveStatus = ControlPointResolveStatus::SUCCESS;
    std::uint64_t inputSignature = 0;
    bool bakeStale = false;
    bool valid = false;
};

/// @brief Reflect ordered Water binding rows and preserve an over-limit sentinel row.
FBZZ_ENGINE_API void ReflectWaterControlPointBindings(IReflector& reflector,
    std::vector<WaterControlPointBinding>& bindings);

/// @brief Reflect ordered Spline binding rows and preserve an over-limit sentinel row.
FBZZ_ENGINE_API void ReflectSplineControlPointBindings(IReflector& reflector,
    std::vector<SplineControlPointBinding>& bindings);

/// @brief Reflect stable-ID numeric Spline bake points.
FBZZ_ENGINE_API void ReflectSplineControlPointValues(IReflector& reflector,
    const char* fieldName, std::vector<SplineControlPointValue>& points);

/// @brief Reflect Water bake rows using the existing spline point schema.
FBZZ_ENGINE_API void ReflectWaterControlPointValues(IReflector& reflector,
    const char* fieldName, std::vector<WaterSplinePoint>& points);

/// @brief Hash Cloth solver particle identity and triangle topology without positions or materials.
/// @param asset Null for generated grid cloth.
/// @param generatedSegments Segment count used by generated grid cloth.
/// @return Nonzero signature stable across position and material-only edits.
[[nodiscard]] FBZZ_ENGINE_API std::uint64_t ClothTopologySignature(
    const asset::ClothAsset* asset, std::uint32_t generatedSegments);

/// @brief Return the current Cloth topology and particle count for a candidate attachment.
/// @param asset Current imported asset, or null for the generated grid source.
/// @param generatedSegments Current grid segment count.
/// @param particle Candidate solver particle index.
/// @param signature Receives the topology signature only on success.
/// @param particleCount Receives the current solver particle count.
/// @return False when the topology is invalid or particle is outside its current range.
[[nodiscard]] FBZZ_ENGINE_API bool ResolveClothTopologyForParticle(const asset::ClothAsset* asset,
    std::uint32_t generatedSegments, int particle, std::uint64_t& signature,
    std::uint32_t& particleCount);

/// @brief Resolve source authoring values regardless of the component's runtime mode.
/// @note signatureRuntimeMode lets a bake operation capture the signature for the mode it will commit.
[[nodiscard]] FBZZ_ENGINE_API WaterControlPointSnapshot ResolveWaterControlPointAuthoring(
    Scene& scene, EntityID ownerId, WaterComponent& water,
    ControlPointRuntimeMode signatureRuntimeMode);

/// @brief Resolve current Water runtime values, including a retained static bake when selected.
/// @note RUNTIME never resolves static authoring targets; Scene activation must first validate their bake.
[[nodiscard]] FBZZ_ENGINE_API WaterControlPointSnapshot ResolveWaterControlPoints(
    Scene& scene, EntityID ownerId, WaterComponent& water,
    ControlPointReadContext context = ControlPointReadContext::AUTHORING);

/// @brief Resolve source authoring values regardless of the component's runtime mode.
/// @note signatureRuntimeMode lets a bake operation capture the signature for the mode it will commit.
[[nodiscard]] FBZZ_ENGINE_API SplineControlPointSnapshot ResolveSplinePointAuthoring(
    Scene& scene, EntityID ownerId, SplineComponent& spline,
    ControlPointRuntimeMode signatureRuntimeMode);

/// @brief Resolve current Spline runtime values, including a retained static bake when selected.
/// @note RUNTIME never resolves static authoring targets; Scene activation must first validate their bake.
[[nodiscard]] FBZZ_ENGINE_API SplineControlPointSnapshot ResolveSplinePoints(
    Scene& scene, EntityID ownerId, SplineComponent& spline,
    ControlPointReadContext context = ControlPointReadContext::AUTHORING);

/// @brief Reject missing, invalid, or stale static Water and Spline bakes before runtime starts.
/// @param scene Scene whose current numeric authoring is compared with each captured bake.
/// @return False when any non-LIVE control-point policy cannot run from its saved snapshot.
[[nodiscard]] FBZZ_ENGINE_API bool ValidateStaticControlPointBakes(Scene& scene);

} /// @note namespace fbzz::scene
