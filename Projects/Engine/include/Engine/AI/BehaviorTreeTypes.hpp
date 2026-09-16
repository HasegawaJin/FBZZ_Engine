/// @file    BehaviorTreeTypes.hpp
/// @brief   Behavior Tree の基本列挙と、ノード種別に対する述語。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// WHY Behavior Tree を選んだか (Docs/design/game-ai-layer.md):
/// - 有限状態機械は状態数 N に対して遷移が O(N^2) に増え、
/// 「全状態から被弾リアクションへ」のような横断遷移で爆発する。
/// - Utility AI はスコア関数の調整が非直感的で、
/// 「なぜこの行動を選んだか」のデバッグが著しく難しい。
/// - BT は木構造で優先順位が視覚的に読め、サブツリーの再利用が効く。
#pragma once
#include <cstdint>

namespace fbzz::ai {

// ノードの実行結果。
// Running は「まだ終わっていない」= 次の tick で続きから再開する、を意味する。
enum class BTStatus : std::uint8_t { Success, Failure, Running };

// Decorator が Running 中のサブツリーを中断できる条件。
//
// WHY LowerPriority が最重要か:
//   これが無いと「巡回中にプレイヤーを発見しても、現在のウェイポイントに
//   着くまで反応しない」という致命的に鈍い AI になる。
//   BT が FSM に対して優位を持つ最大の理由がこの中断機構であり、
//   省くと BT を採用する意味の大半が失われる。
enum class AbortMode : std::uint8_t {
    None = 0,       // 中断しない
    Self,           // 自分のサブツリーが Running 中、条件が偽に落ちたら中断
    LowerPriority,  // 自分より右 (低優先度) が Running 中、条件が真に立ったら割り込む
    Both,
};

// ノード種別。
//
// WHY 末尾追加のみ許されるか:
//   この数値は .behaviortree の TOML へ直接書かれる。途中へ挿入すると
//   既存アセットのノードが黙って別種別として読まれ、木が意味不明な挙動になる。
//   種別を廃止する場合も、値を詰めずに「未使用」として残すこと。
enum class BTNodeType : std::uint16_t {
    // ── Composite (子を複数持つ) ──
    Sequence = 0,     // 全子 Success で Success。1 つでも Failure で Failure
    Selector,         // 1 つでも Success で Success。全 Failure で Failure
    Parallel,         // 全子を同時実行。成功条件は successPolicy
    RandomSelector,   // 重み付きランダムで子を 1 つ選ぶ

    // ── Decorator (子を 1 つ持つ) ──
    Inverter,             // Success <-> Failure を反転
    Succeeder,            // 子の結果に関わらず Success
    Repeat,               // 子を N 回 (0 = 無限) 繰り返す
    Cooldown,             // 直前の実行から N 秒経つまで Failure
    BlackboardCondition,  // Blackboard の条件を満たすときだけ子を実行
    TimeLimit,            // N 秒を超えたら Failure で打ち切る

    // ── Leaf: Action ──
    MoveTo,
    Patrol,
    Wait,
    LookAt,
    PlayAnimation,
    // Windows SDK の PlaySound マクロとの衝突を避ける。列挙値の位置は変更しない。
    PlayAudio,
    SetBlackboard,
    RunScript,

    // ── Leaf: Condition ──
    HasTarget,
    IsTargetInRange,
    IsHealthBelow,
    BlackboardCompare,
    HasLineOfSight,

    // ── Leaf: テスト / 下書き用 ──
    // WHY 実装に含めるか: 未実装の枝を「栓」で塞いだまま木を組めるようにする。
    //     テストでも評価回数のカウントに使える。
    AlwaysSucceed,
    AlwaysFail,
    AlwaysRunning,

    Count
};

enum class BlackboardType : std::uint8_t { Bool = 0, Int, Float, Vector3, Entity, String };

enum class BTCompareOp : std::uint8_t {
    Equal = 0, NotEqual, Less, LessEqual, Greater, GreaterEqual
};

enum class BTParallelPolicy : std::uint8_t { RequireOne = 0, RequireAll };

// Blackboard のキー。BehaviorTreeAsset::blackboard の配列添字そのもの。
//
// WHY 添字を ID にするか:
//   BT は毎 tick に数十回キーを引く。文字列ハッシュ比較がホットパスに乗ると
//   エージェント数に比例して無視できないコストになる。
//   コンパイル時に名前 → 添字へ解決しておけば、実行時は直接添字アクセスで済み、
//   ハッシュマップを 1 つも書かずに済む。
using BlackboardKey = std::uint16_t;

inline constexpr BlackboardKey kInvalidBlackboardKey = 0xFFFF;
inline constexpr std::uint16_t kInvalidNode          = 0xFFFF;

// ── ノード種別の述語 ────────────────────────────────────────────────────────

[[nodiscard]] bool BTNodeIsComposite(BTNodeType type) noexcept;
[[nodiscard]] bool BTNodeIsDecorator(BTNodeType type) noexcept;
[[nodiscard]] bool BTNodeIsLeaf(BTNodeType type) noexcept;

// 副作用を持たない条件ノードか。
//
// WHY 必要か: observerAborts は「Running 中に毎 tick 条件を再評価する」機構。
//     そこで副作用のあるノード (Blackboard を書く / アニメを再生する) を
//     評価すると、中断チェックのたびに世界が変わり木が非決定的になる。
//     ValidateBehaviorTreeAsset がこの述語で abortMode の付与先を制限する。
[[nodiscard]] bool BTNodeIsPureCondition(BTNodeType type) noexcept;

[[nodiscard]] const char* BTNodeTypeName(BTNodeType type) noexcept;

// 許容される子の数。0 = リーフ、1 = Decorator、-1 = 無制限 (Composite)。
[[nodiscard]] int BTNodeMaxChildren(BTNodeType type) noexcept;

[[nodiscard]] const char* BTStatusName(BTStatus status) noexcept;
[[nodiscard]] const char* BlackboardTypeName(BlackboardType type) noexcept;
[[nodiscard]] const char* BTCompareOpName(BTCompareOp op) noexcept;

} // namespace fbzz::ai
