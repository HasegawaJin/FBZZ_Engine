/// @file    BehaviorTreeRuntime.hpp
/// @brief   コンパイル済みの木 (共有・不変)。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// WHY 2 本の並列配列に分けるか:
/// 走査中に触るのは type / 子の範囲だけで、パラメータは
/// 「そのノードが実際に実行されるとき」しか要らない。
/// 8 バイトの BTNode だけを DFS pre-order で並べておけば、
/// 走査順とメモリ順が一致してプリフェッチが効く。
/// パラメータ (文字列や vector を含む重い構造体) を同じ配列に混ぜると、
/// 1 ノードが数百バイトになりキャッシュラインを食い潰す。
#pragma once
#include <Engine/AI/BehaviorTreeAsset.hpp>
#include <Engine/AI/BehaviorTreeTypes.hpp>
#include <Engine/AI/Blackboard.hpp>
#include <Math/Vector3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::ai {

// 走査時に触る最小限。8 バイト。
struct BTNode {
    BTNodeType     type       = BTNodeType::Sequence;  // uint16
    std::uint16_t  firstChild = 0;   // 子は [firstChild, firstChild + childCount)
    std::uint16_t  childCount = 0;
    std::uint16_t  paramIndex = 0;   // params[] への添字
};

// ノードが実行されるときだけ引くパラメータ。BTNodeDef からキー解決済み。
struct BTNodeParams {
    AbortMode      abortMode = AbortMode::None;
    BlackboardKey  key       = kInvalidBlackboardKey;   // keyName の解決結果
    BlackboardKey  moveKey   = kInvalidBlackboardKey;   // moveTargetKey の解決結果
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
    std::string   valueString;   // SetBlackboard(String) の値

    // 種別ごとに 1 つしか使わない文字列をまとめる。
    // WHY 個別に持たないか: 1 ノードが持つ文字列は多くて 1 本なので、
    //     animatorTrigger / soundPath / scriptMethod を別フィールドにすると
    //     使わない std::string が 2 本ぶんメモリに残る。
    std::string   text;

    std::vector<float> childWeights;

    // エディタの実行中ハイライトを逆引きするための元 ID。
    int authoringId = 0;
};

// observerAborts の中断候補。コンパイル時に事前計算する。
//
// WHY 区間で表せるか:
//   木を DFS pre-order で並べると「index が小さいほど左 = 高優先度」が成立し、
//   任意の部分木が [i, subtreeEnd[i]) という**連続区間**になる。
//   これにより「Running 中のリーフが自分の配下か / 自分より右か」の判定が
//   2 回の整数比較で済む。
struct BTAbortCandidate {
    std::uint16_t node = 0;
    AbortMode     mode = AbortMode::None;

    // 自分のサブツリー [selfLo, selfHi)
    std::uint16_t selfLo = 0;
    std::uint16_t selfHi = 0;

    // 自分より低優先度の領域 [lowLo, lowHi)
    std::uint16_t lowLo = 0;
    std::uint16_t lowHi = 0;
};

struct BehaviorTreeRuntime {
    std::string sourcePath;

    std::vector<BTNode>       nodes;   // DFS pre-order。nodes[0] がルート
    std::vector<BTNodeParams> params;

    std::vector<std::uint16_t> parent;      // parent[0] == kInvalidNode
    std::vector<std::uint16_t> subtreeEnd;  // 部分木は [i, subtreeEnd[i])

    std::vector<BlackboardDef> blackboard;
    // ScriptAIProxy が文字列でキーを引くためだけの表。tick ループには乗らない。
    std::vector<std::string> keyNames;

    // node index 昇順 = 優先度の高い順。
    std::vector<BTAbortCandidate> abortCandidates;

    // nodeIndex -> BTNodeDef::id
    std::vector<int> authoringIdOf;

    // コンパイル時に解決できなかった keyName など。保存は通っているが実行時に効かない。
    std::vector<std::string> compileWarnings;

    [[nodiscard]] bool Empty() const { return nodes.empty(); }
    [[nodiscard]] std::size_t NodeCount() const { return nodes.size(); }

    // 名前から Blackboard キーを引く (線形検索)。見つからなければ kInvalidBlackboardKey。
    [[nodiscard]] BlackboardKey FindKey(std::string_view name) const;
};

// アセットをランタイム表現へ変換する。
//
// 失敗するのは構造が壊れている場合のみ (ルートが無い / 循環)。
// keyName の解決失敗は失敗にせず、compileWarnings に積んで
// そのノードの key を kInvalidBlackboardKey にする。
// WHY: キー名の打ち間違い 1 つで木全体が動かなくなるより、
//      該当ノードだけが Failure を返し続ける方が原因を追いやすい。
[[nodiscard]] bool CompileBehaviorTree(const BehaviorTreeAsset& asset,
                                       BehaviorTreeRuntime& out,
                                       std::string* outError = nullptr);

} // namespace fbzz::ai
