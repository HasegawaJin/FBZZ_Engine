/// @file    WaterInteraction.hpp
/// @brief   水中接触の数値イベント・局所濁り・少数の再利用粒子を管理する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <Math/Vector3.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::scene {
class Scene;
struct IReflector;
struct WaterComponent;
struct WaterInteractionSceneStore;

inline constexpr std::size_t WATER_INTERACTION_MAX_PENDING = 32;
inline constexpr std::size_t WATER_INTERACTION_MAX_ACTIVE = 32;
inline constexpr std::size_t WATER_INTERACTION_MAX_PARTICLES = 64;
inline constexpr std::size_t WATER_INTERACTION_MAX_TRACKED_BODIES = 128;
inline constexpr std::size_t WATER_INTERACTION_MAX_OWNERS = 256;
inline constexpr const char* WATER_BUBBLE_MATERIAL =
    "guid:219450eba3664e06b0294f27376b0d3f|Assets/Materials/Water/WaterBubbles.mat";
inline constexpr const char* WATER_SEDIMENT_MATERIAL =
    "guid:8d8c0b099aae44f3b9a30db32f1faf92|Assets/Materials/Water/WaterSediment.mat";

enum class WaterInteractionKind : uint8_t { SURFACE_FOAM = 0, BUBBLES = 1, SEDIMENT = 2 };

/// @brief World-space authoring impulse; position/radius use meters and velocity uses meters per second.
struct WaterInteractionImpulse {
    WaterInteractionKind kind = WaterInteractionKind::BUBBLES;
    math::Vector3 worldPosition{};
    math::Vector3 worldVelocity{};
    float radius = 0.25f;
    float strength = 0.5f;
    float lifetime = 2.0f;
    bool emitSurfaceRipple = true;
};

struct WaterInteractionSettings {
    bool enabled = true;
    bool bubblesEnabled = true;
    bool foamEnabled = true;
    bool sedimentEnabled = true;
    float minimumMotionSpeed = 1.5f;
    float motionInterval = 0.25f;
    float surfaceBand = 0.6f;
    float sedimentMinimumSpeed = 1.0f;
    float sedimentInterval = 0.4f;
    float bubbleLifetime = 2.0f;
    float sedimentLifetime = 5.0f;
    math::Vector3 sedimentScattering = {0.16f, 0.10f, 0.045f};
    std::string bubbleMaterialPath = WATER_BUBBLE_MATERIAL;
    std::string sedimentMaterialPath = WATER_SEDIMENT_MATERIAL;
    void Reflect(IReflector& reflector);
};

/// @brief Immutable numeric plume; density multiplies scattering coefficients in inverse meters.
struct WaterInteractionPlume {
    WaterInteractionKind kind = WaterInteractionKind::SEDIMENT;
    math::Vector3 worldPosition{};
    math::Vector3 worldVelocity{};
    math::Vector3 scattering{};
    float radius = 0.25f;
    float density = 0.0f;
    float age = 0.0f;
    float lifetime = 2.0f;
};

/// @brief Render consumers retain only values; this frame has no Scene, GameObject, Physics or RHI references.
struct WaterInteractionFrame {
    uint64_t sceneGeneration = 0;
    EntityID owner = EntityID::INVALID;
    uint64_t ownerLifetime = 0;
    uint64_t revision = 0;
    std::vector<WaterInteractionPlume> plumes;
};

struct WaterInteractionEmitterState {
    EntityID id = EntityID::INVALID;
    uint64_t lifetime = 0;
};

/// @brief Water-owned transient state; omit from reflection, save and clone.
struct WaterInteractionRuntime {
    uint64_t sceneGeneration = 0;
    EntityID owner = EntityID::INVALID;
    uint64_t ownerLifetime = 0;
    uint64_t revision = 0;
    std::vector<WaterInteractionImpulse> pending;
    std::vector<WaterInteractionPlume> active;
    std::array<WaterInteractionEmitterState, 2> emitters{};
    std::shared_ptr<const WaterInteractionFrame> snapshot;
    std::shared_ptr<WaterInteractionSceneStore> sceneStore;
};

/// @return False leaves the pending queue unchanged for invalid input, disabled/excluded water or exhausted capacity.
/// @note This entry only records values; Cleanup performs structural changes.
bool QueueWaterInteraction(Scene& scene, EntityID owner, const WaterInteractionImpulse& impulse);

/// @brief Projects a validated surface ripple/splash position onto its existing water interface and records foam.
bool QueueWaterSurfaceInteraction(Scene& scene, EntityID owner, math::Vector3 worldPosition, float strength);

/// @brief Establishes tracked owner/body lifetime and rate-limited foam or bubbles using the existing WaterSystem body traversal.
void SupplyWaterMotionImpulse(Scene& scene, EntityID owner, EntityID body,
    math::Vector3 worldPosition, math::Vector3 worldVelocity, float dt, bool nearSurface,
    bool emitSurfaceRipple = true);

/// @brief Records the current fixed-step contact of a previously tracked body; no owner/Scene scan is performed.
void SupplyWaterGroundContact(Scene& scene, EntityID body,
    math::Vector3 worldPoint, math::Vector3 worldNormal, float approachSpeed = 0.0f);

/// @brief Accumulates fixed-step simulation time; contact expiry follows the serial Cleanup frame so catchup steps remain valid.
void BeginWaterInteractionPhysicsStep(Scene& scene, float fixedDt);

/// @brief Copies runtime values at the production renderer boundary.
std::shared_ptr<const WaterInteractionFrame> WaterInteractionSnapshot(const WaterComponent& water);

/// @brief Clears transient authoring-copy state without deleting another owner's generated objects.
void ResetWaterInteractionRuntime(WaterComponent& water);

/// @brief Retires this Scene's generated emitters and transient data at a serial Scene deactivation boundary.
void ClearWaterInteractions(Scene& scene);
} /// @note namespace fbzz::scene
