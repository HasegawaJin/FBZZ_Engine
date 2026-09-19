/// @file    PlayerActionState.hpp
/// @brief   プレイヤーが «今なにをしているか» の一枚。ボスの AI が読んで手を選ぶ
/// @author  Hasegawa Jin
/// @date    2026-09-03
///
/// @note BossAiComponent から BladeComponent を直接読ませない。include すると
///       BossAi→Blade→BossRig→BossAi で環になるため (BossRig は SetCrippled 呼び出しで
///       BossAi を見る)、双方が葉のヘッダーだけを見る «置いていく» 形にする。書いた時刻を
///       一緒に持たせ古い申告は読む側が捨てることで、プレイヤー死亡・シーン遷移の瞬間に
///       «永遠に振り続けている» が残る壊れ方を避ける。
#pragma once

namespace sandbox::playeraction {

/// 剣が毎フレーム申告する一枚。
struct Snapshot {
    /// 発生か硬直の最中。
    bool  swinging    = false;
    /// 今振っているのが連撃の最終段か。硬直が一番長い ＝ 差し込みどころ。
    bool  finisher    = false;
    /// 振り抜いた後の硬直中。
    bool  recovering  = false;
    /// 溜めの進み [0,1]。0 なら溜めていない。
    float chargeRatio = 0.0f;
    /// 最後に申告した時刻 [秒]。負なら一度も申告されていない。
    float stamp       = -1.0f;
};

inline Snapshot& Mutable()
{
    static Snapshot state;
    return state;
}

/// 申告する (剣が呼ぶ)。
inline void Publish(bool swinging, bool finisher, bool recovering,
                    float chargeRatio, float now)
{
    Snapshot& state   = Mutable();
    state.swinging    = swinging;
    state.finisher    = finisher;
    state.recovering  = recovering;
    state.chargeRatio = chargeRatio;
    state.stamp       = now;
}

/// 読む (ボスが呼ぶ)。`maxAge` より古い申告は無かったことにする。
[[nodiscard]] inline Snapshot Read(float now, float maxAge = 0.25f)
{
    const Snapshot& state = Mutable();
    if (state.stamp < 0.0f || now - state.stamp > maxAge) return Snapshot{};
    return state;
}

} // namespace sandbox::playeraction

/// カメラ演出が «今、盤面を止めている» の一枚。BossCameraDirectorComponent が置き、
/// ボス AI とプレイヤーの入力が読む。
///
/// @note 置く側 (カメラ) と読む側 (ボス/プレイヤー) が互いを include せずに済む葉のヘッダーと
///       してここに置く。playeraction と同じ «時刻付きで置いていく» 形で、演出中にカメラが
///       消えても «永遠に止まったまま» を残さない。
namespace sandbox::cutscene {

struct Snapshot {
    /// ボスは手を出さない (登場など)。
    bool  holdBoss   = false;
    /// プレイヤーは入力を受けない。
    bool  holdPlayer = false;
    float stamp      = -1.0f;
};

inline Snapshot& Mutable()
{
    static Snapshot state;
    return state;
}

/// 演出が毎フレーム申告する。演出が終わったら holdBoss / holdPlayer とも false で置く
/// (置かなくても maxAge で切れるが、1 フレームでも早く返した方が操作は軽い)。
inline void Publish(bool holdBoss, bool holdPlayer, float now)
{
    Snapshot& state  = Mutable();
    state.holdBoss   = holdBoss;
    state.holdPlayer = holdPlayer;
    state.stamp      = now;
}

[[nodiscard]] inline Snapshot Read(float now, float maxAge = 0.25f)
{
    const Snapshot& state = Mutable();
    if (state.stamp < 0.0f || now - state.stamp > maxAge) return Snapshot{};
    return state;
}

[[nodiscard]] inline bool HoldsBoss(float now)   { return Read(now).holdBoss; }
[[nodiscard]] inline bool HoldsPlayer(float now) { return Read(now).holdPlayer; }

} // namespace sandbox::cutscene
