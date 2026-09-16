/// @file    BehaviorTreeAsset.hpp
/// @brief   .behaviortree のオーサリング表現 (編集用)。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// WHY オーサリング表現とランタイム表現を分けるか:
/// ランタイムは「DFS pre-order のフラット配列」が最速だが、エディタは
/// 安定した ID と自由な親子付け替えを要求する。フラット配列を直接編集させると
/// ノードを 1 つ動かすたびに全ての firstChild を振り直すことになり、Undo とも相性が悪い。
/// ここでは id / parentId / order で木を表現し、CompileBehaviorTree() が
/// ロード時に一度だけフラット化する。
///
/// WHY 型消去しないか:
/// 設計当初は「パラメータをバイト列プールに詰める」案だったが、それだと
/// TOML へ書けず (base64 にすると人間が読めず diff も取れない)、Inspector も出せず、
/// フィールドを 1 つ増やした瞬間に既存アセットが全滅する。
/// 全種別のパラメータを平置きで持つ。
#pragma once
#include <Engine/AI/BehaviorTreeTypes.hpp>
#include <Engine/AI/Blackboard.hpp>
#include <Math/Vector3.hpp>

#include <string>
#include <vector>

namespace fbzz::ai {

struct BTNodeDef {
    // 安定 ID。エディタの選択・Undo・親子解決の鍵。0 は無効値として使う。
    int id       = 0;
    // 0 = ルート (親を持たない)。
    int parentId = 0;
    // 同一親内の左→右順。**これが優先度そのもの**。
    int order    = 0;

    BTNodeType  type = BTNodeType::Sequence;
    std::string name;   // 表示名 (空ならノード種別名を使う)

    float editorX = 0.0f;
    float editorY = 0.0f;

    // ── Decorator 共通 ──
    // Validate が「純粋条件でないノードに None 以外」を拒否する。
    AbortMode abortMode = AbortMode::None;

    // ── Repeat ──
    int  repeatCount        = 0;      // 0 = 無限
    bool repeatUntilFailure = false;

    // ── Cooldown / TimeLimit / Wait ──
    float duration = 1.0f;
    // ±この秒数をランダムに加える。
    // WHY: 同時にスポーンした敵の待機が完全に同期すると、群れが機械的に見える。
    float durationRandom = 0.0f;

    // ── Parallel ──
    BTParallelPolicy successPolicy = BTParallelPolicy::RequireAll;

    // ── RandomSelector: 子ごとの重み (order 順に対応) ──
    std::vector<float> childWeights;

    // ── Blackboard 参照 (BlackboardCondition / BlackboardCompare / SetBlackboard) ──
    std::string    keyName;                              // Compile 時に添字へ解決
    BTCompareOp    compareOp = BTCompareOp::Equal;
    BlackboardType valueType = BlackboardType::Bool;
    bool           valueBool    = false;
    int            valueInt     = 0;
    float          valueFloat   = 0.0f;
    math::Vector3  valueVector3 = math::Vector3::ZERO;
    std::string    valueString;
    // 「N 秒以内に書かれた値か」も条件に加える。0 = 時間条件なし。
    float withinSeconds = 0.0f;

    // ── MoveTo ──
    std::string moveTargetKey;          // Vector3 か Entity のキー。空なら valueVector3 を使う
    float       acceptanceRadius = 0.5f;
    // true なら SetTarget で追跡し続ける。false なら SetDestination で一度だけ向かう。
    bool        chaseEntity      = false;
    float       repathInterval   = 0.4f;

    // ── LookAt / IsTargetInRange ──
    float range        = 5.0f;
    float turnSpeedDeg = 360.0f;

    // ── PlayAnimation ──
    std::string animatorTrigger;
    bool        waitForAnimation = false;

    // ── PlaySound ──
    std::string soundPath;
    float       volume = 1.0f;

    // ── RunScript ──
    std::string scriptMethod;

    // ── IsHealthBelow ──
    float threshold01 = 0.3f;
};

struct BehaviorTreeAsset {
    int         version = 1;
    std::string name    = "Behavior Tree";
    std::string description;

    // 順不同。木構造は parentId / order が持つ。
    std::vector<BTNodeDef> nodes;

    // [0, bb::ReservedCount) は予約キー。EnsureReservedBlackboardKeys() が保証する。
    std::vector<BlackboardDef> blackboard;

    // エディタの ID 発番カウンタ。次に作るノードの id。
    int nextNodeId = 1;

    [[nodiscard]] const BTNodeDef* FindNode(int id) const;
    [[nodiscard]] BTNodeDef*       FindNode(int id);
    // 親を持たないノード (parentId == 0) を全部返す。正常な木なら 1 個。
    [[nodiscard]] std::vector<int> FindRootIds() const;
};

// 予約キーが欠けていれば先頭へ差し込み、順序を bb:: の添字に合わせて正す。
// 既存のユーザー定義キーは予約キーの後ろへ寄せられる。
//
// WHY 必要か: 手書きの .behaviortree や古いアセットでも bb::Self 等の
//     固定添字が必ず成立することを保証する。
void EnsureReservedBlackboardKeys(BehaviorTreeAsset& asset);

// ── I/O (実装は段階 2) ──────────────────────────────────────────────────────

// 検証込みで読み込む。Validate に落ちたら false。
[[nodiscard]] bool LoadBehaviorTreeAsset(const std::string& path, BehaviorTreeAsset& out,
                                         std::string* outError = nullptr);
// 検証せず構造だけ返す。壊れたアセットをエディタで開いて直すために使う。
[[nodiscard]] bool ParseBehaviorTreeAsset(const std::string& path, BehaviorTreeAsset& out,
                                          std::string* outError = nullptr);
[[nodiscard]] bool SaveBehaviorTreeAsset(const std::string& path, const BehaviorTreeAsset& asset,
                                         std::string* outError = nullptr);

// 保存を拒否すべき致命的な不整合を検出する。
//   - ルートが 0 個 / 2 個以上
//   - 親子関係の循環 / 存在しない親
//   - BTNodeMaxChildren() 超過 (リーフに子、Decorator に子 2 個)
//   - 純粋条件でないノードに abortMode != None
[[nodiscard]] bool ValidateBehaviorTreeAsset(const BehaviorTreeAsset& asset,
                                             std::string* outError = nullptr);

// 保存は通すが可視化したい問題。
struct BTWarning {
    int         nodeId = 0;
    std::string code;
    std::string message;
};
[[nodiscard]] std::vector<BTWarning> CollectBehaviorTreeWarnings(const BehaviorTreeAsset& asset);

} // namespace fbzz::ai
