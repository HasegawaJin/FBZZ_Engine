// FBZZ Engine
// SwordParticleComponent.hpp | sandbox
// 剣の swing 区間に連動してパーティクルエフェクトを再生するスクリプト。
//
// セットアップ手順:
//   1. Player_Sword (または任意の子 GameObject) に ParticleEmitter コンポーネントを追加する。
//   2. 同 GameObject にこのスクリプトをアタッチする。
//   3. Inspector で playerTag に Player を持つ GameObject のタグを設定する (デフォルト: "Player")。
//   4. 必要に応じて Texture Path に Assets/Textures/Particles/ 以下の .fztex パスを設定する。
//
// 動作:
//   PlayerControllerComponent と同じ正規化時間ウィンドウ [0.18, 0.78] を参照し、
//   コンボ 3 段 (Slash_01/02/03) および CrouchSlash の振り区間のみ Emitter を再生する。
//   swing 開始で Play(restart)、終了で Stop(clear=false) して既存パーティクルは自然消滅させる。
#pragma once

#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class SwordParticleComponent : public Script {
    FBZZ_SCRIPT(SwordParticleComponent)

public:
    FBZZ_GROUP("References")
    // Player タグを持つ GameObject の AnimatorComponent を参照してアタック状態を判定する。
    FBZZ_FIELD(std::string, playerTag, "Player", "Player Tag")

    FBZZ_GROUP("Particle")
    // 加算合成 (Additive) のため、アルファは輝度として機能する。
    FBZZ_FIELD(Vector4, colorStart, Vector4(1.0f, 0.70f, 0.15f, 1.0f), "Color Start")
    FBZZ_FIELD(Vector4, colorEnd,   Vector4(1.0f, 0.10f, 0.00f, 0.0f), "Color End")
    FBZZ_FIELD_RANGE(float, sizeStart, 0.12f, "Size Start", 0.01f, 1.0f)
    FBZZ_FIELD_RANGE(float, sizeEnd,   0.02f, "Size End",   0.00f, 1.0f)
    FBZZ_FIELD_RANGE(float, emitRate,  80.0f, "Emit Rate",  1.0f, 500.0f)
    // 空文字列のときはエンジンデフォルトのホワイトパーティクルを使用する。
    FBZZ_FIELD(std::string, texturePath, "", "Texture Path")

    void OnStart() override;
    void OnUpdate() override;

private:
    // Player の AnimatorComponent を取得して swing 区間かどうかを判定する。
    // WHY: 固定秒数ではクリップ長ごとにタイミングがズレるため正規化時間で統一する。
    //      判定は PlayerControllerComponent::HandleCombat と同一ウィンドウ [0.18, 0.78]。
    [[nodiscard]] bool IsSwingWindow() const;

    GameObject* m_playerGO = nullptr; // Player への非所有参照 (OnUpdate で再取得を行う)
    bool        m_wasSwing = false;   // 前フレームの swing 状態 (エッジ検出用)
};

} // namespace sandbox

#include "SwordParticleComponent.generated.hpp"

#ifndef SwordParticleComponent_IMPL
#define SwordParticleComponent_IMPL

namespace sandbox {

void SwordParticleComponent::OnStart()
{
    m_playerGO = playerTag.empty() ? nullptr : scene.FindWithTag(playerTag);

    // ParticleEmitter を剣エフェクト向けにセットアップする。
    // WHY: 毎フレーム更新するとドライバが毎回 GPU バッファを再転送するため OnStart でまとめる。
    particle.SetColor(colorStart, colorEnd);
    particle.SetSize(sizeStart, sizeEnd);
    particle.SetEmitRate(emitRate);
    particle.SetShape(ParticleEmitterShape::Point);
    // WHY: Additive は火花・炎表現で色が加算されて自然に輝く。ソートは不要なため None に固定。
    particle.SetBlendMode(ParticleBlendMode::Additive);
    particle.SetSortMode(ParticleSortMode::None);
    if (!texturePath.empty())
        particle.SetTexture(texturePath);

    particle.Stop(/*clear=*/true);
    m_wasSwing = false;
}

void SwordParticleComponent::OnUpdate()
{
    // Player が未取得または無効になった場合は毎フレーム再検索する。
    if (!m_playerGO || !m_playerGO->IsValid())
        m_playerGO = playerTag.empty() ? nullptr : scene.FindWithTag(playerTag);

    const bool inSwing = IsSwingWindow();

    // エッジ検出: swing 開始時に Emitter を再生、終了時は新規発生だけ止め既存は自然消滅させる。
    if (inSwing && !m_wasSwing)
        particle.Play(/*restart=*/true);
    else if (!inSwing && m_wasSwing)
        particle.Stop(/*clear=*/false);

    m_wasSwing = inSwing;
}

bool SwordParticleComponent::IsSwingWindow() const
{
    if (!m_playerGO) return false;
    auto* anim = m_playerGO->GetComponent<AnimatorComponent>();
    if (!anim) return false;

    // コンボ 3 段と CrouchSlash を攻撃状態として扱う。
    const bool isAttacking =
        anim->IsInState("Slash_01")    ||
        anim->IsInState("Slash_02")    ||
        anim->IsInState("Slash_03")    ||
        anim->IsInState("CrouchSlash");
    if (!isAttacking) return false;

    // WHY: アニメーションクリップ前半は剣が鞘から出る動作のため、実際の振り動作区間のみ発光させる。
    constexpr float SWING_START = 0.18f;
    constexpr float SWING_END   = 0.78f;
    const float t = anim->GetNormalizedTime();
    return t >= SWING_START && t <= SWING_END;
}

} // namespace sandbox
#endif
