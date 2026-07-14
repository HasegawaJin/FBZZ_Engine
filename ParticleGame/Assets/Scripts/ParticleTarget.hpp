// ParticleGame
// ParticleTarget.hpp | particlegame
// PlayerへParticle弾を撃ち返す破壊可能なParticle標的
#pragma once

#include "ParticleCombatShared.hpp"
#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace particlegame {

// ParticleTarget — 射撃判定、耐久値、再形成待ち時間を所有するParticle製の標的。
class ParticleTarget final : public fbzz::scene::Script {
    FBZZ_SCRIPT(ParticleTarget)

public:
    FBZZ_GROUP("Combat")
    FBZZ_FIELD_RANGE_INT(int, maxHealth, 3, "Health", 1, 20)
    FBZZ_FIELD_RANGE(float, fireInterval, 2.2f, "Fire Interval", 0.2f, 10.0f)
    FBZZ_FIELD_RANGE(float, respawnDelay, 4.0f, "Respawn Delay", 0.5f, 30.0f)
    FBZZ_FIELD_RANGE(float, hitRadius, 0.75f, "Hit Radius", 0.1f, 5.0f)

    void OnStart() override;
    void OnUpdate() override;
    void OnDestroy() override;

    bool TickFire(float deltaTime);
    fbzz::math::Vector3 WorldPosition() const;
    static ParticleTarget* TryHitAt(const fbzz::math::Vector3& position, float radius, int damage,
                                    const std::vector<ParticleTarget*>* ignored = nullptr);
    static void DamageAll(int damage);
    static const std::vector<ParticleTarget*>& ActiveTargets() { return s_targets; }

private:
    void ApplyHit(int damage);
    void Respawn();

    int m_health = 3;
    float m_fireTimer = 1.0f;
    float m_respawnTimer = 0.0f;
    float m_hitCooldown = 0.0f;
    bool m_alive = true;

    static inline std::vector<ParticleTarget*> s_targets;
};

FBZZ_REFLECT(ParticleTarget)

inline void ParticleTarget::OnStart()
{
    m_health = maxHealth;
    m_fireTimer = fireInterval * 0.5f;
    m_alive = true;
    s_targets.push_back(this);

    // TargetもParticlesフォルダのTextureだけで構成し、破壊時は一度崩して再形成する。
    particle.SetTexture("Assets/Textures/Particles/spark_01.png");
    particle.SetSphereShape(0.55f);
    particle.SetSimulationMode(fbzz::scene::ParticleSimulationMode::Gpu);
    particle.SetEmitRate(950.0f);
    particle.SetLifetime(0.55f);
    particle.SetMaxParticles(700);
    particle.SetEmitVelocity({ 0.0f, 0.25f, 0.0f });
    particle.SetVelocitySpread(0.35f);
    particle.SetGravity(fbzz::math::Vector3::ZERO);
    particle.SetSize(0.09f, 0.025f);
    particle.SetColor({ 1.0f, 0.25f, 0.45f, 1.0f }, { 0.35f, 0.02f, 0.12f, 0.0f });
    particle.SetReceiveForceFields(false);
    particle.Play(true);
}

inline void ParticleTarget::OnUpdate()
{
    m_hitCooldown = std::max(0.0f, m_hitCooldown - fbzz::Time::deltaTime);
    if (m_alive) {
        // 呼吸するような明滅とHP連動色で、攻撃可能な標的と弱り具合を常時伝える。
        const float healthRatio = static_cast<float>(m_health) /
                                  static_cast<float>(std::max(1, maxHealth));
        const float pulse = 0.5f + 0.5f * std::sin(
            fbzz::Time::time * 4.5f + transform.worldPosition.x * 0.25f);
        particle.SetSize(0.075f + pulse * 0.035f, 0.018f);
        particle.SetColor(
            { 1.0f, 0.08f + healthRatio * 0.28f, 0.12f + healthRatio * 0.58f, 1.0f },
            { 0.32f, 0.0f, 0.04f + healthRatio * 0.18f, 0.0f });
        return;
    }
    m_respawnTimer -= fbzz::Time::deltaTime;
    if (m_respawnTimer <= 0.0f) Respawn();
}

inline void ParticleTarget::OnDestroy()
{
    std::erase(s_targets, this);
}

inline bool ParticleTarget::TickFire(float deltaTime)
{
    if (!m_alive) return false;
    m_fireTimer -= std::max(0.0f, deltaTime);
    if (m_fireTimer > 0.0f) return false;
    m_fireTimer = fireInterval;
    return true;
}

inline fbzz::math::Vector3 ParticleTarget::WorldPosition() const
{
    return transform ? transform.worldPosition : fbzz::math::Vector3::ZERO;
}

inline ParticleTarget* ParticleTarget::TryHitAt(
    const fbzz::math::Vector3& position, float radius, int damage,
    const std::vector<ParticleTarget*>* ignored)
{
    for (ParticleTarget* target : s_targets) {
        if (!target || !target->m_alive || !target->transform || target->m_hitCooldown > 0.0f)
            continue;
        if (ignored && std::find(ignored->begin(), ignored->end(), target) != ignored->end())
            continue;
        const float combinedRadius = std::max(0.0f, radius) + target->hitRadius;
        if ((target->transform.worldPosition - position).LengthSq() > combinedRadius * combinedRadius)
            continue;
        target->ApplyHit(damage);
        return target;
    }
    return nullptr;
}

inline void ParticleTarget::DamageAll(int damage)
{
    for (ParticleTarget* target : s_targets) {
        if (target && target->m_alive) target->ApplyHit(damage);
    }
}

inline void ParticleTarget::ApplyHit(int damage)
{
    if (!m_alive) return;
    m_hitCooldown = 0.12f;
    m_health -= std::max(0, damage);
    particle.Burst(120);
    ParticleCombatEvents::AddScore(50);
    ParticleCombatEvents::AddEnemyHit(m_health <= 0);
    if (m_health > 0) return;

    m_alive = false;
    m_respawnTimer = respawnDelay;
    particle.Burst(500);
    particle.Stop(false);
}

inline void ParticleTarget::Respawn()
{
    m_alive = true;
    m_health = maxHealth;
    m_fireTimer = fireInterval * 0.5f;
    particle.Play(true);
}

} // namespace particlegame
