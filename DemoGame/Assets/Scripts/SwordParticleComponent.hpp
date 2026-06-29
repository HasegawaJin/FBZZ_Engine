// FBZZ Engine
// SwordParticleComponent.hpp | sandbox
// 剣の swing 区間に連動してパーティクルエフェクトを再生するスクリプト。
#pragma once

#include <Engine/Scene/Script.hpp>
#include "GameVocab.hpp"

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class SwordParticleComponent : public Script {
    FBZZ_SCRIPT(SwordParticleComponent)

public:
    FBZZ_GROUP("References")
    // 旧: FBZZ_FIELD(std::string, playerTag, ...)。新: 型安全参照 (未アサイン時はタグ探索)。
    FBZZ_REF(GameObject, player, "Player")
    FBZZ_FIELD(std::string, playerTag, "Player", "Player Tag")

    FBZZ_GROUP("Particle")
    FBZZ_FIELD(Vector4, colorStart, Vector4(1.0f, 0.70f, 0.15f, 1.0f), "")
    FBZZ_FIELD(Vector4, colorEnd,   Vector4(1.0f, 0.10f, 0.00f, 0.0f), "")
    FBZZ_FIELD_RANGE(float, sizeStart, 0.12f, "", 0.01f, 1.0f)
    FBZZ_FIELD_RANGE(float, sizeEnd,   0.02f, "", 0.00f, 1.0f)
    FBZZ_FIELD_RANGE(float, emitRate,  80.0f, "", 1.0f, 500.0f)
    FBZZ_FIELD(std::string, texturePath, "", "")

    void OnStart() override;
    void OnUpdate() override;

private:
    [[nodiscard]] GameObject* ResolvePlayer();
    [[nodiscard]] bool IsSwingWindow() const;

    GameObject* m_playerGO = nullptr;
    bool m_wasSwing = false;
};

FBZZ_REFLECT(SwordParticleComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────
inline void SwordParticleComponent::OnStart()
{
    m_playerGO = ResolvePlayer();

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

inline void SwordParticleComponent::OnUpdate()
{
    if (!m_playerGO || !m_playerGO->IsValid())
        m_playerGO = ResolvePlayer();

    const bool inSwing = IsSwingWindow();
    if (inSwing && !m_wasSwing)
        particle.Play(/*restart=*/true);
    else if (!inSwing && m_wasSwing)
        particle.Stop(/*clear=*/false);

    m_wasSwing = inSwing;
}

inline GameObject* SwordParticleComponent::ResolvePlayer()
{
    // 1) Inspector でアサインされた参照を最優先。2) なければタグで探索。
    if (GameObject* assigned = player.object())
        return assigned;
    return playerTag.empty() ? nullptr : scene.FindWithTag(playerTag);
}

inline bool SwordParticleComponent::IsSwingWindow() const
{
    // 剣を振る通常コンボ State の振り区間 (0.18〜0.78) のみで再生する。共有語彙ヘルパーに集約。
    constexpr float SWING_START = 0.18f;
    constexpr float SWING_END   = 0.78f;
    return m_playerGO && IsAttackSwing(animator, m_playerGO, SWING_START, SWING_END);
}

} // namespace sandbox
