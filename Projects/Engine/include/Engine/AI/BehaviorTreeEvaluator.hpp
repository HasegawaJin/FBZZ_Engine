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

// エージェント 1 体ぶんの実行状態。木そのものは共有なので、可変なのはここだけ。
struct BTInstanceState {
    // ノードごとのビットフラグ (BTNodeFlag)
    std::vector<std::uint8_t> flags;
    // Composite の現在の子スロット / Repeat の完了回数 / RandomSelector の選択結果
    std::vector<std::uint16_t> cursor;
    // Wait の経過 / TimeLimit の経過。**中断でクリアされる**
    std::vector<float> timers;
    // Cooldown の残り時間。**中断でクリアしない**
    //
    // WHY timers と分けるか: 中断で消してしまうと「中断 → 即再突入」で
    //     クールダウンが無効化され、攻撃が連打できてしまう。
    std::vector<float> cooldowns;

    // 現在 Running のリーフ。kInvalidNode なら実行中でない。
    std::uint16_t runningLeaf = kInvalidNode;

    // RandomSelector 用の決定的 RNG 状態。
    // WHY インスタンスごとに持つか: 同じ木を使う敵が全員同じ選択をすると
    //     群れが機械的に見える。かつテストでは種を固定して再現できる必要がある。
    std::uint32_t rngState = 0x9E3779B9u;

    void Resize(std::size_t nodeCount);
    // 全状態を初期化する (木の差し替え / Restart)。
    void Clear();
    // [lo, hi) の範囲だけ初期化する。cooldowns は触らない。
    void ClearRange(std::uint16_t lo, std::uint16_t hi);
};

namespace BTNodeFlag {
inline constexpr std::uint8_t Running     = 1 << 0;  // 前回 Running を返した
inline constexpr std::uint8_t Done        = 1 << 1;  // Parallel の子: 完了済み
inline constexpr std::uint8_t DoneSuccess = 1 << 2;  // Parallel の子: 完了結果が Success
inline constexpr std::uint8_t Entered     = 1 << 3;  // 一度でも実行に入った (Wait の初期化用)
} // namespace BTNodeFlag

class IBTActionHandler;

// 木の評価に必要な外界。scene / world が null でも純粋ノードは動く (テスト用)。
//
// WHY IBTActionHandler より先に定義するか: ハンドラのメソッドが
//     BTTickContext& を受け取るため、こちらが完全型になっている必要がある。
//     逆向き (Context が持つのはポインタ) は不完全型で足りる。
struct BTTickContext {
    scene::Scene*    scene = nullptr;
    physics::World*  world = nullptr;
    scene::EntityID  self  = scene::EntityID::INVALID;
    float            dt    = 0.0f;
    std::uint32_t    tick  = 0;

    IBTActionHandler* actions = nullptr;

    // エディタの実行中ハイライト用。null なら記録しない。
    // nodes.size() 分の配列で、0 = 未評価 / 1 = Success / 2 = Failure / 3 = Running。
    std::vector<std::uint8_t>* outNodeStatus = nullptr;
};

// Scene に触るノード (MoveTo / Patrol / PlayAnimation ...) の実行を外へ委ねるフック。
//
// WHY 評価器から分離するか:
//   評価器は「木の意味論」だけに責任を持たせたい。Scene も NavMesh も知らなければ、
//   Sequence の Running 伝播や Parallel の成功判定をヘッドレスでテストできる。
//   段階 1 では null (= それらのノードは Failure)。段階 3 で実装を差す。
class IBTActionHandler {
public:
    virtual ~IBTActionHandler() = default;

    // リーフアクションを 1 tick 実行する。
    virtual BTStatus Execute(const BehaviorTreeRuntime& tree,
                             std::uint16_t node,
                             BTInstanceState& state,
                             Blackboard& blackboard,
                             BTTickContext& ctx) = 0;

    // Running 中のリーフが中断されたときの後始末 (agent->Stop() など)。
    virtual void Abort(const BehaviorTreeRuntime& tree,
                       std::uint16_t node,
                       BTInstanceState& state,
                       BTTickContext& ctx)
    {
        (void)tree; (void)node; (void)state; (void)ctx;
    }

    // Scene に触らない純粋条件のうち、知覚結果を必要とするもの
    // (HasTarget / IsTargetInRange / IsHealthBelow / HasLineOfSight) を評価する。
    // WHY 分けるか: これらは observerAborts のチェックからも呼ばれるため、
    //     副作用のない経路であることを型で示す。
    [[nodiscard]] virtual bool EvaluateCondition(const BehaviorTreeRuntime& tree,
                                                 std::uint16_t node,
                                                 const Blackboard& blackboard,
                                                 const BTTickContext& ctx) const
    {
        (void)tree; (void)node; (void)blackboard; (void)ctx;
        return false;
    }
};

// 木を 1 tick 進める。
BTStatus TickBehaviorTree(const BehaviorTreeRuntime& tree,
                          BTInstanceState& state,
                          Blackboard& blackboard,
                          BTTickContext& ctx);

// 副作用なしで条件ノードを評価する (observerAborts のチェックとテスト用)。
[[nodiscard]] bool EvaluateBTCondition(const BehaviorTreeRuntime& tree,
                                       std::uint16_t node,
                                       const Blackboard& blackboard,
                                       const BTTickContext& ctx);

} // namespace fbzz::ai
