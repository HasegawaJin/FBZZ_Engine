// ParticleGame
// ParticleCombatShared.hpp | particlegame
// Particle弾とゲーム進行スクリプト間の一方向イベント受け渡し
#pragma once

#include <algorithm>

namespace particlegame {

// ParticleCombatEvents — 弾側からPlayer本体へ直接依存せず、被弾と加点を次フレームへ渡す。
// WHY: Script DLL内の循環includeを避け、Projectileを他のゲームルールでも再利用可能にする。
struct ParticleCombatEvents {
    static void AddPlayerDamage(int amount)
    {
        s_pendingPlayerDamage += std::max(0, amount);
    }

    static void AddScore(int amount)
    {
        s_pendingScore += std::max(0, amount);
    }

    // 敵への命中と撃破を分けて通知し、ゲーム進行側でSE・コンボ・撃破数を一元管理する。
    static void AddEnemyHit(bool killed)
    {
        ++s_pendingEnemyHits;
        if (killed) ++s_pendingEnemyKills;
    }

    static int ConsumePlayerDamage()
    {
        const int value = s_pendingPlayerDamage;
        s_pendingPlayerDamage = 0;
        return value;
    }

    static int ConsumeScore()
    {
        const int value = s_pendingScore;
        s_pendingScore = 0;
        return value;
    }

    static int ConsumeEnemyHits()
    {
        const int value = s_pendingEnemyHits;
        s_pendingEnemyHits = 0;
        return value;
    }

    static int ConsumeEnemyKills()
    {
        const int value = s_pendingEnemyKills;
        s_pendingEnemyKills = 0;
        return value;
    }

    static void Reset()
    {
        s_pendingPlayerDamage = 0;
        s_pendingScore = 0;
        s_pendingEnemyHits = 0;
        s_pendingEnemyKills = 0;
    }

private:
    static inline int s_pendingPlayerDamage = 0;
    static inline int s_pendingScore = 0;
    static inline int s_pendingEnemyHits = 0;
    static inline int s_pendingEnemyKills = 0;
};

} // namespace particlegame
