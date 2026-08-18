// FBZZ Engine
// GameFlowComponent.hpp | sandbox
// 衝突ダメージ、敵全滅、プレイヤー死亡、HUD とリザルト遷移を統括する
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Game/GameResultState.hpp>
#include <Scripts/Player/PlayerHealthComponent.hpp>
#include <Scripts/Player/PolarityGunComponent.hpp>
#include <Scripts/Polarity/PolarityFieldComponent.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using fbzz::Time;

namespace sandbox {

class GameFlowComponent : public Script {
    FBZZ_SCRIPT(GameFlowComponent)

public:
    FBZZ_GROUP("Scenes")
    FBZZ_FIELD(std::string, resultScene, "Result", "Result Scene")
    FBZZ_FIELD_RANGE(float, endDelay, 0.8f, "End Delay", 0.0f, 5.0f)

    FBZZ_GROUP("HUD Names")
    FBZZ_FIELD(std::string, healthTextName, "HUD_Health", "Health Text")
    FBZZ_FIELD(std::string, enemyTextName, "HUD_Enemies", "Enemy Text")
    FBZZ_FIELD(std::string, gunTextName, "HUD_Guns", "Gun Text")
    FBZZ_FIELD(std::string, objectiveTextName, "HUD_Objective", "Objective Text")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugEnemies, 0, "Enemies")
    FBZZ_FIELD_READ_ONLY(int, debugKills, 0, "Kills")
    FBZZ_FIELD_READ_ONLY(float, debugElapsed, 0.0f, "Elapsed")

    void OnStart() override;
    void OnUpdate() override;

private:
    void ResolveImpact(const PolarityImpact& impact);
    void BeginEnd(bool victory);
    void RefreshHud(int enemiesAlive);
    [[nodiscard]] int CountEnemies() const;

    PlayerHealthComponent* m_playerHealth = nullptr;
    PolarityGunComponent* m_gun = nullptr;
    bool m_sawEnemy = false;
    bool m_ending = false;
    bool m_victory = false;
    float m_endRemaining = 0.0f;
    float m_elapsed = 0.0f;
    int m_kills = 0;
    int m_enemyImpacts = 0;
    int m_anchorImpacts = 0;
};

FBZZ_REFLECT(GameFlowComponent)

inline void GameFlowComponent::OnStart()
{
    m_elapsed = 0.0f;
    m_kills = 0;
    m_enemyImpacts = 0;
    m_anchorImpacts = 0;
    m_ending = false;
    m_sawEnemy = CountEnemies() > 0;

    if (auto* field = scene.GetScript<PolarityFieldComponent>())
        field->onImpact = [this](const PolarityImpact& impact) { ResolveImpact(impact); };

    if (GameObject* player = scene.FindWithTag("Player")) {
        m_playerHealth = scene.GetScript<PlayerHealthComponent>(player);
        m_gun = scene.GetScript<PolarityGunComponent>(player);
        if (m_playerHealth)
            m_playerHealth->onDeath = [this]() { BeginEnd(false); };
    }

    if (!m_playerHealth)
        debug.LogError("GameFlowComponent requires a PlayerHealthComponent on the Player-tagged object.");
}

inline int GameFlowComponent::CountEnemies() const
{
    int count = 0;
    for (GameObject* object : scene.FindObjectsOfType<EnemyHealthComponent>()) {
        const auto* health = scene.GetScript<EnemyHealthComponent>(object);
        if (object && object->activeInHierarchy() && health && health->IsAlive()) ++count;
    }
    return count;
}

inline void GameFlowComponent::ResolveImpact(const PolarityImpact& impact)
{
    if (impact.struckIsAnchor) ++m_anchorImpacts;
    else ++m_enemyImpacts;

    if (auto* mover = scene.GetScript<EnemyHealthComponent>(impact.mover))
        if (mover->TakeImpact(impact)) ++m_kills;

    if (!impact.struckIsAnchor && impact.struck != impact.mover) {
        if (auto* struck = scene.GetScript<EnemyHealthComponent>(impact.struck))
            if (struck->TakeImpact(impact)) ++m_kills;
    }
    debugKills = m_kills;
}

inline void GameFlowComponent::BeginEnd(bool victory)
{
    if (m_ending) return;
    m_ending = true;
    m_victory = victory;
    m_endRemaining = std::max(endDelay, 0.0f);
}

inline void GameFlowComponent::RefreshHud(int enemiesAlive)
{
    if (GameObject* text = scene.Find(healthTextName)) {
        const int health = m_playerHealth ? m_playerHealth->Current() : 0;
        const int maxHealth = m_playerHealth ? m_playerHealth->MaxHealth() : 0;
        ui.SetText(text, "HP " + std::to_string(health) + " / " + std::to_string(maxHealth));
    }
    if (GameObject* text = scene.Find(enemyTextName))
        ui.SetText(text, "ENEMIES " + std::to_string(enemiesAlive));
    if (GameObject* text = scene.Find(gunTextName)) {
        const int plus = m_gun ? static_cast<int>(m_gun->ChargeOf(Polarity::Plus) * 100.0f) : 0;
        const int minus = m_gun ? static_cast<int>(m_gun->ChargeOf(Polarity::Minus) * 100.0f) : 0;
        ui.SetText(text, "+ " + std::to_string(plus) + "%    - " + std::to_string(minus) + "%");
    }
    if (GameObject* text = scene.Find(objectiveTextName))
        ui.SetText(text, m_ending ? (m_victory ? "AREA CLEAR" : "SYSTEM DOWN")
                                  : "RED +  BLUE -  COLLIDE ENEMIES");
}

inline void GameFlowComponent::OnUpdate()
{
    if (!m_ending) m_elapsed += Time::deltaTime;
    debugElapsed = m_elapsed;

    const int enemies = CountEnemies();
    debugEnemies = enemies;
    if (enemies > 0) m_sawEnemy = true;
    if (!m_ending && m_sawEnemy && enemies == 0) BeginEnd(true);
    RefreshHud(enemies);

    if (!m_ending) return;
    m_endRemaining -= Time::deltaTime;
    if (m_endRemaining > 0.0f) return;

    GameResultState::victory = m_victory;
    GameResultState::clearSeconds = m_elapsed;
    GameResultState::defeatedEnemies = m_kills;
    GameResultState::enemyImpacts = m_enemyImpacts;
    GameResultState::anchorImpacts = m_anchorImpacts;
    scene.LoadScene(resultScene);
}

} // namespace sandbox
