/// @file    HealthComponent.hpp
/// @brief   観測値を複製せずに公開するユーザー Script のコンパイル検証用例。
/// @author  Hasegawa Jin
/// @date    2026-09-26
#pragma once
#include <Engine/Scene/Script.hpp>

namespace sandbox {

class HealthComponent : public fbzz::scene::Script {
    FBZZ_SCRIPT(HealthComponent)
    FBZZ_FIELD_RANGE_INT(int, maxHealth, 100, "最大HP", 1, 100000)
    FBZZ_OBSERVE(int, currentHealth, m_currentHealth, "現在HP")
    FBZZ_OBSERVE(bool, alive, IsAlive(), "生存")

    void OnStart() override { m_currentHealth = maxHealth > 0 ? maxHealth : 1; }
    bool TakeDamage(int amount)
    {
        if (amount <= 0 || !IsAlive()) return false;
        m_currentHealth = amount >= m_currentHealth ? 0 : m_currentHealth - amount;
        return true;
    }
    [[nodiscard]] int CurrentHealth() const { return m_currentHealth; }
    [[nodiscard]] bool IsAlive() const { return m_currentHealth > 0; }

private:
    int m_currentHealth = 0;
};
FBZZ_REFLECT(HealthComponent)

} /// namespace sandbox
