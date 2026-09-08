/// @file    GameResultState.hpp
/// @brief   Stage_NN から Result.scene へ受け渡す最小限の戦績と、その採点表。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// WHY ランクの «判定» までここに置くか (Result 側ではなく):
///   評価軸 (Docs/game-flow.md「評価とランク」) はゲームデザインの決定で、表示は
///   その結果を出すだけの仕事。判定を表示側に置くと、リザルト画面を作り直すたびに
///   評価規則を運ぶことになり、規則そのものが画面の都合で変わる。
///
/// WHY 見出しと達成ラインの文字までここが持つか:
///   同じ 3 軸をリザルトと選択画面の 2 箇所が描く。しきい値を変えたとき、
///   採点は変わったのに画面の «5 / 4 / 3 体» だけが古いまま残る ─ 実際に
///   雑魚を畳んだ後もリザルトは «押し込み撃破» を採点し続けていて、
///   どんな周でも 3 点満点しか取れなかった。数字と文字は 1 つの表から引く。
#pragma once

#include <algorithm>
#include <string>

namespace sandbox {

enum class GameRank : int { C = 0, B, A, S };

/// 評価軸 1 本ぶんの見出し。採点はこの下の関数が持つ。
struct ScoreAxis {
    const char* name;        ///< 行の見出し
    const char* thresholds;  ///< 3 点 / 2 点 / 1 点 の達成ライン (表示用)
};

struct GameResultState {
    static inline bool victory = false;
    static inline float clearSeconds = 0.0f;
    static inline int defeatedEnemies = 0;

    /// 途切れずに当て続けた斬撃の最長。«手を止めない» の評価軸。
    static inline int bestChain = 0;
    /// 受けたダメージの合計。«間合いを読む» の評価軸。
    static inline int damageTaken = 0;
    /// ジャスト回避の回数。採点には乗せないが、リザルトの伸びしろとして運ぶ。
    static inline int perfectDodges = 0;

    /// ノーリトライで通したか。リトライしたプレイにはランクを出さない。
    static inline bool noRetry = true;

    /// ボスの残り HP (0〜1)。負けたときに «あと何割だったか» を出すためだけに持つ。
    static inline float bossHpRemain01 = 1.0f;
    /// 倒れたときのボスのフェーズ (1 or 2)。HP 50% で変わる (Docs/game-flow.md)。
    static inline int bossPhase = 1;

    /// 自己ベスト。同じセッションのあいだだけ持つ (保存は GameSettings の担当)。
    static inline float bestSeconds = 0.0f;
    static inline int   bestScore   = 0;

    static constexpr int kAxisCount = 3;
    static constexpr ScoreAxis kAxes[kAxisCount] = {
        { "クリアタイム", "3:00 / 4:00 / 5:00" },
        { "最大連撃",     "12 / 8 / 5 回" },
        { "被ダメージ",   "0 / 1 / 3 まで" },
    };

    [[nodiscard]] static int TimePoints(float seconds)
    {
        if (seconds <= 180.0f) return 3;
        if (seconds <= 240.0f) return 2;
        if (seconds <= 300.0f) return 1;
        return 0;
    }
    [[nodiscard]] static int ChainPoints(int chain)
    {
        if (chain >= 12) return 3;
        if (chain >= 8)  return 2;
        if (chain >= 5)  return 1;
        return 0;
    }
    // WHY 0 で満点にするか: HP は 5 しかなく、1 発が全体の 2 割。«一度も食らわない»
    //     が S の条件として読めることが、近接で間合いへ入り続ける理由になる。
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
    /// WHY 時間と連撃と被弾の 3 軸か: 速さだけだと «削れる隙に叩き込む» 以外の
    ///     手が点にならない。連撃は手を止めないこと、被弾は間合いを読むことに
    ///     それぞれ点を付け、攻めと守りの両方が上手さとして残るようにする。
    [[nodiscard]] static int Score()
    {
        return TimePoints(clearSeconds) + ChainPoints(bestChain) + DamagePoints(damageTaken);
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
        bestScore = (std::max)(bestScore, Score());
    }
};

} // namespace sandbox
