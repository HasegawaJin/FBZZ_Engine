/// @file    BehaviorTreeAsset.hpp
/// @brief   .behaviortree のオーサリング表現 (編集用)。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note ランタイムは DFS pre-order のフラット配列が最速だが、エディタは安定 ID と自由な親子付け替えを要求するため、id/parentId/order で木を表現し `CompileBehaviorTree()` がロード時に一度だけフラット化する。
/// @note パラメータをバイト列プールへ詰めると TOML 化・Inspector 表示ができないため、全種別のパラメータを平置きで持つ。
#pragma once
#include <Engine/AI/BehaviorTreeTypes.hpp>
#include <Engine/AI/Blackboard.hpp>
#include <Math/Vector3.hpp>

#include <string>
#include <vector>

namespace fbzz::ai {

struct BTNodeDef {
    int id       = 0;   ///< 安定 ID。エディタの選択・Undo・親子解決の鍵。0 は無効値として使う。
    int parentId = 0;   ///< 0 = ルート (親を持たない)。
    int order    = 0;   ///< 同一親内の左→右順。これが優先度そのもの。

    BTNodeType  type = BTNodeType::Sequence;
    std::string name;   ///< 表示名 (空ならノード種別名を使う)

    float editorX = 0.0f;
    float editorY = 0.0f;

    /// @name Decorator 共通
    /// @{
    AbortMode abortMode = AbortMode::None;   ///< Validate が純粋条件でないノードへの付与を拒否する。
    /// @}

    /// @name Repeat
    /// @{
    int  repeatCount        = 0;   ///< 0 = 無限
    bool repeatUntilFailure = false;
    /// @}

    /// @name Cooldown / TimeLimit / Wait
    /// @{
    float duration = 1.0f;
    float durationRandom = 0.0f;   ///< ±この秒数をランダムに加える。同時スポーンした敵の待機が同期して見えるのを崩す。
    /// @}

    /// @name Parallel
    /// @{
    BTParallelPolicy successPolicy = BTParallelPolicy::RequireAll;
    /// @}

    std::vector<float> childWeights;   ///< RandomSelector: 子ごとの重み (order 順に対応)

    /// @name Blackboard 参照 (BlackboardCondition / BlackboardCompare / SetBlackboard)
    /// @{
    std::string    keyName;   ///< Compile 時に添字へ解決
    BTCompareOp    compareOp = BTCompareOp::Equal;
    BlackboardType valueType = BlackboardType::Bool;
    bool           valueBool    = false;
    int            valueInt     = 0;
    float          valueFloat   = 0.0f;
    math::Vector3  valueVector3 = math::Vector3::ZERO;
    std::string    valueString;
    float withinSeconds = 0.0f;   ///< 「N 秒以内に書かれた値か」も条件に加える。0 = 時間条件なし。
    /// @}

    /// @name MoveTo
    /// @{
    std::string moveTargetKey;   ///< Vector3 か Entity のキー。空なら valueVector3 を使う
    float       acceptanceRadius = 0.5f;
    bool        chaseEntity      = false;   ///< true なら SetTarget で追跡し続ける。false なら SetDestination で一度だけ向かう。
    float       repathInterval   = 0.4f;
    /// @}

    /// @name LookAt / IsTargetInRange
    /// @{
    float range        = 5.0f;
    float turnSpeedDeg = 360.0f;
    /// @}

    /// @name PlayAnimation
    /// @{
    std::string animatorTrigger;
    bool        waitForAnimation = false;
    /// @}

    /// @name PlaySound
    /// @{
    std::string soundPath;
    float       volume = 1.0f;
    /// @}

    /// @name RunScript
    /// @{
    std::string scriptMethod;
    /// @}

    /// @name IsHealthBelow
    /// @{
    float threshold01 = 0.3f;
    /// @}
};

struct BehaviorTreeAsset {
    int         version = 1;
    std::string name    = "Behavior Tree";
    std::string description;

    std::vector<BTNodeDef> nodes;   ///< 順不同。木構造は parentId / order が持つ。

    std::vector<BlackboardDef> blackboard;   ///< [0, bb::ReservedCount) は予約キー。EnsureReservedBlackboardKeys() が保証する。

    int nextNodeId = 1;   ///< エディタの ID 発番カウンタ。次に作るノードの id。

    [[nodiscard]] const BTNodeDef* FindNode(int id) const;
    [[nodiscard]] BTNodeDef*       FindNode(int id);
    /// @brief 親を持たないノード (parentId == 0) を全部返す。正常な木なら 1 個。
    [[nodiscard]] std::vector<int> FindRootIds() const;
};

/// @brief 予約キーが欠けていれば先頭へ差し込み、順序を bb:: の添字に合わせて正す。既存のユーザー定義キーは予約キーの後ろへ寄せられる。
/// @note 手書きの .behaviortree や古いアセットでも bb::Self 等の固定添字が必ず成立することを保証する。
void EnsureReservedBlackboardKeys(BehaviorTreeAsset& asset);

/// @name I/O (実装は段階 2)
/// @{

/// @brief 検証込みで読み込む。
/// @return Validate に落ちたら false。
[[nodiscard]] bool LoadBehaviorTreeAsset(const std::string& path, BehaviorTreeAsset& out,
                                         std::string* outError = nullptr);
/// @brief 検証せず構造だけ返す。壊れたアセットをエディタで開いて直すために使う。
[[nodiscard]] bool ParseBehaviorTreeAsset(const std::string& path, BehaviorTreeAsset& out,
                                          std::string* outError = nullptr);
[[nodiscard]] bool SaveBehaviorTreeAsset(const std::string& path, const BehaviorTreeAsset& asset,
                                         std::string* outError = nullptr);
/// @}

/// @brief 保存を拒否すべき致命的な不整合を検出する。
/// @note ルートが 0 個/2 個以上、親子関係の循環/存在しない親、BTNodeMaxChildren() 超過、純粋条件でないノードに abortMode != None のいずれか。
[[nodiscard]] bool ValidateBehaviorTreeAsset(const BehaviorTreeAsset& asset,
                                             std::string* outError = nullptr);

/// @brief 保存は通すが可視化したい問題。
struct BTWarning {
    int         nodeId = 0;
    std::string code;
    std::string message;
};
[[nodiscard]] std::vector<BTWarning> CollectBehaviorTreeWarnings(const BehaviorTreeAsset& asset);

} // namespace fbzz::ai
