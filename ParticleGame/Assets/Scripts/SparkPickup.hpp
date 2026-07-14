// ParticleGame
// SparkPickup.hpp | particlegame
// 浮遊Sparkを吸引フィールドへ追従させ、接近時に回収する
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include "SparkVacuumGame.hpp"

namespace particlegame {

// SparkPickup — ParticleEmitterの発生源そのものをPlayerへ移動させる回収対象。
// WHY: 粒子readbackに依存せず、Emitterの軌跡とゲーム判定を同じ座標で扱う。
class SparkPickup final : public fbzz::scene::Script {
    FBZZ_SCRIPT(SparkPickup)

public:
    FBZZ_FIELD_RANGE_INT(int, baseScore, 100, "Base Score", 0, 10000)
    FBZZ_FIELD_RANGE(float, vacuumRadius, 7.0f, "Vacuum Radius", 1.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, collectRadius, 0.9f, "Collect Radius", 0.1f, 3.0f)
    FBZZ_FIELD_RANGE(float, pullSpeed, 7.0f, "Pull Speed", 0.1f, 30.0f)

    void OnStart() override;
    void OnUpdate() override;

private:
    fbzz::scene::GameObject* m_player = nullptr;
    bool m_collected = false;
    float m_swirlTime = 0.0f;
};

FBZZ_REFLECT(SparkPickup)

inline void SparkPickup::OnStart()
{
    m_player = scene.FindWithTag("Player");
    // 全回収SparkはParticlesフォルダの共通Sparkテクスチャを使う。
    particle.SetTexture("Assets/Textures/Particles/spark_01.png");
    particle.SetReceiveForceFields(true);
    particle.Play(true);
}

inline void SparkPickup::OnUpdate()
{
    if (m_collected || !SparkVacuumGame::IsVacuumActive()) return;
    if (!m_player) m_player = scene.FindWithTag("Player");
    if (!m_player) return;

    const float distance = transform.DistanceTo(*m_player);
    // WHY: 表示上の半径より少し広く捕捉し、境界付近でEを押したのに反応しない感覚をなくす。
    const float assistedRadius = vacuumRadius * 1.65f;
    if (distance > assistedRadius) return;

    if (distance <= collectRadius * 1.25f) {
        m_collected = true;
        SparkVacuumGame::RegisterSpark(baseScore);
        particle.SetColor({ 1.0f, 0.95f, 0.4f, 1.0f }, { 0.2f, 0.9f, 1.0f, 0.0f });
        particle.SetEmitRate(0.0f);
        particle.Burst(36);
        scene.DestroySelf(0.8f);
        return;
    }

    m_swirlTime += fbzz::Time::deltaTime;
    const fbzz::math::Vector3 direction = transform.DirectionTo(*m_player);
    fbzz::math::Vector3 tangent = fbzz::math::Vector3::Cross(fbzz::math::Vector3::UP, direction);
    if (tangent.LengthSq() > fbzz::math::EPSILON) tangent = tangent.Normalized();

    const float proximity = 1.0f - std::clamp(distance / assistedRadius, 0.0f, 1.0f);
    const float radialSpeed = pullSpeed * (1.55f + proximity * 2.75f);
    const float swirlSpeed = pullSpeed * (0.75f + (1.0f - proximity) * 0.65f);
    const float pulse = 0.72f + 0.28f * std::sin(m_swirlTime * 8.0f + distance * 1.7f);
    const fbzz::math::Vector3 velocity = direction * radialSpeed
        + tangent * (swirlSpeed * pulse);
    const float maxStep = std::max(0.0f, distance - collectRadius * 0.65f);
    const float step = std::min(maxStep, velocity.Length() * fbzz::Time::deltaTime);
    if (velocity.LengthSq() > fbzz::math::EPSILON)
        transform.Translate(velocity.Normalized() * step);
    debug.DrawLine(transform.worldPosition,
                   transform.worldPosition + direction * std::min(distance, 1.5f),
                   { 0.2f, 0.8f, 1.0f, 0.5f });
}

} // namespace particlegame
