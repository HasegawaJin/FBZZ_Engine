/// @file    BossGroundFireComponent.hpp
/// @brief   レーザーの地面着弾後に残る炎と、足元の継続ダメージ。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#pragma once

#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Utils/GroundFireArea.hpp>
#include <algorithm>

namespace sandbox {

class BossGroundFireComponent : public fbzz::scene::Script {
    FBZZ_SCRIPT(BossGroundFireComponent)
    FBZZ_REQUIRE_COMPONENT(fbzz::scene::ParticleEmitter)

public:
    fbzz::scene::EntityRef owner;
    float radius = 1.6f;
    float burnSeconds = 5.0f;
    int damage = 1;
    std::string playerTag = "Player";

    void OnStart() override
    {
        particle.SetSimulationMode(fbzz::scene::ParticleSimulationMode::Cpu);
        particle.SetSimulationSpace(fbzz::scene::ParticleSimulationSpace::Local);
        particle.SetRenderMode(fbzz::scene::ParticleRenderMode::VerticalBillboard);
        particle.SetReceiveFlowFields(false);
        particle.SetEmitPosition({0.0f, 0.55f, 0.0f});
        particle.SetBoxShape({radius * 0.55f, 0.0f, radius * 0.55f});
        particle.SetEmitVelocity({});
        particle.SetVelocitySpread(0.0f);
        particle.SetGravity({});
        particle.SetSize(radius * 1.6f, radius * 1.6f);
        particle.SetColor({1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 0.65f, 0.35f, 0.0f});
        particle.SetLifetime(kFadeSeconds);
        particle.SetMaxParticles(24);
        particle.SetEmitRate(10.0f);
        particle.SetPlayback(true, 1.6f);
        particle.Play();
        particle.Burst(8);
    }

    void Extinguish()
    {
        m_extinguished = true;
        particle.Stop(true);
    }

    void OnDisable() override { Extinguish(); }
    void OnDestroy() override { Extinguish(); }

    void OnUpdate() override
    {
        auto* boss = owner.Resolve(scene);
        if (m_extinguished || !boss || !boss->activeInHierarchy()) {
            Extinguish();
            scene.DestroySelf();
            return;
        }
        const float dt = std::max(time.DeltaTime(), 0.0f);
        if (dt <= 0.0f) return;
        const float previousAge = m_age;
        m_age += dt;
        m_hitCooldown = std::max(m_hitCooldown - dt, 0.0f);
        if (m_age >= kWarmupSeconds + burnSeconds) {
            particle.SetEmitRate(0.0f);
            if (m_age >= kWarmupSeconds + burnSeconds + kFadeSeconds) {
                Extinguish();
                scene.DestroySelf();
            }
            return;
        }
        auto* player = scene.FindWithTag(playerTag, true);
        if (!player) { m_hasPrevious = false; return; }
        const auto current = player->transform.worldPosition;
        auto previous = m_hasPrevious ? m_previous : current;
        m_previous = current;
        m_hasPrevious = true;
        if (damage <= 0 || m_age < kWarmupSeconds || m_hitCooldown > 0.0f) return;
        /// @note 着火する前に横断し終えた移動は、着火フレームでも被弾にしない。
        if (previousAge < kWarmupSeconds)
            previous += (current - previous) * std::clamp((kWarmupSeconds - previousAge) / dt, 0.0f, 1.0f);
        if ((current - previous).LengthSq() > 100.0f) previous = current;
        const auto center = scene.Self()->transform.worldPosition;
        if (!GroundFireArea::Intersects(previous, current, center, radius, 1.4f)) return;
        if (auto* combat = CombatManagerComponent::Instance()) {
            m_hitCooldown = 0.8f;
            combat->HitPlayer(player, damage, &center, PlayerHitKind::Unblockable);
        }
    }

private:
    static constexpr float kWarmupSeconds = 0.35f;
    static constexpr float kFadeSeconds = 1.6f;
    float m_age = 0.0f;
    float m_hitCooldown = 0.0f;
    fbzz::math::Vector3 m_previous{};
    bool m_hasPrevious = false;
    bool m_extinguished = false;
};

FBZZ_REFLECT(BossGroundFireComponent)

} // namespace sandbox
