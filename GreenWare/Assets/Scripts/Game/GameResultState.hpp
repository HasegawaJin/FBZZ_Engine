/// @file    GameResultState.hpp
/// @brief   Main.scene から Result.scene へ受け渡す最小限の戦績。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// WHY ランクの «判定» までここに置くか (Result 側ではなく):
///   評価軸 (Docs/game-flow.md「評価とランク」) はゲームデザインの決定で、表示は
///   その結果を出すだけの仕事。判定を表示側に置くと、リザルト画面を作り直すたびに
///   評価規則を運ぶことになり、規則そのものが画面の都合で変わる。
#pragma once

#include <algorithm>

namespace sandbox {

enum class GameRank : int { C = 0, B, A, S };

struct GameResultState {
    static inline bool victory = false;
    static inline float clearSeconds = 0.0f;
    static inline int defeatedEnemies = 0;
    static inline int enemyImpacts = 0;
    static inline int anchorImpacts = 0;
    /// 1 回の集束で巻き込んだ最大数。«まとめて倒す» の評価軸。
    static inline int bestChain = 0;
    /// 反発でハザード・壁へ押し込んで落とした数。«押し» の評価軸。
    static inline int pushKills = 0;
    /// ノーリトライで通したか。リトライしたプレイにはランクを出さない。
    static inline bool noRetry = true;

    /// 自己ベスト。同じセッションのあいだだけ持つ (保存は GameSettings の担当)。
    static inline float bestSeconds = 0.0f;
    static inline int   bestScore   = 0;

    /// 3 項目を各 0〜3 点で採点した合計 (0〜9)。
    ///
    /// WHY 引きと押しの両方を軸に置くか: 集束だけを評価すると «溜めて撃つ» だけが
    ///     正解になり、旧設計と同じ形へ戻る。押し込みを別項目にすることで、
    ///     近距離の即応と遠距離の組み立ての両方に点が付く。
    [[nodiscard]] static int Score()
    {
        int score = 0;

        if (clearSeconds <= 390.0f)      score += 3;   // 6 分 30 秒
        else if (clearSeconds <= 480.0f) score += 2;   // 8 分
        else if (clearSeconds <= 600.0f) score += 1;   // 10 分

        if (bestChain >= 5)      score += 3;
        else if (bestChain >= 4) score += 2;
        else if (bestChain >= 3) score += 1;

        if (pushKills >= 8)      score += 3;
        else if (pushKills >= 5) score += 2;
        else if (pushKills >= 3) score += 1;

        return score;
    }

    [[nodiscard]] static GameRank Rank()
    {
        const int score = Score();
        if (score >= 8) return GameRank::S;
        if (score >= 6) return GameRank::A;
        if (score >= 4) return GameRank::B;
        return GameRank::C;
    }

    [[nodiscard]] static const char* RankLabel()
    {
        switch (Rank()) {
        case GameRank::S: return "S";
        case GameRank::A: return "A";
        case GameRank::B: return "B";
        case GameRank::C: break;
        }
        return "C";
    }

    /// ランクを表示してよいか。ノーリトライで «勝った» ときだけ。
    [[nodiscard]] static bool RankAvailable() { return victory && noRetry; }

    /// リザルトを表示した側が呼ぶ。自己ベストを更新する。
    static void CommitBest()
    {
        if (!victory) return;
        if (bestSeconds <= 0.0f || clearSeconds < bestSeconds) bestSeconds = clearSeconds;
        bestScore = std::max(bestScore, Score());
    }
};

} // namespace sandbox
