// FBZZ Engine
// PlayerHealthComponent.hpp | sandbox
// プレイヤーの HP・被弾・死亡。
//
// WHY HP 制か:
//   18.3「HP 制を採用する (数発耐える)。ミスが即死にならないため、5〜10 分の短い体験に合う」。
//
// WHY 回避に無敵時間を持たせないか:
//   11 章と 19 章がジャスト回避 (判定・無敵・報酬) を本バージョンから明示的に外している。
//   ここに置くのは被弾直後の短い無敵だけで、これは連続ヒットで一瞬に溶けるのを防ぐための
//   ものであり、回避の報酬ではない。混同すると 19 章の判断を無効化してしまう。
#pragma once

#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Data/PlayerTuning.hpp>
#include <Scripts/Camera/TpsCameraComponent.hpp>
#include <Scripts/Player/PlayerControllerComponent.hpp>
#include <functional>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class PlayerHealthComponent : public Script {
    FBZZ_SCRIPT(PlayerHealthComponent)

    FBZZ_OPTIONAL_COMPONENT(AudioSourceComponent)

public:
    // PlayerComponent が必須 PlayerTuning を注入する。HP 値をこの Script に複製しない。
    fbzz::Asset<PlayerTuning> tuning{};

    FBZZ_GROUP("Feedback")
    FBZZ_FIELD_FILE(sfxHurt, "", "SFX Hurt", ".wav,.ogg")
    FBZZ_FIELD_FILE(sfxDeath, "", "SFX Death", ".wav,.ogg")
    // 被弾時のカメラシェイク。12 章の演出方針に合わせ、強さは 1 箇所で持つ。
    FBZZ_FIELD_RANGE(float, hurtShakeAmplitude, 0.25f, "Hurt Shake", 0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, hurtShakeDuration,  0.25f, "Hurt Shake Duration", 0.0f, 2.0f)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugHealth, 0, "Health")

    [[nodiscard]] int   Current() const { return m_health; }
    // WHY Max() にしないか: math::Max を同じクラス内から呼べなくなる (名前が隠れる)。
    [[nodiscard]] int   MaxHealth() const;
    [[nodiscard]] bool  IsAlive() const { return m_health > 0; }
    [[nodiscard]] bool  IsInvulnerable() const { return m_invulnerable > 0.0f; }
    // UI のライフゲージ用。1 = 満タン / 0 = 死亡。
    [[nodiscard]] float Normalized() const;

    // 敵の攻撃から呼ばれる唯一の入口。無敵中や死亡後は無視して false を返す。
    bool TakeDamage(int amount);
    void Heal(int amount);
    void ResetHealth();

    // 死亡通知。Wave 管理やリザルト画面が購読する。
    // WHY コールバックにするか: ここから直接シーン遷移を呼ぶと、体力の管理と
    //     ゲーム進行の管理が 1 つのスクリプトに同居し、リトライ仕様 (18.3 で未決) を
    //     変えるたびにこのファイルを触ることになる。
    std::function<void()> onDeath;

    void OnStart()  override;
    void OnUpdate() override;
    void SetController(PlayerControllerComponent* controller) { m_controllerOverride = controller; }

private:
    int   m_health       = 0;
    float m_invulnerable = 0.0f;
    PlayerControllerComponent* m_controllerOverride = nullptr;
};

FBZZ_REFLECT(PlayerHealthComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline int PlayerHealthComponent::MaxHealth() const
{
    return tuning->maxHealth;
}

inline float PlayerHealthComponent::Normalized() const
{
    const int max = MaxHealth();
    if (max <= 0) return 0.0f;
    return Clamp01(static_cast<float>(m_health) / static_cast<float>(max));
}

inline void PlayerHealthComponent::OnStart()
{
    ResetHealth();
}

inline void PlayerHealthComponent::ResetHealth()
{
    m_health       = MaxHealth();
    m_invulnerable = 0.0f;
    debugHealth    = m_health;
}

inline void PlayerHealthComponent::OnUpdate()
{
    if (m_invulnerable > 0.0f)
        m_invulnerable = fbzz::math::Max(0.0f, m_invulnerable - Time::deltaTime);
}

inline bool PlayerHealthComponent::TakeDamage(int amount)
{
    if (amount <= 0 || !IsAlive() || IsInvulnerable()) return false;

    m_health = m_health > amount ? m_health - amount : 0;
    debugHealth = m_health;
    m_invulnerable = tuning->hitInvulnerable;

    if (hurtShakeAmplitude > 0.0f) {
        if (GameObject* camera = scene.GetMainCameraObject()) {
            if (auto* tps = scene.GetScript<TpsCameraComponent>(camera))
                tps->StartShake(hurtShakeAmplitude, hurtShakeDuration);
        }
    }

    if (!IsAlive()) {
        if (!sfxDeath.empty()) audio.PlayOneShot(sfxDeath);
        if (onDeath) onDeath();
        return true;
    }

    if (!sfxHurt.empty()) audio.PlayOneShot(sfxHurt);
    if (auto* controller = m_controllerOverride
        ? m_controllerOverride : scene.GetScript<PlayerControllerComponent>())
        controller->PlayHitAnimation();
    return true;
}

inline void PlayerHealthComponent::Heal(int amount)
{
    if (amount <= 0 || !IsAlive()) return;
    const int max = MaxHealth();
    m_health = (m_health + amount > max) ? max : m_health + amount;
    debugHealth = m_health;
}

} // namespace sandbox
