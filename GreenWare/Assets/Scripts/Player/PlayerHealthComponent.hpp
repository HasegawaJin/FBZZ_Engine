/// @file    PlayerHealthComponent.hpp
/// @brief   プレイヤーの HP・被弾・死亡。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// WHY HP 制か:
/// 18.3「HP 制を採用する (数発耐える)。ミスが即死にならないため、5〜10 分の短い体験に合う」。
///
/// WHY 回避に無敵時間を持たせないか:
/// 11 章と 19 章がジャスト回避 (判定・無敵・報酬) を本バージョンから明示的に外している。
/// ここに置くのは被弾直後の短い無敵だけで、これは連続ヒットで一瞬に溶けるのを防ぐための
/// ものであり、回避の報酬ではない。混同すると 19 章の判断を無効化してしまう。
#pragma once

#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Data/PlayerTuning.hpp>
#include <Scripts/Game/ImpactFeedbackManagerComponent.hpp>
#include <Scripts/Player/PlayerControllerComponent.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
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
    FBZZ_FIELD_AUDIO(sfxHurt, "", "SFX Hurt")
    FBZZ_FIELD_AUDIO(sfxDeath, "", "SFX Death")
    // 被弾時の揺れ・振動・画面効果は ImpactFeedbackManagerComponent の Player Hurt が持つ。
    // 12 章の「強さは 1 箇所で持つ」方針を、被弾以外の出来事も含めた形へ広げたもの。

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
    // 未割り当てを 0 で返す。OnStart が名指しで報告するので、ここは落とさないだけでよい。
    return tuning ? tuning->maxHealth : 0;
}

inline float PlayerHealthComponent::Normalized() const
{
    const int max = MaxHealth();
    if (max <= 0) return 0.0f;
    return Clamp01(static_cast<float>(m_health) / static_cast<float>(max));
}

inline void PlayerHealthComponent::OnStart()
{
    // WHY ここだけ調整値を検算するか: 上限が 0 だと開始時点で «死んでいる» 扱いになり、
    //     TakeDamage は無条件に false を返す。画面には «何をされても HP が減らない»
    //     としか出ず、原因が被弾側にも敵側にも見えない。
    if (!tuning || tuning->maxHealth <= 0) {
        debug.LogError("PlayerHealthComponent has no usable PlayerTuning (Max Health must "
                       "be 1 or more). The player can never take damage.");
    }

    ResetHealth();
    se::EnsureSource(scene);
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
    m_invulnerable = tuning ? tuning->hitInvulnerable : 0.0f;

    // 揺れ・振動・画面の赤は ImpactFeedbackManagerComponent が配分を持つ。
    // ここは「被弾した」と「どれくらい深手か」だけを渡す。
    if (auto* feedback = ImpactFeedbackManagerComponent::Instance()) {
        // 残り体力が少ないほど強く返す。同じ 1 ダメージでも、後がない一撃の方が重い。
        const float remaining = Normalized();
        feedback->Play(FeedbackEvent::PlayerHurt, Lerp(1.0f, 0.55f, remaining));
    }

    if (!IsAlive()) {
        se::Play(audio, sfxDeath, se::kPlayerDeath);
        if (onDeath) onDeath();
        return true;
    }

    // WHY 被弾音だけカタログの既定を持たないか: 1 つ上で ImpactFeedbackManager へ
    //     PlayerHurt を渡しており、被弾の SE はあちらの配分表が鳴らす。ここにも既定を
    //     置くと、1 回の被弾で同じ音が 2 つ重なって音量だけが倍になる。
    //     Inspector で明示的に指定されたときだけ、重ねる音として鳴らす。
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
