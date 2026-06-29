// FBZZ Engine
// HitBloodEffectComponent.hpp | sandbox
// 被弾時だけ赤い血しぶき Particle を発生させるスクリプト
#pragma once

#include <Engine/Scene/Script.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class HitBloodEffectComponent : public Script {
    FBZZ_SCRIPT(HitBloodEffectComponent)

public:
    FBZZ_GROUP("Particle")
    FBZZ_FIELD(Vector3, localOffset,  Vector3(0.0f, 1.05f, 0.15f), "")
    FBZZ_FIELD(Vector3, emitVelocity, Vector3(0.0f, 1.2f, 0.8f),   "")
    FBZZ_FIELD(Vector3, gravity,      Vector3(0.0f, -3.0f, 0.0f),  "")
    FBZZ_FIELD(Vector4, colorStart,   Vector4(0.65f, 0.0f, 0.0f, 0.95f), "")
    FBZZ_FIELD(Vector4, colorEnd,     Vector4(0.20f, 0.0f, 0.0f, 0.0f),  "")
    FBZZ_FIELD_RANGE(float, sizeStart,      0.12f, "", 0.01f, 1.0f)
    FBZZ_FIELD_RANGE(float, sizeEnd,        0.02f, "", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, lifetime,       0.45f, "", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, velocitySpread, 1.25f, "", 0.0f, 8.0f)
    FBZZ_FIELD(int, burstCount, 18, "")

    void OnStart() override;
    void PlayBlood();

private:
    void ConfigureEmitter();
};

FBZZ_REFLECT(HitBloodEffectComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────
inline void HitBloodEffectComponent::OnStart()
{
    ConfigureEmitter();
    particle.Stop(/*clear=*/true);
}

inline void HitBloodEffectComponent::PlayBlood()
{
    ConfigureEmitter();
    particle.Play(/*restart=*/true);
    particle.Burst(std::max(1, burstCount));
}

inline void HitBloodEffectComponent::ConfigureEmitter()
{
    // WHY: 血は「被弾した結果」なのでループさせず、短いバーストだけを発生させる。
    particle.SetEnabled(true);
    particle.SetPlayback(/*loop=*/false, 0.08f, /*clearOnStop=*/false);
    particle.SetEmitPosition(localOffset);
    particle.SetEmitVelocity(emitVelocity);
    particle.SetVelocitySpread(velocitySpread);
    particle.SetGravity(gravity);
    particle.SetColor(colorStart, colorEnd);
    particle.SetSize(sizeStart, sizeEnd);
    particle.SetLifetime(lifetime);
    particle.SetEmitRate(0.0f);
    particle.SetMaxParticles(160);
    particle.SetSphereShape(0.08f);
    particle.SetBlendMode(ParticleBlendMode::Alpha);
    particle.SetSortMode(ParticleSortMode::BackToFront);
}

} // namespace sandbox
