/// @file    BehaviorTreeEvaluator.hpp
/// @brief   木の評価とエージェントごとの実行状態。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once
#include <Engine/AI/BehaviorTreeRuntime.hpp>
#include <Engine/AI/Blackboard.hpp>
#include <Engine/Scene/Entity.hpp>

#include <cstdint>
#include <vector>

namespace fbzz::scene { class Scene; }
namespace fbzz::physics { class World; }

namespace fbzz::ai {

/// @brief エージェント 1 体ぶんの実行状態。木そのものは共有なので、可変なのはここだけ。
struct BTInstanceState {
    std::vector<std::uint8_t> flags;    ///< ノードごとのビットフラグ (BTNodeFlag)
    std::vector<std::uint16_t> cursor;  ///< Composite の現在の子スロット / Repeat の完了回数 / RandomSelector の選択結果
    std::vector<float> timers;          ///< Wait の経過 / TimeLimit の経過。中断でクリアされる。
    /// @note Cooldown の残り時間。中断でクリアしない。timers と分けるのは、中断で消すと「中断→即再突入」でクールダウンが無効化され連打できてしまうため。
    std::vector<float> cooldowns;

    std::uint16_t runningLeaf = kInvalidNode;   ///< 現在 Running のリーフ。kInvalidNode なら実行中でない。

    /// @note RandomSelector 用の決定的 RNG 状態。インスタンスごとに持つのは、同じ木を使う敵が全員同じ選択をすると群れが機械的に見え、テストでは種を固定して再現する必要があるため。
    std::uint32_t rngState = 0x9E3779B9u;

    void Resize(std::size_t nodeCount);
    /// @brief 全状態を初期化する (木の差し替え / Restart)。
    void Clear();
    /// @brief [lo, hi) の範囲だけ初期化する。cooldowns は触らない。
    void ClearRange(std::uint16_t lo, std::uint16_t hi);
};

namespace BTNodeFlag {
inline constexpr std::uint8_t Running     = 1 << 0;  ///< 前回 Running を返した
inline constexpr std::uint8_t Done        = 1 << 1;  ///< Parallel の子: 完了済み
inline constexpr std::uint8_t DoneSuccess = 1 << 2;  ///< Parallel の子: 完了結果が Success
inline constexpr std::uint8_t Entered     = 1 << 3;  ///< 一度でも実行に入った (Wait の初期化用)
} // namespace BTNodeFlag

class IBTActionHandler;

/// @brief 木の評価に必要な外界。scene / world が null でも純粋ノードは動く (テスト用)。
/// @note IBTActionHandler より先に定義するのは、ハンドラのメソッドが BTTickContext& を受け取り完全型を要求するため。逆向きは不完全型で足りる。
struct BTTickContext {
    scene::Scene*    scene = nullptr;
    physics::World*  world = nullptr;
    scene::EntityID  self  = scene::EntityID::INVALID;
    float            dt    = 0.0f;
    std::uint32_t    tick  = 0;

    IBTActionHandler* actions = nullptr;

    /// @brief エディタの実行中ハイライト用。null なら記録しない。nodes.size() 分の配列で、0=未評価/1=Success/2=Failure/3=Running。
    std::vector<std::uint8_t>* outNodeStatus = nullptr;
};

/// @brief Scene に触るノード (MoveTo / Patrol / PlayAnimation ...) の実行を外へ委ねるフック。
/// @note 評価器は木の意味論だけに責任を持たせるため分離する。Scene も NavMesh も知らなければ、Sequence の Running 伝播や Parallel の成功判定をヘッドレスでテストできる。段階 1 では null (該当ノードは Failure)。段階 3 で実装を差す。
class IBTActionHandler {
public:
    virtual ~IBTActionHandler() = default;

    /// @brief リーフアクションを 1 tick 実行する。
    virtual BTStatus Execute(const BehaviorTreeRuntime& tree,
                             std::uint16_t node,
                             BTInstanceState& state,
                             Blackboard& blackboard,
                             BTTickContext& ctx) = 0;

    /// @brief Running 中のリーフが中断されたときの後始末 (agent->Stop() など)。
    virtual void Abort(const BehaviorTreeRuntime& tree,
                       std::uint16_t node,
                       BTInstanceState& state,
                       BTTickContext& ctx)
    {
        (void)tree; (void)node; (void)state; (void)ctx;
    }

    /// @brief Scene に触らない純粋条件のうち知覚結果を必要とするもの (HasTarget / IsTargetInRange / IsHealthBelow / HasLineOfSight) を評価する。
    /// @note observerAborts のチェックからも呼ばれるため、副作用のない経路であることを型で示すために分離している。
    [[nodiscard]] virtual bool EvaluateCondition(const BehaviorTreeRuntime& tree,
                                                 std::uint16_t node,
                                                 const Blackboard& blackboard,
                                                 const BTTickContext& ctx) const
    {
        (void)tree; (void)node; (void)blackboard; (void)ctx;
        return false;
    }
};

/// @brief 木を 1 tick 進める。
BTStatus TickBehaviorTree(const BehaviorTreeRuntime& tree,
                          BTInstanceState& state,
                          Blackboard& blackboard,
                          BTTickContext& ctx);

/// @brief 副作用なしで条件ノードを評価する (observerAborts のチェックとテスト用)。
[[nodiscard]] bool EvaluateBTCondition(const BehaviorTreeRuntime& tree,
                                       std::uint16_t node,
                                       const Blackboard& blackboard,
                                       const BTTickContext& ctx);

} // namespace fbzz::ai
