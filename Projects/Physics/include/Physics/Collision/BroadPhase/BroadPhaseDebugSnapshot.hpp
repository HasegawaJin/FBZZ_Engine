/// @file    BroadPhaseDebugSnapshot.hpp
/// @brief   Bounded value diagnostics of the actual Physics broadphase.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once
#include <Physics/World/BodyHandle.hpp>
#include <Physics/Collision/Shapes/Collider.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::physics
{
    inline constexpr std::size_t BROAD_PHASE_DEBUG_MAX_ITEMS = 4096;
    inline constexpr std::size_t BROAD_PHASE_DEBUG_MAX_WORK = 65536;
    inline constexpr uint32_t BROAD_PHASE_DEBUG_MAX_DEPTH = 64;

    enum class BroadPhaseDebugStructure { UNAVAILABLE, BVH, BRUTE_FORCE, EMPTY };

    struct BroadPhaseDebugRequest
    {
        bool enabled = false; ///< この要求で診断用スナップショットを採取するかどうか。
        bool hierarchy = true; ///< BVH の階層ノードを採取するかどうか。
        bool bounds = false; ///< 候補のコライダーごとの境界を採取するかどうか。
        bool candidatePairs = false; ///< 詳細判定へ渡す候補ペアを採取するかどうか。
        bool selectedOnly = false; ///< 指定したコライダーに関わる項目だけを採取するかどうか。
        uint32_t minDepth = 0; ///< 採取する階層の最小深さ。ルートが 0。
        uint32_t maxDepth = 8; ///< 採取する階層の最大深さ。ルートが 0。
        std::size_t maxNodes = 512; ///< 返す BVH ノード数の上限。
        std::size_t maxProxies = 512; ///< 返すコライダー境界数の上限。
        std::size_t maxPairs = 256; ///< 返す接触候補ペア数の上限。
        ColliderHandle selected; ///< 採取対象として選択したコライダーのハンドル。
        uint64_t selectedWorldEpoch = 0; ///< 選択したコライダーの所属 World の識別値。
    };

    /// @note Node indices identify the source BVH within this snapshot only; filtered relatives may be absent.
    struct BroadPhaseDebugNode
    {
        AABB bounds; ///< 採取時点の BVH ノードのワールド境界 [m]。
        int sourceIndex = -1; ///< 採取元の BVH 内でのこのノードの添字。
        int parent = -1; ///< 採取元の BVH 内の親の添字。-1 は親なし。
        int left = -1; ///< 採取元の BVH 内の左の子の添字。-1 は子なし。
        int right = -1; ///< 採取元の BVH 内の右の子の添字。-1 は子なし。
        uint32_t depth = 0; ///< ルートを 0 とするノードの深さ。
        std::size_t proxyCount = 0; ///< このノードの下に含まれるコライダー境界の個数。
        bool leaf = false; ///< このノードがコライダーを格納する葉かどうか。
    };

    struct BroadPhaseDebugProxy
    {
        ColliderHandle handle; ///< この境界に対応する World 上のコライダー識別子。
        AABB bounds; ///< 採取時点のコライダーのワールド AABB [m]。
        std::size_t sourceIndex = 0; ///< 採取元のコライダー境界配列の添字。
        bool isStatic = true; ///< 対応するコライダーが静的として扱われるかどうか。
        bool isSleeping = false; ///< 対応する剛体が休止状態かどうか。
        bool isTrigger = false; ///< 当たり判定の通知のみを行うトリガーかどうか。
        int layer = 0; ///< 衝突フィルターで使うコライダーのレイヤー番号。
    };

    /// @note Accepted broadphase candidates before NarrowPhase; these are not confirmed contacts.
    struct BroadPhaseDebugPair
    {
        ColliderHandle handleA; ///< 詳細判定へ渡した候補ペアの 1 番目のコライダー識別子。
        ColliderHandle handleB; ///< 詳細判定へ渡した候補ペアの 2 番目のコライダー識別子。
        AABB boundsA; ///< 採取時点の 1 番目のコライダーのワールド AABB [m]。
        AABB boundsB; ///< 採取時点の 2 番目のコライダーのワールド AABB [m]。
        std::size_t sourceIndexA = 0; ///< 1 番目のコライダーの採取元配列での添字。
        std::size_t sourceIndexB = 0; ///< 2 番目のコライダーの採取元配列での添字。
    };

    /// @note Values describe the final substep before NarrowPhase/Resolve, in world space.
    /// @note No mutable tree, Collider, Body or scene pointer crosses this boundary.
    struct BroadPhaseDebugSnapshot
    {
        uint64_t worldEpoch = 0; ///< この採取結果を作成した World の識別値。
        uint64_t sequence = 0; ///< この World 内で採取の順序を表す連番。
        BroadPhaseDebugStructure structure = BroadPhaseDebugStructure::UNAVAILABLE; ///< 概略判定が採取時に使ったデータ構造の種類。
        BroadPhaseDebugRequest request; ///< この採取に使った要求と上限の値。
        std::vector<BroadPhaseDebugNode> nodes; ///< 要求条件に従って採取した BVH ノードの値。
        std::vector<BroadPhaseDebugProxy> proxies; ///< 要求条件に従って採取したコライダー境界の値。
        std::vector<BroadPhaseDebugPair> pairs; ///< 要求条件に従って採取した概略判定の候補ペア。
        std::size_t totalNodes = 0; ///< 採取対象の BVH に含まれる全ノード数。
        std::size_t totalProxies = 0; ///< 採取元が保持する全コライダー境界数。
        std::size_t totalPairs = 0; ///< 概略判定で生成した全候補ペア数。
        std::size_t examinedNodes = 0; ///< 採取中に検査したノード数。
        std::size_t examinedProxies = 0; ///< 採取中に検査したコライダー境界数。
        std::size_t examinedPairs = 0; ///< 採取中に検査した候補ペア数。
        bool broadPhaseRan = false; ///< この採取対象のサブステップで概略判定を実行したかどうか。
        bool candidatePairsAvailable = false; ///< この採取で概略判定の候補ペアを参照できたかどうか。
        bool traversalStatusAvailable = false; ///< この採取で走査状況を参照できたかどうか。
        bool selectionValid = false; ///< 要求された選択を採取時の World 上で解決できたかどうか。
        bool nodesTruncated = false; ///< 上限によって採取するノードを省いたかどうか。
        bool proxiesTruncated = false; ///< 上限によって採取する境界を省いたかどうか。
        bool pairsTruncated = false; ///< 上限によって採取するペアを省いたかどうか。
        bool workLimitReached = false; ///< 採取のために調べられる項目数の上限へ達したかどうか。
    };
} /// @note namespace fbzz::physics
