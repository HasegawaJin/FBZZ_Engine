// FBZZ Engine
// HitBloodEffectComponent.hpp | sandbox
// 被弾時だけ赤い血しぶき Particle を発生させるスクリプト
#pragma once

#include <Engine/Scene/Script.hpp>
#include <algorithm>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class HitBloodEffectComponent : public Script {
    FBZZ_SCRIPT(HitBloodEffectComponent)

public:
    FBZZ_GROUP("Particle")
    FBZZ_FIELD(Vector3, localOffset, Vector3(0.0f, 1.05f, 0.15f), "Local Offset")
    FBZZ_FIELD(Vector3, emitVelocity, Vector3(0.0f, 1.2f, 0.8f), "Emit Velocity")
    FBZZ_FIELD(Vector3, gravity, Vector3(0.0f, -3.0f, 0.0f), "Gravity")
    FBZZ_FIELD(Vector4, colorStart, Vector4(0.65f, 0.0f, 0.0f, 0.95f), "Color Start")
    FBZZ_FIELD(Vector4, colorEnd, Vector4(0.20f, 0.0f, 0.0f, 0.0f), "Color End")
    FBZZ_FIELD_RANGE(float, sizeStart, 0.12f, "Size Start", 0.01f, 1.0f)
    FBZZ_FIELD_RANGE(float, sizeEnd, 0.02f, "Size End", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, lifetime, 0.45f, "Lifetime", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, velocitySpread, 1.25f, "Velocity Spread", 0.0f, 8.0f)
    FBZZ_FIELD(int, burstCount, 18, "Burst Count")

    void OnStart() override;
    void PlayBlood();

private:
    void ConfigureEmitter();
};

} // namespace sandbox

#include "HitBloodEffectComponent.generated.hpp"

#ifndef HitBloodEffectComponent_IMPL
#define HitBloodEffectComponent_IMPL

namespace sandbox {

void HitBloodEffectComponent::OnStart()
{
    ConfigureEmitter();
    particle.Stop(/*clear=*/true);
}

void HitBloodEffectComponent::PlayBlood()
{
    ConfigureEmitter();
    particle.Play(/*restart=*/true);
    particle.Burst(std::max(1, burstCount));
}

void HitBloodEffectComponent::ConfigureEmitter()
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
#endif
