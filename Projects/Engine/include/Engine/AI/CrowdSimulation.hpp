/// @file    CrowdSimulation.hpp
/// @brief   Bounded 3D crowd decisions and continuous steering over caller-owned snapshots.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include <Math/Vector3.hpp>
#include <cstdint>
#include <span>
#include <vector>

namespace fbzz::ai {

enum class CrowdIntent : std::uint32_t { WANDER, SEEK, ARRIVE, PURSUE, FLEE, HOLD, EXTERNAL };
enum class CrowdMode : std::uint32_t { CRUISE, FLEE, PURSUE, ARRIVE, HOLD };
namespace CrowdFlags {
inline constexpr std::uint32_t ACTIVE = 1;
inline constexpr std::uint32_t REQUEST_DECISION = 2;
inline constexpr std::uint32_t OVERRIDE_STEERING = 4;
}
namespace CrowdEvents {
inline constexpr std::uint32_t MODE_CHANGED = 1;
inline constexpr std::uint32_t THREAT_ENTERED = 2;
inline constexpr std::uint32_t THREAT_LEFT = 4;
inline constexpr std::uint32_t ARRIVED = 8;
inline constexpr std::uint32_t CONSTRAINED = 16;
inline constexpr std::uint32_t RECOVERED = 32;
}

/// @note All distances are world meters; intervals and memory are seconds. One simulation is one habitat.
struct CrowdSettings {
    math::Vector3 minimum{-50, -20, -50};
    math::Vector3 maximum{50, 0, 50};
    math::Vector3 observer{};
    float nearDistance = 25;
    float midDistance = 85;
    float nearInterval = 0.1f;
    float midInterval = 0.25f;
    float farInterval = 1;
    float maximumDelta = 0.1f;
    float deltaSeconds = 0;
    std::uint32_t maximumDecisions = 256;
    std::uint32_t candidateLimit = 36;
    std::uint32_t neighborLimit = 12;
    std::uint32_t decisionCursor = 0;
    float fearDecay = 0.5f;
    float threatMemory = 1.5f;
    float avoidanceLookahead = 0.5f;
    std::uint32_t reserved = 0;
};

/// @note Grouping is per-agent; profiles hold species or movement policies, with no fish-specific code.
struct CrowdProfile {
    float neighborRadius = 2;
    float separationRadius = 0.6f;
    float cohesion = 0.4f;
    float alignment = 0.5f;
    float separation = 1.2f;
    float goalWeight = 1;
    float cruiseSpeed = 1;
    float fleeSpeed = 2;
    float maximumAcceleration = 3;
    float maximumTurnRate = 2;
    float arrivalRadius = 0.2f;
    float slowingRadius = 2;
    float bodyRadius = 0.15f;
    float floorOffset = 0.3f;
    float floorAttraction = 2;
    std::uint32_t followFloor = 0;
};

/// @note Input and output are separate snapshots. RNG, decisions and threat memory are per individual.
/// @note Floor/ceiling are cached world-Y envelopes supplied by terrain sampling; this is not 3D pathfinding.
struct CrowdAgent {
    math::Vector3 position{};
    math::Vector3 velocity{};
    math::Vector3 desiredVelocity{};
    math::Vector3 goal{};
    math::Vector3 target{};
    math::Vector3 targetVelocity{};
    math::Vector3 externalVelocity{};
    math::Vector3 dangerPosition{};
    float floor = -20;
    float ceiling = 0;
    float fear = 0;
    float decisionIn = 0;
    float decisionAge = 0;
    float threatIn = 0;
    float stuckSeconds = 0;
    std::uint32_t seed = 1;
    std::uint32_t group = 0;
    std::uint32_t profile = 0;
    CrowdIntent intent = CrowdIntent::WANDER;
    std::uint32_t flags = CrowdFlags::ACTIVE;
    std::uint32_t events = 0;
    CrowdMode mode = CrowdMode::CRUISE;
    std::uint32_t senseGroup = UINT32_MAX;
    std::int32_t sensedIndex = -1;
    float sensedDistance = 0;
};

struct CrowdThreat {
    math::Vector3 position{};
    float radius = 5;
    float strength = 1;
    float memory = 1.5f;
    std::uint32_t group = UINT32_MAX;
};
struct CrowdObstacle { math::Vector3 position{}; float radius = 1; };
struct CrowdStats {
    std::uint32_t decisions = 0;
    std::uint32_t candidateTests = 0;
    std::uint32_t neighbors = 0;
    std::uint32_t deferredDecisions = 0;
    std::uint32_t constraints = 0;
    std::uint32_t recoveries = 0;
    std::uint32_t threatReactions = 0;
    std::uint32_t occupiedCells = 0;
    std::uint32_t decisionCursor = 0;
    float maximumAcceleration = 0;
    float maximumTurn = 0;
};

/// @brief Scratch capacity is retained between batches; no world or agent identity is retained.
class CrowdWorkspace {
public:
    /// @brief Preallocates capacity before measured or real-time work.
    void Reserve(std::size_t capacity);
private:
    struct Cell { int x = 0, y = 0, z = 0, head = -1; std::uint32_t stamp = 0; };
    std::vector<Cell> m_cells;
    std::vector<int> m_next;
    std::uint32_t m_stamp = 0;
    friend bool StepCrowd(const CrowdSettings&, std::span<const CrowdProfile>, std::span<const CrowdAgent>,
                          std::span<const CrowdThreat>, std::span<const CrowdObstacle>, std::span<CrowdAgent>,
                          CrowdStats&, CrowdWorkspace&);
};

/// @brief Runs one bounded decision pass and continuous movement pass without touching a Scene.
/// @return Invalid inputs or aliased input/output return false and leave output and stats unchanged.
/// @note Decision budgets are deterministic work counts. Threat steering bypasses the decision budget.
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine
/// @see https://www.red3d.com/cwr/boids/
[[nodiscard]] bool StepCrowd(const CrowdSettings& settings, std::span<const CrowdProfile> profiles,
                             std::span<const CrowdAgent> agents, std::span<const CrowdThreat> threats,
                             std::span<const CrowdObstacle> obstacles, std::span<CrowdAgent> output,
                             CrowdStats& stats, CrowdWorkspace& workspace);

static_assert(sizeof(CrowdSettings) == 96);
static_assert(sizeof(CrowdProfile) == 64);
static_assert(sizeof(CrowdAgent) == 164);
static_assert(sizeof(CrowdThreat) == 28);
static_assert(sizeof(CrowdObstacle) == 16);
static_assert(sizeof(CrowdStats) == 44);
}
