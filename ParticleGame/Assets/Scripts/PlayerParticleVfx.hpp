// ParticleGame
// PlayerParticleVfx.hpp | particlegame
// Playerの戦闘状態へ追従する多層Particle演出
#pragma once

#include "SparkVacuumGame.hpp"
#include <Engine/Scene/Script.hpp>
#include <cmath>

namespace particlegame {

// PlayerParticleVfx — 1 GameObjectにつき1層を担当し、Particle設定の競合を避ける。
// WHY: Core・吸収・チャージ・必殺技を別Emitterに分けることで、発生数と寿命を個別調整できる。
class PlayerParticleVfx final : public fbzz::scene::Script {
    FBZZ_SCRIPT(PlayerParticleVfx)

public:
    FBZZ_GROUP("VFX Layer")
    FBZZ_FIELD_RANGE_INT(int, layerMode, 0, "Layer Mode", 0, 5)
    FBZZ_FIELD_RANGE(float, intensityScale, 1.0f, "Intensity Scale", 0.1f, 4.0f)

    void OnStart() override;
    void OnUpdate() override;

private:
    void ConfigureLayer();
    void SetContinuous(bool active);

    fbzz::scene::GameObject* m_player = nullptr;
    bool m_continuous = false;
    uint32_t m_lanceSerial = 0;
    uint32_t m_damageSerial = 0;
    uint32_t m_supernovaSerial = 0;
    uint32_t m_rebuildSerial = 0;
};

FBZZ_REFLECT(PlayerParticleVfx)

inline void PlayerParticleVfx::OnStart()
{
    m_player = scene.FindWithTag("Player");
    m_lanceSerial = SparkVacuumGame::LanceSerial();
    m_damageSerial = SparkVacuumGame::DamageSerial();
    m_supernovaSerial = SparkVacuumGame::SupernovaSerial();
    m_rebuildSerial = SparkVacuumGame::RebuildSerial();
    ConfigureLayer();
}

inline void PlayerParticleVfx::ConfigureLayer()
{
    particle.SetSimulationMode(fbzz::scene::ParticleSimulationMode::Gpu);
    particle.SetGravity(fbzz::math::Vector3::ZERO);
    particle.SetBlendMode(fbzz::scene::ParticleBlendMode::Additive);
    particle.SetReceiveForceFields(false);

    switch (layerMode) {
    case 0: // Core Aura
        particle.SetTexture("Assets/Textures/Particles/light_02.png");
        particle.SetSphereShape(1.15f);
        particle.SetEmitRate(420.0f * intensityScale);
        particle.SetLifetime(0.72f);
        particle.SetMaxParticles(900);
        particle.SetEmitVelocity({ 0.0f, 0.32f, 0.0f });
        particle.SetVelocitySpread(0.55f);
        particle.SetVelocityDamping(1.8f);
        particle.SetNoise(0.65f, 1.4f, 1.8f);
        particle.SetSize(0.11f, 0.018f);
        particle.Play(true);
        m_continuous = true;
        light.SetType(fbzz::scene::LightType::Point);
        light.SetRange(8.0f);
        light.SetEnabled(true);
        break;
    case 1: // Vacuum Vortex
        particle.SetTexture("Assets/Textures/Particles/twirl_02.png");
        particle.SetSphereShape(8.5f);
        particle.SetEmitRate(2600.0f * intensityScale);
        particle.SetLifetime(0.9f);
        particle.SetMaxParticles(3000);
        particle.SetEmitVelocity(fbzz::math::Vector3::ZERO);
        particle.SetVelocitySpread(0.3f);
        particle.SetVelocityDamping(2.8f);
        particle.SetNoise(1.1f, 0.75f, 2.4f);
        particle.SetSize(0.14f, 0.012f);
        particle.SetReceiveForceFields(true);
        particle.Stop(true);
        break;
    case 2: // Fire Charge
        // 手元へ集まる炎の輪で、離した瞬間に撃てるチャージ量を暖色で伝える。
        particle.SetTexture("Assets/Textures/Particles/muzzle_04.png");
        particle.SetSphereShape(0.82f);
        particle.SetEmitRate(0.0f);
        particle.SetLifetime(0.26f);
        particle.SetMaxParticles(1800);
        particle.SetEmitVelocity(fbzz::math::Vector3::ZERO);
        particle.SetVelocitySpread(1.1f);
        particle.SetVelocityDamping(4.0f);
        particle.SetNoise(1.8f, 2.2f, 3.8f);
        particle.SetSize(0.16f, 0.025f);
        particle.Stop(true);
        break;
    case 3: // Supernova
        particle.SetTexture("Assets/Textures/Particles/star_06.png");
        particle.SetSphereShape(0.32f);
        particle.SetEmitRate(0.0f);
        particle.SetLifetime(1.25f);
        particle.SetMaxParticles(6000);
        particle.SetEmitVelocity(fbzz::math::Vector3::ZERO);
        particle.SetVelocitySpread(12.0f);
        particle.SetVelocityDamping(1.1f);
        particle.SetNoise(1.5f, 0.6f, 2.6f);
        particle.SetSize(0.28f, 0.018f);
        particle.Stop(true);
        break;
    case 4: // Dash Slash
        particle.SetTexture("Assets/Textures/Particles/slash_02.png");
        particle.SetBoxShape({ 0.48f, 0.9f, 1.7f });
        particle.SetEmitRate(3200.0f * intensityScale);
        particle.SetLifetime(0.34f);
        particle.SetMaxParticles(1500);
        particle.SetEmitVelocity({ 0.0f, 0.12f, 0.0f });
        particle.SetVelocitySpread(0.7f);
        particle.SetVelocityDamping(3.2f);
        particle.SetNoise(0.9f, 1.8f, 3.0f);
        particle.SetSize(0.19f, 0.012f);
        particle.Stop(true);
        break;
    case 5: // Body Reconstruction
    default:
        // 外殻から中心へ落ち込む光点で、崩れた身体Particleの帰還経路を可視化する。
        particle.SetTexture("Assets/Textures/Particles/light_02.png");
        particle.SetSphereShape(2.4f);
        particle.SetEmitRate(0.0f);
        particle.SetLifetime(0.62f);
        particle.SetMaxParticles(2600);
        particle.SetEmitVelocity(fbzz::math::Vector3::ZERO);
        particle.SetVelocitySpread(1.8f);
        particle.SetVelocityDamping(1.4f);
        particle.SetNoise(0.85f, 1.3f, 3.2f);
        particle.SetSize(0.12f, 0.008f);
        particle.SetReceiveForceFields(true);
        particle.Stop(true);
        break;
    }
}

inline void PlayerParticleVfx::OnUpdate()
{
    if (!m_player) m_player = scene.FindWithTag("Player");
    if (!m_player || !transform) return;
    transform.position = m_player->transform.worldPosition + fbzz::math::Vector3::UP * 0.82f;

    const float body = SparkVacuumGame::VisualBodyRatio();
    const float charge = SparkVacuumGame::VisualLanceCharge();
    const float absorbed = SparkVacuumGame::VisualAbsorbedPower();
    const float pulse = 0.88f + 0.12f * std::sin(fbzz::Time::time * 7.5f);

    if (layerMode == 0) {
        particle.SetEmitRate((180.0f + body * 520.0f) * intensityScale * pulse);
        particle.SetSize(0.075f + body * 0.07f, 0.012f);
        const bool damaged = m_damageSerial != SparkVacuumGame::DamageSerial();
        if (damaged) {
            m_damageSerial = SparkVacuumGame::DamageSerial();
            particle.SetColor({ 1.0f, 0.08f, 0.2f, 1.0f }, { 0.4f, 0.0f, 0.04f, 0.0f });
            particle.Burst(850);
        } else if (body < 0.25f) {
            particle.SetColor({ 1.0f, 0.12f, 0.25f, 0.9f }, { 0.25f, 0.0f, 0.04f, 0.0f });
        } else {
            particle.SetColor({ 0.15f, 0.9f, 1.0f, 0.82f }, { 0.02f, 0.2f, 0.7f, 0.0f });
        }
        light.SetColor(body < 0.25f
            ? fbzz::math::Vector3{ 1.0f, 0.05f, 0.12f }
            : fbzz::math::Vector3{ 0.05f, 0.65f, 1.0f });
        light.SetIntensity((2.0f + body * 5.5f + SparkVacuumGame::VisualImpact() * 7.0f) * pulse);
        return;
    }

    if (layerMode == 1) {
        // E吸収はゲームプレイ判定だけ残し、画面を覆うVortex Particleは再生しない。
        SetContinuous(false);
        return;
    }

    if (layerMode == 2) {
        SetContinuous(charge > 0.001f);
        particle.SetEmitRate((700.0f + charge * 6200.0f + absorbed * 1800.0f) * intensityScale);
        particle.SetSize(0.09f + charge * 0.16f, 0.018f);
        particle.SetColor({ 1.0f, 0.42f + charge * 0.45f, 0.04f, 1.0f },
                          { 0.8f, 0.02f, 0.0f, 0.0f });
        if (m_lanceSerial != SparkVacuumGame::LanceSerial()) {
            m_lanceSerial = SparkVacuumGame::LanceSerial();
            particle.Burst(650 + static_cast<int>(charge * 900.0f));
        }
        return;
    }

    if (layerMode == 3) {
        if (m_supernovaSerial != SparkVacuumGame::SupernovaSerial()) {
            m_supernovaSerial = SparkVacuumGame::SupernovaSerial();
            particle.SetColor({ 0.9f, 0.98f, 1.0f, 1.0f }, { 0.15f, 0.25f, 1.0f, 0.0f });
            particle.Burst(static_cast<int>(4800.0f * intensityScale));
        }
        return;
    }

    if (layerMode == 4) {
        SetContinuous(SparkVacuumGame::VisualDashing());
        particle.SetColor({ 0.1f, 1.0f, 0.65f, 0.92f }, { 0.02f, 0.25f, 1.0f, 0.0f });
        return;
    }

    const float rebuild = SparkVacuumGame::VisualBodyRebuild();
    particle.SetSize(0.08f + rebuild * 0.1f, 0.006f);
    particle.SetColor({ 0.72f, 0.98f, 1.0f, 0.95f }, { 0.08f, 0.3f, 1.0f, 0.0f });
    if (m_rebuildSerial != SparkVacuumGame::RebuildSerial()) {
        m_rebuildSerial = SparkVacuumGame::RebuildSerial();
        particle.Burst(static_cast<int>(2100.0f * intensityScale));
    }
}

inline void PlayerParticleVfx::SetContinuous(bool active)
{
    if (m_continuous == active) return;
    m_continuous = active;
    if (active) particle.Play(true);
    else particle.Stop(false);
}

} // namespace particlegame
