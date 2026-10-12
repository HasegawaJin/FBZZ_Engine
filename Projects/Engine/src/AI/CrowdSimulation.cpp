/// @file    CrowdSimulation.cpp
/// @brief   Stamped spatial hashing, budgeted decisions and safety-constrained 3D steering.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include <Engine/AI/CrowdSimulation.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace fbzz::ai {
namespace {
using V = math::Vector3;
constexpr float EPS = 0.00001f;
bool Scalar(float v) { return std::isfinite(v) && std::abs(v) <= 1000000; }
bool Vector(V v) { return Scalar(v.x) && Scalar(v.y) && Scalar(v.z); }
bool Nonnegative(float v) { return Scalar(v) && v >= 0; }
V Unit(V v, V fallback = {0, 0, 1}) { const float l = v.Length(); return l > EPS ? v / l : fallback; }
V Limit(V v, float maximum) { const float l = v.Length(); return l > maximum && l > EPS ? v * (maximum / l) : v; }
V Clamp(V v, V lo, V hi) { return {std::clamp(v.x, lo.x, hi.x), std::clamp(v.y, lo.y, hi.y), std::clamp(v.z, lo.z, hi.z)}; }
std::uint32_t Random(std::uint32_t& seed) { std::uint32_t x = seed ? seed : 0x9E3779B9U; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return seed = x; }
float UnitRandom(std::uint32_t& seed) { return static_cast<float>(Random(seed) >> 8) / 16777216.0f; }
V RandomPoint(std::uint32_t& seed, V lo, V hi) {
    const float x = UnitRandom(seed), y = UnitRandom(seed), z = UnitRandom(seed);
    return {lo.x + (hi.x - lo.x) * x, lo.y + (hi.y - lo.y) * y, lo.z + (hi.z - lo.z) * z};
}
std::uint32_t Hash(int x, int y, int z) {
    return (static_cast<std::uint32_t>(x) * 73856093U) ^ (static_cast<std::uint32_t>(y) * 19349663U) ^ (static_cast<std::uint32_t>(z) * 83492791U);
}
float Interval(const CrowdSettings& s, V position) {
    const float d = (position - s.observer).LengthSq();
    return d <= s.nearDistance * s.nearDistance ? s.nearInterval : d <= s.midDistance * s.midDistance ? s.midInterval : s.farInterval;
}
CrowdMode Mode(const CrowdAgent& a) {
    if (a.threatIn > 0 || a.intent == CrowdIntent::FLEE) return CrowdMode::FLEE;
    if (a.intent == CrowdIntent::PURSUE) return CrowdMode::PURSUE;
    if (a.intent == CrowdIntent::ARRIVE) return CrowdMode::ARRIVE;
    if (a.intent == CrowdIntent::HOLD) return CrowdMode::HOLD;
    return CrowdMode::CRUISE;
}
bool Valid(const CrowdSettings& s, std::span<const CrowdProfile> profiles, std::span<const CrowdAgent> agents,
           std::span<const CrowdThreat> threats, std::span<const CrowdObstacle> obstacles) {
    if (agents.size() > 4096 || profiles.empty() || profiles.size() > 64 || threats.size() > 64 || obstacles.size() > 256
        || !Vector(s.minimum) || !Vector(s.maximum) || !Vector(s.observer) || s.minimum.x >= s.maximum.x || s.minimum.y >= s.maximum.y || s.minimum.z >= s.maximum.z
        || !Nonnegative(s.nearDistance) || !Nonnegative(s.midDistance) || s.nearDistance > s.midDistance
        || !Nonnegative(s.nearInterval) || !Nonnegative(s.midInterval) || !Nonnegative(s.farInterval)
        || !Nonnegative(s.maximumDelta) || s.maximumDelta <= 0 || s.maximumDelta > 1 || !Nonnegative(s.deltaSeconds)
        || s.candidateLimit == 0 || s.candidateLimit > 256 || s.neighborLimit == 0 || s.neighborLimit > s.candidateLimit
        || s.maximumDecisions > 4096 || !Nonnegative(s.fearDecay) || !Nonnegative(s.threatMemory) || !Nonnegative(s.avoidanceLookahead) || s.reserved) return false;
    for (const auto& p : profiles) {
        if (!Nonnegative(p.neighborRadius) || p.neighborRadius < 0.001f || !Nonnegative(p.separationRadius) || p.separationRadius > p.neighborRadius
            || !Nonnegative(p.cohesion) || !Nonnegative(p.alignment) || !Nonnegative(p.separation) || !Nonnegative(p.goalWeight)
            || !Nonnegative(p.cruiseSpeed) || p.cruiseSpeed > 1000 || !Nonnegative(p.fleeSpeed) || p.fleeSpeed > 1000
            || !Nonnegative(p.maximumAcceleration) || p.maximumAcceleration > 10000 || !Nonnegative(p.maximumTurnRate) || p.maximumTurnRate > 1000
            || !Nonnegative(p.arrivalRadius) || !Nonnegative(p.slowingRadius) || !Nonnegative(p.bodyRadius)
            || p.bodyRadius * 2 >= std::min({s.maximum.x - s.minimum.x, s.maximum.y - s.minimum.y, s.maximum.z - s.minimum.z})
            || !Nonnegative(p.floorOffset) || !Nonnegative(p.floorAttraction) || p.followFloor > 1) return false;
    }
    for (const auto& a : agents) {
        if (!Vector(a.position) || !Vector(a.velocity) || !Vector(a.desiredVelocity) || !Vector(a.goal) || !Vector(a.target)
            || !Vector(a.targetVelocity) || !Vector(a.externalVelocity) || !Vector(a.dangerPosition) || !Scalar(a.floor) || !Scalar(a.ceiling)
            || a.floor > a.ceiling || !Nonnegative(a.fear) || a.fear > 1 || !Scalar(a.decisionIn) || !Nonnegative(a.decisionAge)
            || !Nonnegative(a.threatIn) || !Nonnegative(a.stuckSeconds) || a.profile >= profiles.size()
            || static_cast<std::uint32_t>(a.intent) > 6 || static_cast<std::uint32_t>(a.mode) > 4 || (a.flags & ~7U)
            || !Nonnegative(a.sensedDistance) || a.sensedIndex < -1 || a.sensedIndex >= static_cast<std::int32_t>(agents.size())) return false;
    }
    for (const auto& t : threats) if (!Vector(t.position) || !Nonnegative(t.radius) || !Nonnegative(t.strength) || t.strength > 1 || !Nonnegative(t.memory)) return false;
    for (const auto& o : obstacles) if (!Vector(o.position) || !Nonnegative(o.radius)) return false;
    return true;
}
V Turn(V velocity, V desired, float maximumAngle) {
    const float speed = desired.Length();
    if (velocity.LengthSq() < EPS * EPS || speed < EPS) return desired;
    const V from = Unit(velocity), to = desired / speed;
    const float angle = std::acos(std::clamp(V::Dot(from, to), -1.0f, 1.0f));
    if (angle <= maximumAngle || angle < EPS) return desired;
    V tangent = to - from * V::Dot(from, to);
    if (tangent.LengthSq() < EPS * EPS) tangent = V::Cross(from, std::abs(from.y) < 0.9f ? V{0, 1, 0} : V{1, 0, 0});
    tangent = Unit(tangent);
    return (from * std::cos(maximumAngle) + tangent * std::sin(maximumAngle)) * speed;
}
}

void CrowdWorkspace::Reserve(std::size_t capacity) {
    capacity = std::min<std::size_t>(capacity, 4096);
    std::size_t cells = 8;
    while (cells < capacity * 2) cells <<= 1;
    if (m_cells.size() < cells) m_cells.resize(cells);
    if (m_next.size() < capacity) m_next.resize(capacity);
}

bool StepCrowd(const CrowdSettings& s, std::span<const CrowdProfile> profiles, std::span<const CrowdAgent> agents,
               std::span<const CrowdThreat> threats, std::span<const CrowdObstacle> obstacles, std::span<CrowdAgent> output,
               CrowdStats& stats, CrowdWorkspace& w) {
    if (output.size() < agents.size() || !Valid(s, profiles, agents, threats, obstacles)) return false;
    const std::less<const CrowdAgent*> before;
    if (!agents.empty() && before(agents.data(), output.data() + agents.size()) && before(output.data(), agents.data() + agents.size())) return false;
    const float elapsed = s.deltaSeconds;
    const float dt = std::min(elapsed, s.maximumDelta);
    const std::size_t count = agents.size();
    CrowdStats result;
    w.Reserve(count);
    if (++w.m_stamp == 0) { for (auto& cell : w.m_cells) cell.stamp = 0; ++w.m_stamp; }
    float cellSize = 0.001f;
    for (const auto& p : profiles) cellSize = std::max(cellSize, p.neighborRadius);
    const auto coord = [cellSize](float position) { return static_cast<int>(std::floor(position / cellSize)); };
    const std::size_t mask = w.m_cells.size() - 1;
    const auto cellSlot = [&](int x, int y, int z) {
        std::size_t slot = Hash(x, y, z) & mask;
        while (w.m_cells[slot].stamp == w.m_stamp && (w.m_cells[slot].x != x || w.m_cells[slot].y != y || w.m_cells[slot].z != z)) slot = (slot + 1) & mask;
        return slot;
    };
    for (std::size_t i = 0; i < count; ++i) {
        const auto& a = agents[i];
        if (!(a.flags & CrowdFlags::ACTIVE)) continue;
        const int x = coord(a.position.x), y = coord(a.position.y), z = coord(a.position.z);
        auto& cell = w.m_cells[cellSlot(x, y, z)];
        if (cell.stamp != w.m_stamp) { cell = {x, y, z, -1, w.m_stamp}; ++result.occupiedCells; }
        w.m_next[i] = cell.head; cell.head = static_cast<int>(i);
    }
    std::copy(agents.begin(), agents.end(), output.begin());
    for (std::size_t i = 0; i < count; ++i) {
        auto& a = output[i]; a.events = 0;
        if (!(a.flags & CrowdFlags::ACTIVE)) continue;
        const bool hadThreat = a.threatIn > 0;
        a.decisionIn -= elapsed; a.decisionAge += elapsed;
        a.threatIn = std::max(0.0f, a.threatIn - elapsed); a.fear = std::max(0.0f, a.fear - s.fearDecay * elapsed);
        float strongest = 0;
        for (const auto& t : threats) {
            if (t.group != UINT32_MAX && t.group != a.group) continue;
            if ((a.position - t.position).LengthSq() > t.radius * t.radius || t.strength <= strongest) continue;
            strongest = t.strength; a.dangerPosition = t.position;
            a.fear = std::max(a.fear, t.strength); a.threatIn = std::max(a.threatIn, std::max(t.memory, dt));
        }
        if (!hadThreat && a.threatIn > 0) { a.events |= CrowdEvents::THREAT_ENTERED; ++result.threatReactions; }
        if (hadThreat && a.threatIn <= 0) a.events |= CrowdEvents::THREAT_LEFT;
    }
    const std::size_t start = count ? s.decisionCursor % count : 0;
    std::size_t lastDecision = start;
    /// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine Simulation LOD separates decision frequency from movement.
    for (std::size_t offset = 0; offset < count; ++offset) {
        const std::size_t i = (start + offset) % count;
        auto& a = output[i];
        if (!(a.flags & CrowdFlags::ACTIVE) || (a.decisionIn > 0 && !(a.flags & CrowdFlags::REQUEST_DECISION))) continue;
        if (result.decisions >= s.maximumDecisions) { ++result.deferredDecisions; continue; }
        ++result.decisions; lastDecision = (i + 1) % count;
        const auto& p = profiles[a.profile];
        const float radius = p.bodyRadius;
        V lo = s.minimum + V{radius, radius, radius}, hi = s.maximum - V{radius, radius, radius};
        lo.y = std::max(lo.y, a.floor + radius); hi.y = std::min(hi.y, a.ceiling - radius);
        if (lo.y > hi.y) lo.y = hi.y = std::clamp((a.floor + a.ceiling) * 0.5f, s.minimum.y + radius, s.maximum.y - radius);
        a.flags &= ~CrowdFlags::REQUEST_DECISION;
        a.decisionAge = 0; a.decisionIn = Interval(s, a.position);
        if (a.intent == CrowdIntent::WANDER) {
            a.goal = Clamp(a.goal, lo, hi);
            if ((a.goal - a.position).LengthSq() < std::max(p.arrivalRadius * p.arrivalRadius, 0.01f)) a.goal = RandomPoint(a.seed, lo, hi);
        }
        V goal = a.goal;
        if (a.intent == CrowdIntent::PURSUE) {
            const float prediction = std::min((a.target - a.position).Length() / std::max(p.cruiseSpeed, EPS), 2.0f);
            goal = a.target + a.targetVelocity * prediction;
        }
        V heading = Unit(Clamp(goal, lo, hi) - a.position, Unit(a.velocity));
        if (a.intent == CrowdIntent::FLEE) heading = Unit(a.position - a.target);
        V cohesion{}, alignment{}, separation{}; std::uint32_t tests = 0, neighbors = 0;
        a.sensedIndex = -1; a.sensedDistance = 0;
        float nearestDistanceSq = p.neighborRadius * p.neighborRadius;
        const int cx = coord(a.position.x), cy = coord(a.position.y), cz = coord(a.position.z);
        /// @see https://www.red3d.com/cwr/boids/ Local separation, alignment and cohesion avoid all-to-all work.
        for (int z = -1; z <= 1 && tests < s.candidateLimit && neighbors < s.neighborLimit; ++z)
            for (int y = -1; y <= 1 && tests < s.candidateLimit && neighbors < s.neighborLimit; ++y)
                for (int x = -1; x <= 1 && tests < s.candidateLimit && neighbors < s.neighborLimit; ++x) {
                    const auto& cell = w.m_cells[cellSlot(cx + x, cy + y, cz + z)];
                    if (cell.stamp != w.m_stamp) continue;
                    for (int j = cell.head; j >= 0 && tests < s.candidateLimit && neighbors < s.neighborLimit; j = w.m_next[j]) {
                        if (static_cast<std::size_t>(j) == i) continue;
                        ++tests; const auto& n = agents[j];
                        const V away = a.position - n.position; const float distanceSq = away.LengthSq();
                        if (distanceSq > p.neighborRadius * p.neighborRadius) continue;
                        if ((a.senseGroup == UINT32_MAX || a.senseGroup == n.group) && distanceSq <= nearestDistanceSq) {
                            nearestDistanceSq = distanceSq; a.sensedIndex = j; a.sensedDistance = std::sqrt(distanceSq);
                        }
                        if (n.group != a.group) continue;
                        ++neighbors; cohesion += n.position; alignment += n.velocity;
                        if (distanceSq < p.separationRadius * p.separationRadius) {
                            const V fallback = Unit(V{static_cast<float>((a.seed & 255) + 1), 0, static_cast<float>((a.seed >> 8) & 255) - 128});
                            separation += distanceSq > EPS * EPS ? away / std::max(distanceSq, 0.01f) : fallback;
                        }
                        if (n.threatIn > 0 && n.fear > a.fear + 0.05f) {
                            if (a.threatIn <= 0) { a.events |= CrowdEvents::THREAT_ENTERED; ++result.threatReactions; }
                            a.fear = n.fear * 0.8f; a.threatIn = std::max(a.threatIn, s.threatMemory); a.dangerPosition = n.dangerPosition;
                        }
                    }
                }
        result.candidateTests += tests; result.neighbors += neighbors;
        V desired = heading * p.goalWeight;
        if (neighbors) desired += Unit(cohesion / static_cast<float>(neighbors) - a.position, V{}) * p.cohesion + Unit(alignment, V{}) * p.alignment + Limit(separation, 3) * p.separation;
        float speed = p.cruiseSpeed;
        if (a.intent == CrowdIntent::ARRIVE) speed *= std::clamp((goal - a.position).Length() / std::max(p.slowingRadius, EPS), 0.0f, 1.0f);
        if (a.intent == CrowdIntent::HOLD) speed = 0;
        a.desiredVelocity = Unit(desired, Unit(a.velocity)) * speed;
    }
    result.decisionCursor = static_cast<std::uint32_t>(lastDecision);
    for (std::size_t i = 0; i < count; ++i) {
        auto& a = output[i]; if (!(a.flags & CrowdFlags::ACTIVE)) continue;
        const auto& p = profiles[a.profile]; const CrowdMode previousMode = a.mode;
        a.mode = Mode(a); if (a.mode != previousMode) a.events |= CrowdEvents::MODE_CHANGED;
        const float radius = p.bodyRadius;
        V lo = s.minimum + V{radius, radius, radius}, hi = s.maximum - V{radius, radius, radius};
        lo.y = std::max(lo.y, a.floor + radius); hi.y = std::min(hi.y, a.ceiling - radius);
        const bool blocked = lo.y > hi.y;
        if (blocked) { lo.y = hi.y = std::clamp((a.floor + a.ceiling) * 0.5f, s.minimum.y + radius, s.maximum.y - radius); }
        V desired = a.desiredVelocity;
        if ((a.flags & CrowdFlags::OVERRIDE_STEERING) || a.intent == CrowdIntent::EXTERNAL) desired = a.externalVelocity;
        if (a.mode == CrowdMode::FLEE) desired = Unit(a.position - (a.threatIn > 0 ? a.dangerPosition : a.target)) * p.fleeSpeed;
        if (p.followFloor) desired.y += (std::clamp(a.floor + p.floorOffset + radius, lo.y, hi.y) - a.position.y) * p.floorAttraction;
        const V ahead = a.position + a.velocity * s.avoidanceLookahead;
        for (const auto& o : obstacles) {
            const V away = ahead - o.position; const float safe = o.radius + radius + p.separationRadius;
            if (away.LengthSq() < safe * safe) desired += Unit(away, Unit(a.position - o.position)) * p.maximumAcceleration;
        }
        if (a.position.x < lo.x + radius) desired.x += p.maximumAcceleration;
        if (a.position.x > hi.x - radius) desired.x -= p.maximumAcceleration;
        if (a.position.y < lo.y + radius) desired.y += p.maximumAcceleration;
        if (a.position.y > hi.y - radius) desired.y -= p.maximumAcceleration;
        if (a.position.z < lo.z + radius) desired.z += p.maximumAcceleration;
        if (a.position.z > hi.z - radius) desired.z -= p.maximumAcceleration;
        const float maximumSpeed = a.mode == CrowdMode::FLEE ? p.fleeSpeed : p.cruiseSpeed;
        if (a.intent == CrowdIntent::ARRIVE && a.mode != CrowdMode::FLEE && (a.goal - a.position).Length() <= p.arrivalRadius) desired = {};
        desired = Turn(a.velocity, Limit(desired, maximumSpeed), p.maximumTurnRate * dt);
        const V previousVelocity = a.velocity;
        a.velocity += Limit(desired - a.velocity, p.maximumAcceleration * dt);
        V next = a.position + a.velocity * dt;
        if (a.intent == CrowdIntent::ARRIVE && a.mode != CrowdMode::FLEE && (a.goal - next).Length() <= p.arrivalRadius) {
            if ((a.goal - a.position).Length() > p.arrivalRadius) a.events |= CrowdEvents::ARRIVED;
            a.desiredVelocity = {};
        }
        for (const auto& o : obstacles) {
            const V away = next - o.position; const float safe = o.radius + radius;
            if (away.LengthSq() < safe * safe) { next = o.position + Unit(away, Unit(a.velocity)) * safe; a.events |= CrowdEvents::CONSTRAINED; }
        }
        const V constrained = Clamp(next, lo, hi);
        if ((constrained - next).LengthSq() > EPS * EPS || blocked) a.events |= CrowdEvents::CONSTRAINED;
        next = constrained;
        if (blocked) a.velocity = {};
        if (a.events & CrowdEvents::CONSTRAINED) ++result.constraints;
        if ((next - a.position).LengthSq() < 0.000001f && a.desiredVelocity.LengthSq() > 0.01f) a.stuckSeconds += dt;
        else a.stuckSeconds = 0;
        if (a.stuckSeconds >= 2) {
            a.desiredVelocity = Unit(RandomPoint(a.seed, {-1, -0.25f, -1}, {1, 0.25f, 1})) * p.cruiseSpeed;
            a.decisionIn = std::max(a.decisionIn, 0.5f); a.stuckSeconds = 0; a.events |= CrowdEvents::RECOVERED; ++result.recoveries;
        }
        if (dt > 0) result.maximumAcceleration = std::max(result.maximumAcceleration, (a.velocity - previousVelocity).Length() / dt);
        if (a.velocity.LengthSq() > EPS * EPS && previousVelocity.LengthSq() > EPS * EPS) result.maximumTurn = std::max(result.maximumTurn, std::acos(std::clamp(V::Dot(Unit(previousVelocity), Unit(a.velocity)), -1.0f, 1.0f)));
        a.position = next;
    }
    stats = result; return true;
}
}
