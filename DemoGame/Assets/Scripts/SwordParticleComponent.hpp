// FBZZ Engine
// SwordParticleComponent.hpp | sandbox
// 剣の swing 区間に連動してパーティクルエフェクトを再生するスクリプト。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class SwordParticleComponent : public Script {
    FBZZ_SCRIPT(SwordParticleComponent)

public:
    FBZZ_GROUP("References")
    FBZZ_FIELD(std::string, playerTag, "Player", "Player Tag")

    FBZZ_GROUP("Particle")
    FBZZ_FIELD(Vector4, colorStart, Vector4(1.0f, 0.70f, 0.15f, 1.0f), "Color Start")
    FBZZ_FIELD(Vector4, colorEnd,   Vector4(1.0f, 0.10f, 0.00f, 0.0f), "Color End")
    FBZZ_FIELD_RANGE(float, sizeStart, 0.12f, "Size Start", 0.01f, 1.0f)
    FBZZ_FIELD_RANGE(float, sizeEnd,   0.02f, "Size End",   0.00f, 1.0f)
    FBZZ_FIELD_RANGE(float, emitRate,  80.0f, "Emit Rate",  1.0f, 500.0f)
    FBZZ_FIELD(std::string, texturePath, "", "Texture Path")

    void OnStart() override;
    void OnUpdate() override;

private:
    [[nodiscard]] bool IsSwingWindow() const;

    GameObject* m_playerGO = nullptr;
    bool m_wasSwing = false;
};

} // namespace sandbox

#include "SwordParticleComponent.generated.hpp"

#ifndef SwordParticleComponent_IMPL
#define SwordParticleComponent_IMPL

namespace sandbox {

void SwordParticleComponent::OnStart()
{
    m_playerGO = playerTag.empty() ? nullptr : scene.FindWithTag(playerTag);

    // WHY: 毎フレーム更新すると GPU バッファ再転送が増えるため、静的な見た目は開始時にまとめて設定する。
    particle.SetColor(colorStart, colorEnd);
    particle.SetSize(sizeStart, sizeEnd);
    particle.SetEmitRate(emitRate);
    particle.SetShape(ParticleEmitterShape::Point);
    particle.SetBlendMode(ParticleBlendMode::Additive);
    particle.SetSortMode(ParticleSortMode::None);
    if (!texturePath.empty())
        particle.SetTexture(texturePath);
    particle.Stop(/*clear=*/true);
    m_wasSwing = false;
}

void SwordParticleComponent::OnUpdate()
{
    if (!m_playerGO || !m_playerGO->IsValid())
        m_playerGO = playerTag.empty() ? nullptr : scene.FindWithTag(playerTag);

    const bool inSwing = IsSwingWindow();
    if (inSwing && !m_wasSwing)
        particle.Play(/*restart=*/true);
    else if (!inSwing && m_wasSwing)
        particle.Stop(/*clear=*/false);

    m_wasSwing = inSwing;
}

bool SwordParticleComponent::IsSwingWindow() const
{
    if (!m_playerGO) return false;

    const bool isAttacking =
        animator.IsInState(m_playerGO, "Slash_01") ||
        animator.IsInState(m_playerGO, "Slash_02") ||
        animator.IsInState(m_playerGO, "Slash_03") ||
        animator.IsInState(m_playerGO, "CrouchSlash");
    if (!isAttacking) return false;

    constexpr float SWING_START = 0.18f;
    constexpr float SWING_END   = 0.78f;
    const float t = animator.GetNormalizedTime(m_playerGO);
    return t >= SWING_START && t <= SWING_END;
}

} // namespace sandbox
#endif
