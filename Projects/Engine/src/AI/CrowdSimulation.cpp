/// @file    CrowdSimulation.cpp
/// @brief   処理量を制限した空間検索と判断、および安全制約付きの 3D 操舵。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include <Engine/AI/CrowdSimulation.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <functional>
#include <limits>

namespace fbzz::ai {
namespace {
using V = math::Vector3;
constexpr float EPS = 0.00001f;
constexpr std::size_t SPHERE_LINEAR_LIMIT = 8;
constexpr std::size_t SPHERE_NODE_BUDGET = 64;
constexpr std::size_t PROJECTION_REQUERY_LIMIT = 4;
constexpr std::size_t MAX_AGENTS = 4096;
constexpr std::size_t MAX_PROFILES = 64;
constexpr std::size_t MAX_THREATS = 64;
constexpr std::size_t MAX_OBSTACLES = 256;
constexpr std::size_t MAX_NEIGHBOR_CANDIDATES = 256;
constexpr std::uint32_t VALID_AGENT_FLAGS = CrowdFlags::ACTIVE | CrowdFlags::REQUEST_DECISION | CrowdFlags::OVERRIDE_STEERING;

float Axis(V value, int axis) {
    if (axis == 0) {
        return value.x;
    }
    return axis == 1 ? value.y : value.z;
}
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
float Interval(const CrowdSettings& settings, V position) {
    const float observerDistanceSq = (position - settings.observer).LengthSq();
    if (observerDistanceSq <= settings.nearDistance * settings.nearDistance) {
        return settings.nearInterval;
    }
    if (observerDistanceSq <= settings.midDistance * settings.midDistance) {
        return settings.midInterval;
    }
    return settings.farInterval;
}
CrowdMode Mode(const CrowdAgent& a) {
    if (a.threatIn > 0 || a.intent == CrowdIntent::FLEE) {
        return CrowdMode::FLEE;
    }
    if (a.intent == CrowdIntent::PURSUE) {
        return CrowdMode::PURSUE;
    }
    if (a.intent == CrowdIntent::ARRIVE) {
        return CrowdMode::ARRIVE;
    }
    if (a.intent == CrowdIntent::HOLD) {
        return CrowdMode::HOLD;
    }
    return CrowdMode::CRUISE;
}
/// @brief 生息域と判断・移動の設定がシミュレーションの入力範囲に収まるか調べる。
/// @param[in] settings 生息域と観測者のワールド座標、判断の頻度と上限、移動の時間設定。
bool ValidSettings(const CrowdSettings& settings) {
    // グリッドに登録する座標と、個体を収める生息域の各軸を確認する。
    const bool validHabitat = Vector(settings.minimum) && Vector(settings.maximum) && Vector(settings.observer)
        && settings.minimum.x < settings.maximum.x && settings.minimum.y < settings.maximum.y
        && settings.minimum.z < settings.maximum.z;
    if (!validHabitat) {
        return false;
    }

    // 観測距離の区分と、その区分で使う判断間隔を確認する。
    const bool validDecisionSchedule = Nonnegative(settings.nearDistance) && Nonnegative(settings.midDistance)
        && settings.nearDistance <= settings.midDistance && Nonnegative(settings.nearInterval)
        && Nonnegative(settings.midInterval) && Nonnegative(settings.farInterval);
    if (!validDecisionSchedule) {
        return false;
    }

    // 実経過時間と、移動の積分に使える刻みの上限を確認する。
    const bool validTimeStep = Nonnegative(settings.maximumDelta) && settings.maximumDelta > 0.0f
        && settings.maximumDelta <= 1.0f && Nonnegative(settings.deltaSeconds);
    if (!validTimeStep) {
        return false;
    }

    // 探索で採用する近傍数は候補数以内とし、更新全体での判断数も制限する。
    const bool validDecisionBudget = settings.candidateLimit != 0 && settings.candidateLimit <= MAX_NEIGHBOR_CANDIDATES
        && settings.neighborLimit != 0 && settings.neighborLimit <= settings.candidateLimit
        && settings.maximumDecisions <= MAX_AGENTS;
    if (!validDecisionBudget) {
        return false;
    }

    return Nonnegative(settings.fearDecay) && Nonnegative(settings.threatMemory)
        && Nonnegative(settings.avoidanceLookahead) && settings.reserved == 0;
}

/// @brief 近傍の探索、操舵、床追従に使うプロファイルを検証する。
/// @param[in] profile 個体の操舵設定。半径はワールド空間の [m]。
/// @param[in] settings 検証済みの生息域。個体の直径が全軸に収まるか確認する。
bool ValidProfile(const CrowdProfile& profile, const CrowdSettings& settings) {
    // 分離に使う近傍は、グリッドで調べる近傍半径に含まれる必要がある。
    if (!Nonnegative(profile.neighborRadius) || profile.neighborRadius < 0.001f) {
        return false;
    }
    if (!Nonnegative(profile.separationRadius) || profile.separationRadius > profile.neighborRadius) {
        return false;
    }
    const bool invalidSteeringWeights = !Nonnegative(profile.cohesion) || !Nonnegative(profile.alignment)
        || !Nonnegative(profile.separation) || !Nonnegative(profile.goalWeight);
    if (invalidSteeringWeights) {
        return false;
    }

    // 巡航と逃走の速度、および一度の積分で使う加速・旋回の上限を確認する。
    if (!Nonnegative(profile.cruiseSpeed) || profile.cruiseSpeed > 1000
        || !Nonnegative(profile.fleeSpeed) || profile.fleeSpeed > 1000) {
        return false;
    }
    if (!Nonnegative(profile.maximumAcceleration) || profile.maximumAcceleration > 10000
        || !Nonnegative(profile.maximumTurnRate) || profile.maximumTurnRate > 1000) {
        return false;
    }

    // 生息域の全軸で、個体の半径を引いた後にも移動範囲が残る必要がある。
    if (!Nonnegative(profile.arrivalRadius) || !Nonnegative(profile.slowingRadius) || !Nonnegative(profile.bodyRadius)) {
        return false;
    }
    const float shortestHabitatExtent = std::min({
        settings.maximum.x - settings.minimum.x,
        settings.maximum.y - settings.minimum.y,
        settings.maximum.z - settings.minimum.z,
    });
    if (profile.bodyRadius * 2 >= shortestHabitatExtent) {
        return false;
    }
    return Nonnegative(profile.floorOffset) && Nonnegative(profile.floorAttraction) && profile.followFloor <= 1;
}

/// @brief 個体のスナップショットと、同じバッチ内の参照先を検証する。
/// @param[in] agent 位置・速度と前回の更新結果を持つ個体。
/// @param[in] profileCount バッチに含まれるプロファイル数。
/// @param[in] agentCount バッチに含まれる個体数。
bool ValidAgent(const CrowdAgent& agent, std::size_t profileCount, std::size_t agentCount) {
    // 操舵の入力と前回の脅威位置を検証してから、個体の上下限を確認する。
    if (!Vector(agent.position) || !Vector(agent.velocity) || !Vector(agent.desiredVelocity)) {
        return false;
    }
    if (!Vector(agent.goal) || !Vector(agent.target) || !Vector(agent.targetVelocity)
        || !Vector(agent.externalVelocity) || !Vector(agent.dangerPosition)) {
        return false;
    }
    if (!Scalar(agent.floor) || !Scalar(agent.ceiling) || agent.floor > agent.ceiling) {
        return false;
    }

    // 判断の残り時間は期限切れを表す負値も受け入れる。経過時間と記憶時間は非負。
    if (!Nonnegative(agent.fear) || agent.fear > 1) {
        return false;
    }
    if (!Scalar(agent.decisionIn) || !Nonnegative(agent.decisionAge)
        || !Nonnegative(agent.threatIn) || !Nonnegative(agent.stuckSeconds)) {
        return false;
    }

    // 列挙値と添字を確認し、走査中に範囲外のプロファイルや個体を参照しないようにする。
    if (agent.profile >= profileCount) {
        return false;
    }
    if (static_cast<std::uint32_t>(agent.intent) > static_cast<std::uint32_t>(CrowdIntent::EXTERNAL)
        || static_cast<std::uint32_t>(agent.mode) > static_cast<std::uint32_t>(CrowdMode::HOLD)
        || (agent.flags & ~VALID_AGENT_FLAGS)) {
        return false;
    }
    return Nonnegative(agent.sensedDistance) && agent.sensedIndex >= -1
        && agent.sensedIndex < static_cast<std::int32_t>(agentCount);
}

/// @brief スナップショット全体を、作業領域の確保や出力の書き込みより先に検証する。
/// @param[in] settings 生息域と更新の設定。
/// @param[in] profiles 個体が参照するプロファイル配列。
/// @param[in] agents 更新前の個体配列。
/// @param[in] threats ワールド空間の脅威球。
/// @param[in] obstacles ワールド空間の障害物球。
bool Valid(const CrowdSettings& settings, std::span<const CrowdProfile> profiles, std::span<const CrowdAgent> agents,
           std::span<const CrowdThreat> threats, std::span<const CrowdObstacle> obstacles) {
    // 配列の上限を確認し、後続の検証と空間索引の処理量を抑える。
    if (agents.size() > MAX_AGENTS || profiles.empty() || profiles.size() > MAX_PROFILES
        || threats.size() > MAX_THREATS || obstacles.size() > MAX_OBSTACLES) {
        return false;
    }
    if (!ValidSettings(settings)) {
        return false;
    }

    // 設定を基準にプロファイルを調べ、その後で個体からの参照を照合する。
    for (const auto& profile : profiles) {
        if (!ValidProfile(profile, settings)) {
            return false;
        }
    }
    for (const auto& agent : agents) {
        if (!ValidAgent(agent, profiles.size(), agents.size())) {
            return false;
        }
    }

    // 空間索引に渡す球と、脅威への反応に使う強度・記憶時間を確認する。
    const bool validThreats = std::all_of(threats.begin(), threats.end(), [](const CrowdThreat& threat) {
        return Vector(threat.position) && Nonnegative(threat.radius)
            && Nonnegative(threat.strength) && threat.strength <= 1.0f && Nonnegative(threat.memory);
    });
    if (!validThreats) {
        return false;
    }
    return std::all_of(obstacles.begin(), obstacles.end(), [](const CrowdObstacle& obstacle) {
        return Vector(obstacle.position) && Nonnegative(obstacle.radius);
    });
}
V Turn(V velocity, V desired, float maximumAngle) {
    const float speed = desired.Length();
    if (velocity.LengthSq() < EPS * EPS || speed < EPS) {
        return desired;
    }
    const V from = Unit(velocity), to = desired / speed;
    const float angle = std::acos(std::clamp(V::Dot(from, to), -1.0f, 1.0f));
    if (angle <= maximumAngle || angle < EPS) {
        return desired;
    }
    V tangent = to - from * V::Dot(from, to);
    if (tangent.LengthSq() < EPS * EPS) {
        tangent = V::Cross(from, std::abs(from.y) < 0.9f ? V{0, 1, 0} : V{1, 0, 0});
    }
    tangent = Unit(tangent);
    return (from * std::cos(maximumAngle) + tangent * std::sin(maximumAngle)) * speed;
}
}

void CrowdWorkspace::SpatialIndex::Reserve(std::size_t capacity) {
    if (m_bounds.size() < capacity) {
        m_bounds.resize(capacity);
    }
    if (m_order.size() < capacity) {
        m_order.resize(capacity);
    }
    if (m_nodes.size() < capacity * 2 - 1) {
        m_nodes.resize(capacity * 2 - 1);
    }
}

CrowdWorkspace::SpatialIndex::Bounds CrowdWorkspace::SpatialIndex::MakeBounds(V position, float radius) {
    Bounds result;
    constexpr double SLACK = 16.0 * std::numeric_limits<float>::epsilon();
    for (int axis = 0; axis < 3; ++axis) {
        const double center = Axis(position, axis);
        // 最終端点だけを広げても、丸めた球境界の近傍で 1000000 - 999990 のように桁落ちする場合を拾えない。
        const double margin = SLACK * (std::abs(center) + radius + 1.0);
        result.minimum[axis] = center - radius - margin;
        result.maximum[axis] = center + radius + margin;
    }
    return result;
}

bool CrowdWorkspace::SpatialIndex::Overlaps(const Bounds& left, const Bounds& right) {
    for (int axis = 0; axis < 3; ++axis)
        if (left.maximum[axis] < right.minimum[axis] || right.maximum[axis] < left.minimum[axis]) {
            return false;
        }
    return true;
}

/// @see https://pbr-book.org/4ed/Primitives_and_Intersection_Acceleration/Bounding_Volume_Hierarchies 木の探索順序によって、同じ強さの脅威の扱い、記憶時間、障害物への逐次投影が変わらないようにする。
template<class Sphere> void CrowdWorkspace::SpatialIndex::Build(std::span<const Sphere> spheres) {
    m_count = spheres.size(); m_nodeCount = 0;
    if (m_count <= SPHERE_LINEAR_LIMIT) {
        return;
    }
    for (std::size_t index = 0; index < m_count; ++index) {
        m_bounds[index] = MakeBounds(spheres[index].position, spheres[index].radius);
        m_order[index] = static_cast<std::uint16_t>(index);
    }
    const auto build = [&](auto&& self, std::uint16_t begin, std::uint16_t end) -> std::uint16_t {
        const auto index = static_cast<std::uint16_t>(m_nodeCount++);
        auto& node = m_nodes[index];
        node.begin = begin; node.end = end; node.left = node.right = 0;
        node.bounds = m_bounds[m_order[begin]];
        for (std::size_t item = begin + 1; item < end; ++item)
            for (int axis = 0; axis < 3; ++axis) {
                node.bounds.minimum[axis] = std::min(node.bounds.minimum[axis], m_bounds[m_order[item]].minimum[axis]);
                node.bounds.maximum[axis] = std::max(node.bounds.maximum[axis], m_bounds[m_order[item]].maximum[axis]);
            }
        if (end - begin <= 4) {
            return index;
        }
        int splitAxis = 0;
        for (int axis = 1; axis < 3; ++axis)
            if (node.bounds.maximum[axis] - node.bounds.minimum[axis] > node.bounds.maximum[splitAxis] - node.bounds.minimum[splitAxis]) {
                splitAxis = axis;
            }
        const auto middle = static_cast<std::uint16_t>(begin + (end - begin) / 2);
        std::nth_element(m_order.begin() + begin, m_order.begin() + middle, m_order.begin() + end,
            [&](std::uint16_t left, std::uint16_t right) {
                const float a = Axis(spheres[left].position, splitAxis), b = Axis(spheres[right].position, splitAxis);
                return a == b ? left < right : a < b;
            });
        node.left = self(self, begin, middle); node.right = self(self, middle, end);
        return index;
    };
    build(build, 0, static_cast<std::uint16_t>(m_count));
}

std::size_t CrowdWorkspace::SpatialIndex::All(std::span<std::uint16_t> candidates) const {
    assert(candidates.size() >= m_count);
    for (std::size_t index = 0; index < m_count; ++index) {
        candidates[index] = static_cast<std::uint16_t>(index);
    }
    return m_count;
}

std::size_t CrowdWorkspace::SpatialIndex::Query(V position, float radius, std::span<std::uint16_t> candidates) const {
    assert(candidates.size() >= m_count);
    if (!m_nodeCount || !Vector(position) || !Nonnegative(radius)) {
        return All(candidates);
    }
    const Bounds query = MakeBounds(position, radius);
    std::array<std::uint16_t, 16> stack{};
    std::size_t pending = 1, visited = 0, found = 0;
    while (pending) {
        if (++visited > SPHERE_NODE_BUDGET) {
            return All(candidates);
        }
        const auto& node = m_nodes[stack[--pending]];
        if (!Overlaps(node.bounds, query)) {
            continue;
        }
        if (node.left == node.right) {
            for (std::size_t item = node.begin; item < node.end; ++item)
                if (Overlaps(m_bounds[m_order[item]], query)) {
                    candidates[found++] = m_order[item];
                }
        } else {
            if (pending + 2 > stack.size()) {
                return All(candidates);
            }
            stack[pending++] = node.left; stack[pending++] = node.right;
        }
    }
    // プリミティブを等個数で分割し、二分木の格納量を抑える節。
    if (found == m_count) {
        return All(candidates);
    }
    std::sort(candidates.begin(), candidates.begin() + found);
    return found;
}

void CrowdWorkspace::Reserve(std::size_t capacity) {
    capacity = std::min(capacity, MAX_AGENTS);
    std::size_t cells = 8;
    while (cells < capacity * 2) {
        cells <<= 1;
    }
    if (m_cells.size() < cells) {
        m_cells.resize(cells);
    }
    if (m_next.size() < capacity) {
        m_next.resize(capacity);
    }
    m_threats.Reserve(MAX_THREATS);
    m_obstacles.Reserve(MAX_OBSTACLES);
}

bool StepCrowd(const CrowdSettings& s, std::span<const CrowdProfile> profiles, std::span<const CrowdAgent> agents,
               std::span<const CrowdThreat> threats, std::span<const CrowdObstacle> obstacles, std::span<CrowdAgent> output,
               CrowdStats& stats, CrowdWorkspace& w) {
    // 入力全体と出力の容量を検証する。失敗した場合は作業領域や出力を更新しない。
    if (output.size() < agents.size()) {
        return false;
    }
    if (!Valid(s, profiles, agents, threats, obstacles)) {
        return false;
    }

    // 近傍は更新前の位置を参照するため、実際に書き込む範囲が入力と重なる場合は処理できない。
    if (!agents.empty()) {
        const std::less<const CrowdAgent*> before;
        const auto* inputEnd = agents.data() + agents.size();
        const auto* outputEnd = output.data() + agents.size();
        const bool overlapsInput = before(agents.data(), outputEnd) && before(output.data(), inputEnd);
        if (overlapsInput) {
            return false;
        }
    }

    // 判断の時間は実経過で進め、移動の積分だけを最大時間刻みで制限する。
    const float elapsed = s.deltaSeconds;
    const float dt = std::min(elapsed, s.maximumDelta);
    const std::size_t count = agents.size();
    CrowdStats result;

    // 現在のバッチの球を索引に登録し、近傍を調べるグリッドは世代番号で再利用する。
    w.Reserve(count);
    w.m_threats.Build(threats);
    w.m_obstacles.Build(obstacles);
    std::array<std::uint16_t, std::max(MAX_THREATS, MAX_OBSTACLES)> sphereCandidates{};
    if (++w.m_stamp == 0) {
        for (auto& cell : w.m_cells) {
            cell.stamp = 0;
        }
        ++w.m_stamp;
    }
    float cellSize = 0.001f;
    for (const auto& p : profiles) {
        cellSize = std::max(cellSize, p.neighborRadius);
    }
    const auto coord = [cellSize](float position) { return static_cast<int>(std::floor(position / cellSize)); };
    const std::size_t mask = w.m_cells.size() - 1;
    const auto cellSlot = [&](int x, int y, int z) {
        std::size_t slot = Hash(x, y, z) & mask;
        while (w.m_cells[slot].stamp == w.m_stamp && (w.m_cells[slot].x != x || w.m_cells[slot].y != y || w.m_cells[slot].z != z)) {
            slot = (slot + 1) & mask;
        }
        return slot;
    };
    for (std::size_t i = 0; i < count; ++i) {
        const auto& a = agents[i];
        if (!(a.flags & CrowdFlags::ACTIVE)) {
            continue;
        }
        const int x = coord(a.position.x), y = coord(a.position.y), z = coord(a.position.z);
        auto& cell = w.m_cells[cellSlot(x, y, z)];
        if (cell.stamp != w.m_stamp) { cell = {x, y, z, -1, w.m_stamp}; ++result.occupiedCells; }
        w.m_next[i] = cell.head; cell.head = static_cast<int>(i);
    }
    // 有効な個体の脅威を先に更新し、判断の予算を使い切った場合も逃走に反映できるようにする。
    std::copy(agents.begin(), agents.end(), output.begin());
    for (std::size_t i = 0; i < count; ++i) {
        auto& a = output[i];
        a.events = 0;
        if (!(a.flags & CrowdFlags::ACTIVE)) {
            continue;
        }
        const bool hadThreat = a.threatIn > 0;
        a.decisionIn -= elapsed; a.decisionAge += elapsed;
        a.threatIn = std::max(0.0f, a.threatIn - elapsed); a.fear = std::max(0.0f, a.fear - s.fearDecay * elapsed);
        float strongest = 0;
        const std::size_t threatCount = w.m_threats.Query(a.position, 0, sphereCandidates);
        for (std::size_t candidate = 0; candidate < threatCount; ++candidate) {
            const auto& t = threats[sphereCandidates[candidate]];
            if (t.group != UINT32_MAX && t.group != a.group) {
                continue;
            }
            if ((a.position - t.position).LengthSq() > t.radius * t.radius || t.strength <= strongest) {
                continue;
            }
            strongest = t.strength; a.dangerPosition = t.position;
            a.fear = std::max(a.fear, t.strength); a.threatIn = std::max(a.threatIn, std::max(t.memory, dt));
        }
        if (!hadThreat && a.threatIn > 0) { a.events |= CrowdEvents::THREAT_ENTERED; ++result.threatReactions; }
        if (hadThreat && a.threatIn <= 0) {
            a.events |= CrowdEvents::THREAT_LEFT;
        }
    }
    // 前回の続きから順に判断し、見送られた個体にも次回の判断枠が回るようにする。
    const std::size_t start = count ? s.decisionCursor % count : 0;
    std::size_t lastDecision = start;
    /// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine Simulation LOD で判断の頻度を移動から分離する設計。
    for (std::size_t offset = 0; offset < count; ++offset) {
        const std::size_t i = (start + offset) % count;
        auto& a = output[i];
        if (!(a.flags & CrowdFlags::ACTIVE)) {
            continue;
        }
        const bool waitingForNextDecision = a.decisionIn > 0 && !(a.flags & CrowdFlags::REQUEST_DECISION);
        if (waitingForNextDecision) {
            continue;
        }
        if (result.decisions >= s.maximumDecisions) {
            ++result.deferredDecisions;
            continue;
        }
        ++result.decisions;
        lastDecision = (i + 1) % count;
        const auto& p = profiles[a.profile];
        const float radius = p.bodyRadius;
        V lo = s.minimum + V{radius, radius, radius}, hi = s.maximum - V{radius, radius, radius};
        lo.y = std::max(lo.y, a.floor + radius); hi.y = std::min(hi.y, a.ceiling - radius);
        if (lo.y > hi.y) {
            lo.y = hi.y = std::clamp((a.floor + a.ceiling) * 0.5f, s.minimum.y + radius, s.maximum.y - radius);
        }
        a.flags &= ~CrowdFlags::REQUEST_DECISION;
        a.decisionAge = 0; a.decisionIn = Interval(s, a.position);
        if (a.intent == CrowdIntent::WANDER) {
            a.goal = Clamp(a.goal, lo, hi);
            if ((a.goal - a.position).LengthSq() < std::max(p.arrivalRadius * p.arrivalRadius, 0.01f)) {
                a.goal = RandomPoint(a.seed, lo, hi);
            }
        }
        V goal = a.goal;
        if (a.intent == CrowdIntent::PURSUE) {
            const float prediction = std::min((a.target - a.position).Length() / std::max(p.cruiseSpeed, EPS), 2.0f);
            goal = a.target + a.targetVelocity * prediction;
        }
        V heading = Unit(Clamp(goal, lo, hi) - a.position, Unit(a.velocity));
        if (a.intent == CrowdIntent::FLEE) {
            heading = Unit(a.position - a.target);
        }
        V cohesion{}, alignment{}, separation{}; std::uint32_t tests = 0, neighbors = 0;
        a.sensedIndex = -1; a.sensedDistance = 0;
        float nearestDistanceSq = p.neighborRadius * p.neighborRadius;
        const int cx = coord(a.position.x), cy = coord(a.position.y), cz = coord(a.position.z);
        /// @see https://www.red3d.com/cwr/boids/ 近傍だけで分離・整列・結合を評価し、全個体の組を処理しない Boids の規則。
        for (int z = -1; z <= 1 && tests < s.candidateLimit && neighbors < s.neighborLimit; ++z)
            for (int y = -1; y <= 1 && tests < s.candidateLimit && neighbors < s.neighborLimit; ++y)
                for (int x = -1; x <= 1 && tests < s.candidateLimit && neighbors < s.neighborLimit; ++x) {
                    const auto& cell = w.m_cells[cellSlot(cx + x, cy + y, cz + z)];
                    if (cell.stamp != w.m_stamp) {
                        continue;
                    }
                    for (int j = cell.head; j >= 0 && tests < s.candidateLimit && neighbors < s.neighborLimit; j = w.m_next[j]) {
                        if (static_cast<std::size_t>(j) == i) {
                            continue;
                        }
                        ++tests; const auto& n = agents[j];
                        const V away = a.position - n.position; const float distanceSq = away.LengthSq();
                        if (distanceSq > p.neighborRadius * p.neighborRadius) {
                            continue;
                        }
                        if ((a.senseGroup == UINT32_MAX || a.senseGroup == n.group) && distanceSq <= nearestDistanceSq) {
                            nearestDistanceSq = distanceSq; a.sensedIndex = j; a.sensedDistance = std::sqrt(distanceSq);
                        }
                        if (n.group != a.group) {
                            continue;
                        }
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
        if (neighbors) {
            desired += Unit(cohesion / static_cast<float>(neighbors) - a.position, V{}) * p.cohesion + Unit(alignment, V{}) * p.alignment + Limit(separation, 3) * p.separation;
        }
        float speed = p.cruiseSpeed;
        if (a.intent == CrowdIntent::ARRIVE) {
            speed *= std::clamp((goal - a.position).Length() / std::max(p.slowingRadius, EPS), 0.0f, 1.0f);
        }
        if (a.intent == CrowdIntent::HOLD) {
            speed = 0;
        }
        a.desiredVelocity = Unit(desired, Unit(a.velocity)) * speed;
    }
    result.decisionCursor = static_cast<std::uint32_t>(lastDecision);

    // 判断頻度に関係なく操舵を適用し、障害物と生息域で次の位置を制約する。
    for (std::size_t i = 0; i < count; ++i) {
        auto& a = output[i];
        if (!(a.flags & CrowdFlags::ACTIVE)) {
            continue;
        }
        const auto& p = profiles[a.profile]; const CrowdMode previousMode = a.mode;
        a.mode = Mode(a); if (a.mode != previousMode) a.events |= CrowdEvents::MODE_CHANGED;
        const float radius = p.bodyRadius;
        V lo = s.minimum + V{radius, radius, radius}, hi = s.maximum - V{radius, radius, radius};
        lo.y = std::max(lo.y, a.floor + radius); hi.y = std::min(hi.y, a.ceiling - radius);
        const bool blocked = lo.y > hi.y;
        if (blocked) {
            lo.y = hi.y = std::clamp((a.floor + a.ceiling) * 0.5f, s.minimum.y + radius, s.maximum.y - radius);
        }
        V desired = a.desiredVelocity;
        if ((a.flags & CrowdFlags::OVERRIDE_STEERING) || a.intent == CrowdIntent::EXTERNAL) {
            desired = a.externalVelocity;
        }
        if (a.mode == CrowdMode::FLEE) {
            desired = Unit(a.position - (a.threatIn > 0 ? a.dangerPosition : a.target)) * p.fleeSpeed;
        }
        if (p.followFloor) {
            desired.y += (std::clamp(a.floor + p.floorOffset + radius, lo.y, hi.y) - a.position.y) * p.floorAttraction;
        }
        const V ahead = a.position + a.velocity * s.avoidanceLookahead;
        const std::size_t avoidanceCount = w.m_obstacles.Query(ahead, radius + p.separationRadius, sphereCandidates);
        for (std::size_t candidate = 0; candidate < avoidanceCount; ++candidate) {
            const auto& o = obstacles[sphereCandidates[candidate]];
            const V away = ahead - o.position; const float safe = o.radius + radius + p.separationRadius;
            if (away.LengthSq() < safe * safe) {
                desired += Unit(away, Unit(a.position - o.position)) * p.maximumAcceleration;
            }
        }
        if (a.position.x < lo.x + radius) {
            desired.x += p.maximumAcceleration;
        }
        if (a.position.x > hi.x - radius) {
            desired.x -= p.maximumAcceleration;
        }
        if (a.position.y < lo.y + radius) {
            desired.y += p.maximumAcceleration;
        }
        if (a.position.y > hi.y - radius) {
            desired.y -= p.maximumAcceleration;
        }
        if (a.position.z < lo.z + radius) {
            desired.z += p.maximumAcceleration;
        }
        if (a.position.z > hi.z - radius) {
            desired.z -= p.maximumAcceleration;
        }
        const float maximumSpeed = a.mode == CrowdMode::FLEE ? p.fleeSpeed : p.cruiseSpeed;
        if (a.intent == CrowdIntent::ARRIVE && a.mode != CrowdMode::FLEE && (a.goal - a.position).Length() <= p.arrivalRadius) {
            desired = {};
        }
        desired = Turn(a.velocity, Limit(desired, maximumSpeed), p.maximumTurnRate * dt);
        const V previousVelocity = a.velocity;
        a.velocity += Limit(desired - a.velocity, p.maximumAcceleration * dt);
        V next = a.position + a.velocity * dt;
        if (a.intent == CrowdIntent::ARRIVE && a.mode != CrowdMode::FLEE && (a.goal - next).Length() <= p.arrivalRadius) {
            if ((a.goal - a.position).Length() > p.arrivalRadius) {
                a.events |= CrowdEvents::ARRIVED;
            }
            a.desiredVelocity = {};
        }
        std::size_t projectionCount = w.m_obstacles.Query(next, radius, sphereCandidates), projectionQueries = 0;
        for (std::size_t candidate = 0; candidate < projectionCount;) {
            const auto obstacleIndex = sphereCandidates[candidate++];
            const auto& o = obstacles[obstacleIndex];
            const V away = next - o.position; const float safe = o.radius + radius;
            if (away.LengthSq() < safe * safe) {
                next = o.position + Unit(away, Unit(a.velocity)) * safe; a.events |= CrowdEvents::CONSTRAINED;
                if (projectionCount != obstacles.size()) {
                    // 投影によって前回の検索外にある後続の球へ進入し得る。それ以前の添字は元の走査で既に通過している。
                    projectionCount = projectionQueries++ < PROJECTION_REQUERY_LIMIT
                        ? w.m_obstacles.Query(next, radius, sphereCandidates) : w.m_obstacles.All(sphereCandidates);
                    candidate = static_cast<std::size_t>(std::lower_bound(sphereCandidates.begin(), sphereCandidates.begin() + projectionCount,
                        static_cast<std::uint16_t>(obstacleIndex + 1)) - sphereCandidates.begin());
                }
            }
        }
        const V constrained = Clamp(next, lo, hi);
        if ((constrained - next).LengthSq() > EPS * EPS || blocked) {
            a.events |= CrowdEvents::CONSTRAINED;
        }
        next = constrained;
        if (blocked) {
            a.velocity = {};
        }
        if (a.events & CrowdEvents::CONSTRAINED) {
            ++result.constraints;
        }
        if ((next - a.position).LengthSq() < 0.000001f && a.desiredVelocity.LengthSq() > 0.01f) {
            a.stuckSeconds += dt;
        }
        else {
            a.stuckSeconds = 0;
        }
        if (a.stuckSeconds >= 2) {
            a.desiredVelocity = Unit(RandomPoint(a.seed, {-1, -0.25f, -1}, {1, 0.25f, 1})) * p.cruiseSpeed;
            a.decisionIn = std::max(a.decisionIn, 0.5f); a.stuckSeconds = 0; a.events |= CrowdEvents::RECOVERED; ++result.recoveries;
        }
        if (dt > 0) {
            result.maximumAcceleration = std::max(result.maximumAcceleration, (a.velocity - previousVelocity).Length() / dt);
        }
        if (a.velocity.LengthSq() > EPS * EPS && previousVelocity.LengthSq() > EPS * EPS) {
            result.maximumTurn = std::max(result.maximumTurn, std::acos(std::clamp(V::Dot(Unit(previousVelocity), Unit(a.velocity)), -1.0f, 1.0f)));
        }
        a.position = next;
    }
    stats = result;
    return true;
}
}
