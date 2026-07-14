// ParticleGame
// ParticleProjectile.hpp | particlegame
// 炎の自機弾・敵弾・被弾時の散乱粒子
#pragma once

#include "ParticleCombatShared.hpp"
#include "ParticleTarget.hpp"
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace particlegame {

enum class ParticleProjectileFaction {
    Player,
    Enemy,
    Neutral
};

// ParticleProjectile — Sceneに常駐する小さなPool要素として、弾生成時のヒープ確保を避ける。
class ParticleProjectile final : public fbzz::scene::Script {
    FBZZ_SCRIPT(ParticleProjectile)

public:
    FBZZ_GROUP("Projectile")
    FBZZ_FIELD_RANGE(float, speed, 18.0f, "Speed", 1.0f, 80.0f)
    FBZZ_FIELD_RANGE(float, lifetime, 1.8f, "Lifetime", 0.1f, 10.0f)
    FBZZ_FIELD_RANGE(float, hitRadius, 0.3f, "Hit Radius", 0.05f, 2.0f)
    FBZZ_FIELD_RANGE_INT(int, damage, 1, "Damage", 1, 100)

    void OnStart() override;
    void OnUpdate() override;
    void OnDestroy() override;

    static bool Fire(const fbzz::math::Vector3& origin, const fbzz::math::Vector3& direction,
                     ParticleProjectileFaction faction);
    static bool FireLance(const fbzz::math::Vector3& origin,
                          const fbzz::math::Vector3& direction, float chargeRatio);
    static int AbsorbEnemyProjectiles(const fbzz::math::Vector3& center, float radius,
                                      float pullStrength, float deltaTime);
    static int ClearEnemyProjectiles(const fbzz::math::Vector3& center, float radius);
    static void Scatter(const fbzz::math::Vector3& origin, int count);

private:
    void Activate(const fbzz::math::Vector3& origin, const fbzz::math::Vector3& direction,
                  ParticleProjectileFaction faction);
    static ParticleProjectile* Acquire();
    void Deactivate();

    bool m_active = false;
    float m_remaining = 0.0f;
    fbzz::math::Vector3 m_direction = fbzz::math::Vector3::FORWARD;
    ParticleProjectileFaction m_faction = ParticleProjectileFaction::Neutral;
    int m_runtimeDamage = 1;
    float m_runtimeHitRadius = 0.3f;
    int m_pierceRemaining = 0;
    std::vector<ParticleTarget*> m_hitTargets;

    static inline std::vector<ParticleProjectile*> s_pool;
    static inline size_t s_reuseCursor = 0;
};

FBZZ_REFLECT(ParticleProjectile)

inline void ParticleProjectile::OnStart()
{
    s_pool.push_back(this);
    particle.SetTexture("Assets/Textures/Particles/magic_01.png");
    particle.SetShape(fbzz::scene::ParticleEmitterShape::Point);
    particle.SetSimulationMode(fbzz::scene::ParticleSimulationMode::Gpu);
    particle.SetEmitRate(720.0f);
    particle.SetLifetime(0.28f);
    particle.SetMaxParticles(260);
    particle.SetEmitVelocity(fbzz::math::Vector3::ZERO);
    particle.SetVelocitySpread(0.4f);
    particle.SetGravity(fbzz::math::Vector3::ZERO);
    particle.SetVelocityDamping(1.5f);
    particle.SetSize(0.085f, 0.015f);
    particle.SetReceiveForceFields(false);
    particle.Stop(true);
}

inline void ParticleProjectile::OnUpdate()
{
    if (!m_active || !transform) return;

    m_remaining -= fbzz::Time::deltaTime;
    transform.position += m_direction * (speed * fbzz::Time::deltaTime);

    if (m_faction == ParticleProjectileFaction::Player) {
        if (ParticleTarget* hit = ParticleTarget::TryHitAt(
                transform.worldPosition, m_runtimeHitRadius, m_runtimeDamage, &m_hitTargets)) {
            m_hitTargets.push_back(hit);
            particle.Burst(80);
            if (m_pierceRemaining <= 0) {
                Deactivate();
                return;
            }
            --m_pierceRemaining;
        }
    } else if (m_faction == ParticleProjectileFaction::Enemy) {
        if (auto* player = scene.FindWithTag("Player")) {
            constexpr float PLAYER_HIT_RADIUS = 0.55f;
            const float combinedRadius = m_runtimeHitRadius + PLAYER_HIT_RADIUS;
            if ((player->transform.worldPosition - transform.worldPosition).LengthSq()
                <= combinedRadius * combinedRadius) {
                ParticleCombatEvents::AddPlayerDamage(damage);
                particle.Burst(100);
                Deactivate();
                return;
            }
        }
    }

    if (m_remaining <= 0.0f) Deactivate();
}

inline void ParticleProjectile::OnDestroy()
{
    std::erase(s_pool, this);
}

inline ParticleProjectile* ParticleProjectile::Acquire()
{
    if (s_pool.empty()) return nullptr;
    for (ParticleProjectile* projectile : s_pool) {
        if (projectile && !projectile->m_active) return projectile;
    }

    // Poolが満杯なら巡回再利用し、連射入力を無視して操作感が途切れることを避ける。
    s_reuseCursor %= s_pool.size();
    return s_pool[s_reuseCursor++];
}

inline bool ParticleProjectile::Fire(const fbzz::math::Vector3& origin,
                                     const fbzz::math::Vector3& direction,
                                     ParticleProjectileFaction faction)
{
    if (direction.LengthSq() <= fbzz::math::EPSILON) return false;
    ParticleProjectile* projectile = Acquire();
    if (!projectile) return false;
    projectile->Activate(origin, direction, faction);
    return true;
}

inline bool ParticleProjectile::FireLance(const fbzz::math::Vector3& origin,
                                          const fbzz::math::Vector3& direction,
                                          float chargeRatio)
{
    if (direction.LengthSq() <= fbzz::math::EPSILON) return false;
    ParticleProjectile* projectile = Acquire();
    if (!projectile) return false;

    const float charge = std::clamp(chargeRatio, 0.0f, 1.0f);
    projectile->Activate(origin, direction, ParticleProjectileFaction::Player);
    projectile->m_runtimeDamage = 1 + static_cast<int>(std::round(charge * 4.0f));
    projectile->m_runtimeHitRadius = projectile->hitRadius + charge * 0.55f;
    projectile->m_pierceRemaining = static_cast<int>(std::round(charge * 3.0f));
    projectile->particle.SetEmitRate(720.0f + 1100.0f * charge);
    projectile->particle.SetSize(0.085f + 0.12f * charge, 0.015f + 0.025f * charge);
    return true;
}

inline int ParticleProjectile::AbsorbEnemyProjectiles(const fbzz::math::Vector3& center,
                                                       float radius, float pullStrength,
                                                       float deltaTime)
{
    int absorbed = 0;
    const float radiusSq = radius * radius;
    for (ParticleProjectile* projectile : s_pool) {
        if (!projectile || !projectile->m_active
            || projectile->m_faction != ParticleProjectileFaction::Enemy
            || !projectile->transform)
            continue;

        const fbzz::math::Vector3 toCenter = center - projectile->transform.worldPosition;
        const float distanceSq = toCenter.LengthSq();
        if (distanceSq > radiusSq || distanceSq <= fbzz::math::EPSILON) continue;
        const float distance = std::sqrt(distanceSq);
        const float attraction = std::clamp(pullStrength * deltaTime / std::max(distance, 0.25f),
                                            0.0f, 0.92f);
        projectile->m_direction = fbzz::math::Vector3::Lerp(
            projectile->m_direction, toCenter.Normalized(), attraction).Normalized();
        if (distance <= 1.25f) {
            projectile->particle.Burst(90);
            projectile->Deactivate();
            ++absorbed;
        }
    }
    return absorbed;
}

inline int ParticleProjectile::ClearEnemyProjectiles(const fbzz::math::Vector3& center, float radius)
{
    int cleared = 0;
    const float radiusSq = radius * radius;
    for (ParticleProjectile* projectile : s_pool) {
        if (!projectile || !projectile->m_active
            || projectile->m_faction != ParticleProjectileFaction::Enemy
            || !projectile->transform)
            continue;
        if ((projectile->transform.worldPosition - center).LengthSq() > radiusSq) continue;
        projectile->particle.Burst(100);
        projectile->Deactivate();
        ++cleared;
    }
    return cleared;
}

inline void ParticleProjectile::Scatter(const fbzz::math::Vector3& origin, int count)
{
    constexpr float TWO_PI = 6.28318530717958647692f;
    const int clampedCount = std::clamp(count, 0, 12);
    for (int i = 0; i < clampedCount; ++i) {
        const float angle = TWO_PI * static_cast<float>(i) / static_cast<float>(clampedCount);
        const fbzz::math::Vector3 direction{
            std::cos(angle), 0.25f + 0.1f * static_cast<float>(i % 3), std::sin(angle)
        };
        Fire(origin, direction.Normalized(), ParticleProjectileFaction::Neutral);
    }
}

inline void ParticleProjectile::Activate(const fbzz::math::Vector3& origin,
                                         const fbzz::math::Vector3& direction,
                                         ParticleProjectileFaction faction)
{
    if (!transform) return;
    transform.position = origin;
    m_direction = direction.Normalized();
    m_faction = faction;
    m_runtimeDamage = damage;
    m_runtimeHitRadius = hitRadius;
    m_pierceRemaining = 0;
    m_hitTargets.clear();
    m_remaining = faction == ParticleProjectileFaction::Neutral ? lifetime * 0.45f : lifetime;
    m_active = true;

    if (faction == ParticleProjectileFaction::Enemy) {
        particle.SetEmitRate(720.0f);
        particle.SetSize(0.085f, 0.015f);
        // 敵弾は暖色の火花にして、青いPlayer弾と視線だけで判別できるようにする。
        particle.SetTexture("Assets/Textures/Particles/muzzle_04.png");
        particle.SetColor({ 1.0f, 0.12f, 0.28f, 1.0f }, { 0.4f, 0.0f, 0.05f, 0.0f });
    } else if (faction == ParticleProjectileFaction::Player) {
        particle.SetEmitRate(720.0f);
        particle.SetSize(0.085f, 0.015f);
        // 炎弾は暖色の中心と暗い残炎を組み合わせ、敵の赤紫弾とは輪郭でも区別する。
        particle.SetTexture("Assets/Textures/Particles/muzzle_04.png");
        particle.SetColor({ 1.0f, 0.82f, 0.12f, 1.0f }, { 1.0f, 0.08f, 0.0f, 0.0f });
    } else {
        particle.SetEmitRate(720.0f);
        particle.SetSize(0.085f, 0.015f);
        particle.SetTexture("Assets/Textures/Particles/spark_06.png");
        particle.SetColor({ 0.6f, 0.8f, 1.0f, 0.85f }, { 0.05f, 0.1f, 0.4f, 0.0f });
    }
    particle.Play(true);
}

inline void ParticleProjectile::Deactivate()
{
    m_active = false;
    m_remaining = 0.0f;
    particle.Stop(false);
}

} // namespace particlegame
