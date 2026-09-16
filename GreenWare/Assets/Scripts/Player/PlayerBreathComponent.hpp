/// @file    PlayerBreathComponent.hpp
/// @brief   プレイヤーの息 (スタミナ)。守りと回避だけを削り、攻めで戻る。
/// @author  Hasegawa Jin
/// @date    2026-09-14
///
/// WHY 数字を全部ここへ寄せるか:
///   消費も回復も PlayerTuning が正本で、読むのはこのファイルだけにしてある。
///   呼ぶ側 (回避・弾き・斬撃) は SpendDodge() / GainParry() のように «動詞» で
///   叩く ── 3 箇所が別々に tuning を辿ると、量を触るたびに «どこが払っているか»
///   を追い直すことになる。
///
/// WHY 息切れを «0 に触れた瞬間» ではなく状態で持つか:
///   0 で明けると、回復した 1 滴でまた転がって即座に尽きる往復になり、切れている
///   こと自体が画面にも手にも出ない。Exhaust Recover まで戻るあいだ手を封じると、
///   «攻めるしかない時間» として読める長さになる。
#pragma once
#include <Scripts/Game/TimeManagerComponent.hpp>

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Data/PlayerTuning.hpp>
#include <algorithm>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class PlayerBreathComponent : public Script {
    FBZZ_SCRIPT(PlayerBreathComponent)

public:
    // PlayerComponent が必須 PlayerTuning を注入する。量をこの Script に複製しない。
    fbzz::Asset<PlayerTuning> tuning{};

    FBZZ_GROUP("息")
    FBZZ_FIELD_READ_ONLY(float, debugBreath, 0.0f, "息")
    FBZZ_FIELD_READ_ONLY(bool, debugExhausted, false, "息切れ")

    // WHY Max() にしないか: math::Max を同じクラス内から呼べなくなる (名前が隠れる)。
    [[nodiscard]] float MaxBreath() const { return tuning ? std::max(tuning->breathMax, 0.0f) : 0.0f; }
    [[nodiscard]] float Current() const { return m_breath; }
    /// HUD のバー用。1 = 満タン / 0 = 空。
    [[nodiscard]] float Normalized() const;
    /// 息が尽きて手が封じられている状態。
    [[nodiscard]] bool IsExhausted() const { return m_exhausted; }
    /// 息の要る手 (回避・構え) を今出せるか。
    [[nodiscard]] bool CanAct() const { return !m_exhausted && m_breath > 0.0f; }
    /// 直近 seconds 秒のあいだに «息が無くて出せなかった» があったか。HUD が読む。
    [[nodiscard]] bool DeniedWithin(float seconds) const;

    /// 出せたら払って true。払える限り出せる (残量が足りなくても 0 で止まる) ので、
    /// «最後の 1 滴で転がる» は成立する。出せなかったときは Deny() を通る。
    bool SpendDodge()
    {
        const float before = m_breath;
        const bool spent = Spend(tuning ? tuning->breathDodgeCost : 0.0f);
        m_dodgeSpent += before - m_breath;
        return spent;
    }
    [[nodiscard]] float DodgeSpent() const { return m_dodgeSpent; }
    [[nodiscard]] float GuardSpent() const { return m_guardSpent; }
    bool SpendParryWhiff() { return Spend(tuning ? tuning->breathParryWhiffCost : 0.0f); }
    /// 構えを保っている間の消費。**尽きた瞬間のフレームだけ** true を返す。
    bool DrainGuard(float dt);

    void GainSlash() { Gain(tuning ? tuning->breathSlashGain : 0.0f); }
    void GainParry(bool just)
    { Gain(tuning ? (just ? tuning->breathJustParryGain : tuning->breathParryGain) : 0.0f); }
    /// 満タンまで戻す。ジャスト回避の見返り (報酬は 1 つの瞬間へ集める)。
    void Refill();
    /// 息が無くて手が出せなかったことを記録する。HUD がバーを咎めの色で返す。
    void Deny() { m_deniedAt = Time::unscaledTime; }
    // WHY Reset() にしないか: Script の Reset() はエディターのオーサリング用の
    //     仮想関数で、同じ名前を書くと «コンポーネントを Reset した» が息の初期化を
    //     呼ぶことになる (PlayerHealthComponent::ResetHealth と同じ理由)。
    void ResetBreath();

    void OnStart() override;
    void OnUpdate() override;

private:
    /// 足りなくても 0 で止めて true。息切れ中だけ false。
    bool Spend(float amount);
    void Gain(float amount);

    float m_breath = 0.0f;
    float m_dodgeSpent = 0.0f;
    float m_guardSpent = 0.0f;
    /// 回復が始まるまでの残り [秒]。息を使うたびに置き直す。
    float m_idle = 0.0f;
    bool  m_exhausted = false;
    /// 最後に «出せなかった» 実時刻。0 は «一度も無い» ではなく開始直後なので、
    /// 読む側は DeniedWithin() だけを使う。
    float m_deniedAt = -1.0e6f;
};

FBZZ_REFLECT(PlayerBreathComponent)

inline float PlayerBreathComponent::Normalized() const
{
    const float max = MaxBreath();
    if (max <= 0.0f) return 0.0f;
    return Clamp01(m_breath / max);
}

inline bool PlayerBreathComponent::DeniedWithin(float seconds) const
{
    return Time::unscaledTime - m_deniedAt <= std::max(seconds, 0.0f);
}

inline void PlayerBreathComponent::ResetBreath()
{
    m_breath    = MaxBreath();
    m_idle      = 0.0f;
    m_exhausted = false;
    m_deniedAt  = -1.0e6f;
    debugBreath    = m_breath;
    debugExhausted = false;
}

inline void PlayerBreathComponent::OnStart()
{
    // WHY ここだけ検算するか: 満タンが 0 だと開始時点で息切れ扱いになり、回避も
    //     ガードも «押しても何も起きない» としか画面に出ない。原因は入力側にも
    //     回避側にも見えないので、名指しで報告する。
    if (!tuning || tuning->breathMax <= 0.0f) {
        debug.LogError("PlayerBreathComponent has no usable PlayerTuning (Breath Max must be "
                       "greater than 0). Dodge and guard will never be available.");
    }
    m_dodgeSpent = 0.0f;
    m_guardSpent = 0.0f;
    ResetBreath();
}

inline bool PlayerBreathComponent::Spend(float amount)
{
    if (!CanAct()) {
        Deny();
        return false;
    }
    m_breath = std::max(0.0f, m_breath - std::max(amount, 0.0f));
    m_idle   = tuning ? std::max(tuning->breathRegenDelay, 0.0f) : 0.0f;
    if (m_breath <= 0.0f) m_exhausted = true;
    return true;
}

inline bool PlayerBreathComponent::DrainGuard(float dt)
{
    if (!tuning || m_exhausted) return false;
    const float before = m_breath;
    m_breath = std::max(0.0f, m_breath - std::max(tuning->breathGuardDrain, 0.0f)
                                             * std::max(dt, 0.0f));
    m_guardSpent += before - m_breath;
    m_idle   = std::max(tuning->breathRegenDelay, 0.0f);
    if (m_breath > 0.0f) return false;
    m_exhausted = true;
    return true;
}

inline void PlayerBreathComponent::Gain(float amount)
{
    // WHY 待ち時間 (m_idle) を触らないか: 攻めて戻すぶんは自然回復とは別の口で、
    //     斬った直後に «自然回復まで止まる» と、攻めた見返りが次の呼吸を遅らせる
    //     という逆向きの意味になる。
    m_breath = std::min(MaxBreath(), m_breath + std::max(amount, 0.0f));
}

inline void PlayerBreathComponent::Refill()
{
    m_breath    = MaxBreath();
    m_exhausted = false;
    m_idle      = 0.0f;
}

inline void PlayerBreathComponent::OnUpdate()
{
    const float max = MaxBreath();
    if (max <= 0.0f) return;

    const float dt = std::max(TimeManagerComponent::PlayerDeltaTime(), 0.0f);
    if (m_idle > 0.0f)
        m_idle = std::max(0.0f, m_idle - dt);
    else if (m_breath < max)
        m_breath = std::min(max, m_breath + std::max(tuning->breathRegenRate, 0.0f) * dt);

    if (m_exhausted && m_breath >= max * Clamp01(tuning->breathExhaustRecover))
        m_exhausted = false;

    debugBreath    = m_breath;
    debugExhausted = m_exhausted;
}

} // namespace sandbox
