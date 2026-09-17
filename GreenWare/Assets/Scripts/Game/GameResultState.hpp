/// @file    GameResultState.hpp
/// @brief   Stage_NN から Result.scene へ受け渡す最小限の戦績と、その採点表。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// @note 評価軸・しきい値・見出し文字は全部ここが持つ。判定はゲームデザインの決定であり、
///       表示側 (Result / 選択画面) は結果を描くだけにする。両画面が同じ 3 軸を描くため、
///       しきい値を変えても見出し数字が片方だけ古いまま残らないよう 1 つの表から引く。
#pragma once

#include <algorithm>
#include <string>

namespace sandbox {

enum class GameRank : int { C = 0, B, A, S };

/// 評価軸 1 本ぶんの見出し。採点はこの下の関数が持つ。
struct ScoreAxis {
    const char* name;        ///< 行の見出し
    std::string thresholds;  ///< 得点の達成条件 (表示用)
};

struct GameResultState {
    static inline bool victory = false;
    static inline float clearSeconds = 0.0f;
    static inline int defeatedEnemies = 0;

    /// 途切れずに当て続けた斬撃の最長。«手を止めない» の評価軸。
    static inline int bestChain = 0;
    static inline int parries = 0;
    static inline int perfectCadences = 0;
    static inline int stageIndex = 0;
    static constexpr int kScoreVersion = 2;
    /// 受けたダメージの合計。«間合いを読む» の評価軸。
    static inline int damageTaken = 0;
    /// ジャスト回避の回数。採点には乗せないが、リザルトの伸びしろとして運ぶ。
    static inline int perfectDodges = 0;

    /// リトライ経由かの記録。採点は今回の挑戦だけで決める。
    static inline bool noRetry = true;

    /// ボスの残り HP (0〜1)。負けたときに «あと何割だったか» を出すためだけに持つ。
    static inline float bossHpRemain01 = 1.0f;
    static inline int bossHealthRemaining = 0;
    static inline int bossHealthTotal = 0;
    /// 倒れたときのボスのフェーズ (1 or 2)。HP 50% で変わる (Docs/game-flow.md)。
    static inline int bossPhase = 1;

    /// 自己ベスト。同じセッションのあいだだけ持つ (保存は GameSettings の担当)。
    static inline float bestSeconds = 0.0f;
    static inline int   bestScore   = 0;

    static constexpr int kAxisCount = 3;
    struct ScoreProfile {
        float seconds[3];
        int chain;
        int parry;
        int cadence;
    };
    static constexpr ScoreProfile kProfiles[3] = {
        {{240.0f, 330.0f, 420.0f}, 6, 2, 1},
        {{360.0f, 480.0f, 600.0f}, 8, 6, 2},
        {{240.0f, 330.0f, 420.0f}, 10, 4, 2},
    };

    [[nodiscard]] static const ScoreProfile& Profile(int stage)
    { return kProfiles[std::clamp(stage, 0, 2)]; }

    [[nodiscard]] static ScoreAxis Axis(int row)
    { return Axis(stageIndex, row); }

    [[nodiscard]] static ScoreAxis Axis(int stage, int row)
    {
        const auto& p = Profile(stage);
        if (row == 0) {
            const auto clock = [](float seconds) {
                const int total = static_cast<int>(seconds);
                return std::to_string(total / 60) + ":" + (total % 60 < 10 ? "0" : "")
                     + std::to_string(total % 60);
            };
            return {"クリアタイム", clock(p.seconds[0]) + " / " + clock(p.seconds[1])
                                   + " / " + clock(p.seconds[2])};
        }
        if (row == 1)
            return {"攻防の達成", std::to_string(p.chain) + "連撃・" + std::to_string(p.parry)
                                 + "弾き・" + std::to_string(p.cadence) + "拍締め"};
        return {"被ダメージ", "0 / 1 / 3 まで"};
    }

    [[nodiscard]] static int TimePoints(float seconds, int stage)
    {
        if (!(seconds >= 0.0f)) return 0;
        const auto& profile = Profile(stage);
        for (int i = 0; i < 3; ++i)
            if (seconds <= profile.seconds[i]) return 3 - i;
        return 0;
    }
    [[nodiscard]] static int TimePoints(float seconds)
    { return TimePoints(seconds, stageIndex); }

    [[nodiscard]] static int TechniquePoints(int chain, int parry, int cadence, int stage)
    {
        const auto& profile = Profile(stage);
        return (chain >= profile.chain ? 1 : 0)
             + (parry >= profile.parry ? 1 : 0)
             + (cadence >= profile.cadence ? 1 : 0);
    }
    [[nodiscard]] static int TechniquePoints()
    { return TechniquePoints(bestChain, parries, perfectCadences, stageIndex); }

    [[nodiscard]] static std::string TechniqueText(int points)
    { return std::to_string(points) + " / 3"; }
    /// @note HP は 5 しかなく 1 発で全体の 2 割減るため、0 を満点にすると «一度も食らわない»
    ///       が S の条件として読め、近接で間合いへ入り続ける動機になる。
    [[nodiscard]] static int DamagePoints(int damage)
    {
        if (damage <= 0) return 3;
        if (damage <= 1) return 2;
        if (damage <= 3) return 1;
        return 0;
    }

    [[nodiscard]] static std::string ChainText(int chain)
    { return std::to_string(chain) + " 回"; }
    [[nodiscard]] static std::string DamageText(int damage)
    { return std::to_string(damage); }

    /// 3 項目を各 0〜3 点で採点した合計 (0〜9)。
    ///
    /// @note 速さだけでは «削れる隙に叩き込む» 以外の手が点にならない。連撃は手を止めない
    ///       こと、被弾は間合いを読むことに点を付け、攻めと守りの両方を上手さとして残す。
    [[nodiscard]] static int Score()
    {
        return TimePoints(clearSeconds) + TechniquePoints() + DamagePoints(damageTaken);
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

    /// クリアした挑戦にランクを表示する。
    [[nodiscard]] static bool RankAvailable() { return victory; }

    /// リザルトを表示した側が呼ぶ。自己ベストを更新する。
    static void CommitBest()
    {
        if (!victory) return;
        if (bestSeconds <= 0.0f || clearSeconds < bestSeconds) bestSeconds = clearSeconds;
        bestScore = (std::max)(bestScore, Score());
    }
};

} // namespace sandbox
