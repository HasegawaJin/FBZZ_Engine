/// @file    CrowdSimulation.hpp
/// @brief   呼び出し側のスナップショットを使う、処理量を制限した 3D 群衆の判断と連続的な操舵。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include <Engine/Api.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
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

/// @note 距離はワールド空間の [m]、間隔と記憶時間は [s]。1 シミュレーションで 1 生息域を扱う。
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

/// @note グループ化は個体単位。プロファイルは魚固有の処理を持たず、種別や移動方針を定義する。
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

/// @note 入力と出力のスナップショットは別々に保持する。乱数状態、判断、脅威の記憶は個体ごとに持つ。
/// @note 上下限は地形サンプルからキャッシュしたワールド Y の包絡。3D 経路探索は行わない。
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

/// @brief バッチ間で一時領域の容量を再利用する。ワールドや個体の識別子は保持しない。
class CrowdWorkspace {
public:
    /// @brief 計測やリアルタイム処理を始める前に必要な容量を確保する。
    FBZZ_ENGINE_API void Reserve(std::size_t capacity);
private:
    /// @note 球はバッチごとに再構築する。候補は元入力の添字で、StepCrowd の外部へ渡さない。
    struct SpatialIndex {
        struct Bounds { double minimum[3]{}, maximum[3]{}; };
        struct Node { Bounds bounds; std::uint16_t begin = 0, end = 0, left = 0, right = 0; };
        std::vector<Bounds> m_bounds;
        std::vector<Node> m_nodes;
        std::vector<std::uint16_t> m_order;
        std::size_t m_count = 0;
        std::size_t m_nodeCount = 0;
        FBZZ_ENGINE_API void Reserve(std::size_t capacity);
        template<class Sphere> void Build(std::span<const Sphere> spheres);
        /// @note 問い合わせが安全に行えない場合や探索量が固定上限を超えた場合は、全添字を入力順で返す。
        FBZZ_ENGINE_API std::size_t Query(math::Vector3 position, float radius, std::span<std::uint16_t> candidates) const;
        FBZZ_ENGINE_API std::size_t All(std::span<std::uint16_t> candidates) const;
        /// @note オペランドの大きさに応じた浮動小数点の丸め余白を境界に加え、従来の球判定を最終判定に使う。
        FBZZ_ENGINE_API static Bounds MakeBounds(math::Vector3 position, float radius);
        FBZZ_ENGINE_API static bool Overlaps(const Bounds& left, const Bounds& right);
    };
    struct Cell { int x = 0, y = 0, z = 0, head = -1; std::uint32_t stamp = 0; };
    std::vector<Cell> m_cells;
    std::vector<int> m_next;
    std::uint32_t m_stamp = 0;
    SpatialIndex m_threats, m_obstacles;
    friend FBZZ_ENGINE_API bool StepCrowd(const CrowdSettings&, std::span<const CrowdProfile>, std::span<const CrowdAgent>,
                          std::span<const CrowdThreat>, std::span<const CrowdObstacle>, std::span<CrowdAgent>,
                          CrowdStats&, CrowdWorkspace&);
};

/// @brief Scene に触れず、処理量を制限した判断と連続的な移動を 1 回ずつ行う。
/// @return 不正な入力や同じ領域を参照する入出力では false を返し、出力と統計は変更しない。
/// @note 判断の上限は決定的な処理回数で指定する。脅威回避の操舵は上限の対象外。
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine
/// @see https://www.red3d.com/cwr/boids/
[[nodiscard]] FBZZ_ENGINE_API bool StepCrowd(const CrowdSettings& settings, std::span<const CrowdProfile> profiles,
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
