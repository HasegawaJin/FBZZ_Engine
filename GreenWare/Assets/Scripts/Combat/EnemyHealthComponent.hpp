// FBZZ Engine
// EnemyHealthComponent.hpp | sandbox
// 極性衝突だけで減る敵 HP と死亡処理
#pragma once

#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/IDamageable.hpp>
#include <Scripts/Polarity/PolarityBodyComponent.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class EnemyHealthComponent : public Script, public IDamageable {
    FBZZ_SCRIPT_DERIVED(EnemyHealthComponent, Script, IDamageable)
    // 引力運動に必要な剛体を構成上の必須条件にする。銃撃ダメージ用の入口は意図的に持たない。
    FBZZ_REQUIRE_COMPONENT(RigidBodyComponent)
    FBZZ_OPTIONAL_COMPONENT(AudioSourceComponent)

public:
    FBZZ_GROUP("Health")
    FBZZ_FIELD_RANGE_INT(int, maxHealth, 100, "Max Health", 1, 10000)

    FBZZ_GROUP("Impact Damage")
    // 企画書 18.2 は数値を未決としているため、式を固定せず調整値として公開する。
    FBZZ_FIELD_RANGE_INT(int, baseImpactDamage, 100, "Base Damage", 0, 10000)
    FBZZ_FIELD_RANGE(float, speedDamageScale, 0.0f, "Speed Scale", 0.0f, 100.0f)
    FBZZ_FIELD_RANGE(float, anchorDamageMultiplier, 1.0f, "Anchor Multiplier", 0.0f, 10.0f)
    FBZZ_TOOLTIP("柱・壁へ叩きつけた場合だけ掛ける倍率")
    FBZZ_FIELD_RANGE(float, destroyDelay, 0.05f, "Destroy Delay", 0.0f, 5.0f)

    FBZZ_GROUP("Feedback")
    FBZZ_FIELD_AUDIO(sfxHit, "", "SFX Hit")
    FBZZ_FIELD_AUDIO(sfxDeath, "", "SFX Death")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugHealth, 0, "Health")

    // ── IDamageable ─────────────────────────────────────────────────────────
    // WHY 極性衝突の入口 (TakeImpact) と別に持つか: あちらは衝突の速度と柱倍率から
    //     ダメージ量を算出する「この敵に固有の式」で、渡ってくるのは PolarityImpact。
    //     こちらは量が決まった後の適用口で、罠や環境ダメージのように盤面を経由しない
    //     経路から呼ばれる。式と適用を分けておくと、どちらを足しても片方に影響しない。
    bool ApplyDamage(int amount) override;
    [[nodiscard]] int  CurrentHealth() const override { return m_health; }
    [[nodiscard]] int  MaxHealth()     const override { return std::max(maxHealth, 1); }
    [[nodiscard]] bool IsAlive()       const override { return m_health > 0; }

    /// CurrentHealth() の別名。既存の呼び出し側がこちらを使っている。
    [[nodiscard]] int Current() const { return m_health; }
    [[nodiscard]] float Normalized() const
    {
        return maxHealth > 0
            ? Clamp01(static_cast<float>(m_health) / static_cast<float>(maxHealth))
            : 0.0f;
    }

    // true はこの呼び出しで死亡したことを表す。Wave/リザルト側が撃破数を重複加算しないために使う。
    bool TakeImpact(const PolarityImpact& impact);
    void ResetHealth();
    void OnStart() override
    {
        ResetHealth();
        // 敵は盤面のあちこちに居る。どの方向で何が起きたかが分かる必要があるので 3D。
        se::EnsureSource(scene, "SE", 1.0f);
        if (!scene.GetScript<PolarityBodyComponent>())
            debug.LogError("EnemyHealthComponent requires PolarityBodyComponent on the same object.");
    }

private:
    /// 量が決まった後の適用。ひるみ / 撃破の反応もここで返す。
    /// @ret この呼び出しで死亡したら true。
    bool Deal(int damage);

    int m_health = 0;
};

FBZZ_REFLECT(EnemyHealthComponent)

inline void EnemyHealthComponent::ResetHealth()
{
    m_health = std::max(maxHealth, 1);
    debugHealth = m_health;
}

inline bool EnemyHealthComponent::ApplyDamage(int amount)
{
    if (amount <= 0 || !IsAlive()) return false;
    Deal(amount);
    return true;
}

inline bool EnemyHealthComponent::TakeImpact(const PolarityImpact& impact)
{
    if (!IsAlive()) return false;

    const float speedPart = std::max(impact.speed, 0.0f) * std::max(speedDamageScale, 0.0f);
    float damageValue = static_cast<float>(std::max(baseImpactDamage, 0)) + speedPart;
    if (impact.struckIsAnchor)
        damageValue *= std::max(anchorDamageMultiplier, 0.0f);

    return Deal(std::max(1, static_cast<int>(std::lround(damageValue))));
}

inline bool EnemyHealthComponent::Deal(int damage)
{
    m_health = std::max(0, m_health - damage);
    debugHealth = m_health;

    if (m_health > 0) {
        se::Play(audio, sfxHit, se::kEnemyFlinch);
        return false;
    }

    // WHY 自分ではなく位置で鳴らすか: destroyDelay の後にこの GameObject は消える。
    //     自分の AudioSource で鳴らすと、撃破音が鳴り終わる前に音源ごと消えて
    //     途中で切れる。撃破は「そこで起きたこと」なので、場所に残す。
    if (!sfxDeath.empty()) audio.PlayAtPoint(sfxDeath, transform.worldPosition);
    else                   se::PlayAt(audio, se::kEnemyDestroy, transform.worldPosition);
    scene.DestroySelf(std::max(destroyDelay, 0.0f));
    return true;
}

} // namespace sandbox
