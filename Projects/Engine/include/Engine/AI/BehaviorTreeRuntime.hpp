/// @file    BehaviorTreeRuntime.hpp
/// @brief   コンパイル済みの木 (共有・不変)。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note 走査中に触るのは type/子の範囲だけで、パラメータは実行時にしか要らないため2本の並列配列に分ける。8 バイトの BTNode だけを DFS pre-order で並べると走査順とメモリ順が一致しプリフェッチが効く。重いパラメータ構造体を混ぜると1ノードが数百バイトになりキャッシュラインを食い潰す。
#pragma once
#include <Engine/AI/BehaviorTreeAsset.hpp>
#include <Engine/AI/BehaviorTreeTypes.hpp>
#include <Engine/AI/Blackboard.hpp>
#include <Math/Vector3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::ai {

/// @brief 走査時に触る最小限。8 バイト。
struct BTNode {
    BTNodeType     type       = BTNodeType::Sequence;  ///< uint16
    std::uint16_t  firstChild = 0;   ///< 子は [firstChild, firstChild + childCount)
    std::uint16_t  childCount = 0;
    std::uint16_t  paramIndex = 0;   ///< params[] への添字
};

/// @brief ノードが実行されるときだけ引くパラメータ。BTNodeDef からキー解決済み。
struct BTNodeParams {
    AbortMode      abortMode = AbortMode::None;
    BlackboardKey  key       = kInvalidBlackboardKey;   ///< keyName の解決結果
    BlackboardKey  moveKey   = kInvalidBlackboardKey;   ///< moveTargetKey の解決結果
    BTCompareOp    compareOp = BTCompareOp::Equal;
    BlackboardType valueType = BlackboardType::Bool;
    BTParallelPolicy successPolicy = BTParallelPolicy::RequireAll;

    int   repeatCount        = 0;
    bool  repeatUntilFailure = false;
    float duration           = 1.0f;
    float durationRandom     = 0.0f;
    float acceptanceRadius   = 0.5f;
    bool  chaseEntity        = false;
    float repathInterval     = 0.4f;
    float range              = 5.0f;
    float turnSpeedDeg       = 360.0f;
    float threshold01        = 0.3f;
    float withinSeconds      = 0.0f;
    float volume             = 1.0f;
    bool  waitForAnimation   = false;

    bool          valueBool    = false;
    int           valueInt     = 0;
    float         valueFloat   = 0.0f;
    math::Vector3 valueVector3 = math::Vector3::ZERO;
    std::string   valueString;   ///< SetBlackboard(String) の値

    /// @note animatorTrigger/soundPath/scriptMethod を別フィールドにすると未使用の std::string が残るため、種別ごとに1つしか使わない文字列をここへまとめる。
    std::string   text;

    std::vector<float> childWeights;

    int authoringId = 0;   ///< エディタの実行中ハイライトを逆引きするための元 ID。
};

/// @brief observerAborts の中断候補。コンパイル時に事前計算する。
/// @note 木を DFS pre-order で並べると「index が小さいほど左=高優先度」が成立し、任意の部分木が [i, subtreeEnd[i]) という連続区間になる。これにより「Running 中のリーフが自分の配下か/自分より右か」の判定が2回の整数比較で済む。
struct BTAbortCandidate {
    std::uint16_t node = 0;
    AbortMode     mode = AbortMode::None;

    std::uint16_t selfLo = 0;   ///< 自分のサブツリー [selfLo, selfHi)
    std::uint16_t selfHi = 0;

    std::uint16_t lowLo = 0;   ///< 自分より低優先度の領域 [lowLo, lowHi)
    std::uint16_t lowHi = 0;
};

struct BehaviorTreeRuntime {
    std::string sourcePath;

    std::vector<BTNode>       nodes;   ///< DFS pre-order。nodes[0] がルート
    std::vector<BTNodeParams> params;

    std::vector<std::uint16_t> parent;      ///< parent[0] == kInvalidNode
    std::vector<std::uint16_t> subtreeEnd;  ///< 部分木は [i, subtreeEnd[i])

    std::vector<BlackboardDef> blackboard;
    std::vector<std::string> keyNames;   ///< ScriptAIProxy が文字列でキーを引くためだけの表。tick ループには乗らない。

    std::vector<BTAbortCandidate> abortCandidates;   ///< node index 昇順 = 優先度の高い順。

    std::vector<int> authoringIdOf;   ///< nodeIndex -> BTNodeDef::id

    std::vector<std::string> compileWarnings;   ///< コンパイル時に解決できなかった keyName など。保存は通っているが実行時に効かない。

    [[nodiscard]] bool Empty() const { return nodes.empty(); }
    [[nodiscard]] std::size_t NodeCount() const { return nodes.size(); }

    /// @brief 名前から Blackboard キーを引く (線形検索)。
    /// @return 見つからなければ kInvalidBlackboardKey。
    [[nodiscard]] BlackboardKey FindKey(std::string_view name) const;
};

/// @brief アセットをランタイム表現へ変換する。
/// @return 失敗するのは構造が壊れている場合のみ (ルートが無い / 循環)。
/// @note keyName の解決失敗は失敗にせず compileWarnings に積みノードの key を kInvalidBlackboardKey にする。打ち間違い1つで木全体が止まるより該当ノードだけ Failure を返し続ける方が原因を追いやすい。
[[nodiscard]] bool CompileBehaviorTree(const BehaviorTreeAsset& asset,
                                       BehaviorTreeRuntime& out,
                                       std::string* outError = nullptr);

} // namespace fbzz::ai
