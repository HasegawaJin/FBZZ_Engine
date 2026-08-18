// FBZZ Engine
// EnemyHealthComponent.hpp | sandbox
// 極性衝突だけで減る敵 HP と死亡処理
#pragma once

#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Polarity/PolarityBodyComponent.hpp>
#include <algorithm>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class EnemyHealthComponent : public Script {
    FBZZ_SCRIPT(EnemyHealthComponent)
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
    FBZZ_FIELD_FILE(sfxHit, "", "SFX Hit", ".wav,.ogg")
    FBZZ_FIELD_FILE(sfxDeath, "", "SFX Death", ".wav,.ogg")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugHealth, 0, "Health")

    [[nodiscard]] int Current() const { return m_health; }
    [[nodiscard]] bool IsAlive() const { return m_health > 0; }
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
        if (!scene.GetScript<PolarityBodyComponent>())
            debug.LogError("EnemyHealthComponent requires PolarityBodyComponent on the same object.");
    }

private:
    int m_health = 0;
};

FBZZ_REFLECT(EnemyHealthComponent)

inline void EnemyHealthComponent::ResetHealth()
{
    m_health = std::max(maxHealth, 1);
    debugHealth = m_health;
}

inline bool EnemyHealthComponent::TakeImpact(const PolarityImpact& impact)
{
    if (!IsAlive()) return false;

    const float speedPart = std::max(impact.speed, 0.0f) * std::max(speedDamageScale, 0.0f);
    float damageValue = static_cast<float>(std::max(baseImpactDamage, 0)) + speedPart;
    if (impact.struckIsAnchor)
        damageValue *= std::max(anchorDamageMultiplier, 0.0f);

    const int damage = std::max(1, static_cast<int>(std::lround(damageValue)));
    m_health = std::max(0, m_health - damage);
    debugHealth = m_health;

    if (m_health > 0) {
        if (!sfxHit.empty()) audio.PlayOneShot(sfxHit);
        return false;
    }

    if (!sfxDeath.empty()) audio.PlayOneShot(sfxDeath);
    scene.DestroySelf(std::max(destroyDelay, 0.0f));
    return true;
}

} // namespace sandbox
