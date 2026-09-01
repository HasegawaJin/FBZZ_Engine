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
///
/// WHY 無敵を «見せる» か:
/// 弾かれた一撃は画面に何も残さない ─ HP が減らないので、プレイヤーには
/// «当たったのに減らなかった» のか «避けられていた» のかが区別できない。
/// 体が点滅していれば、その 0.6 秒は «今は当たらない» と読めて、踏み込む判断ができる。
/// 明滅は自発光でやる (半透明にすると深度書き込みが外れて体の裏が透ける)。
#pragma once

#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Data/PlayerTuning.hpp>
#include <Scripts/Game/ImpactFeedbackManagerComponent.hpp>
#include <Scripts/Player/PlayerControllerComponent.hpp>
#include <Scripts/Utils/GlowMaterial.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

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

    // 無敵の «見せ方»。被弾直後の 0.6 秒 (PlayerTuning の hitInvulnerable) に掛かる。
    FBZZ_GROUP("Invulnerable Flash")
    FBZZ_FIELD(bool, flashOnInvulnerable, true, "Flash")
    FBZZ_FIELD_RANGE(float, flashHz, 9.0f, "Hz", 0.0f, 30.0f)
    FBZZ_TOOLTIP("明滅の速さ。遅いと «光っている» に見え、速すぎるとちらつく")
    FBZZ_FIELD_COLOR(flashColor, (Vector4{ 1.00f, 0.42f, 0.38f, 1.0f }), "Color")
    FBZZ_TOOLTIP("被弾の赤。極の赤 (＋) と紛れないよう、彩度を落とした肌色寄りにしてある")
    FBZZ_FIELD_RANGE(float, flashStrength, 3.2f, "Strength", 0.0f, 20.0f)
    FBZZ_TOOLTIP("自発光の強さ。ブルームのしきい値 (4.0) より少し下 ─ "
                 "越えると画面が滲んで «攻撃を受けている» に見える")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugHealth, 0, "Health")
    FBZZ_FIELD_READ_ONLY(int, debugFlashParts, 0, "Flash Parts")

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
    /// 明滅させる 1 スロット。元の自発光を控えて、無敵が明けたら必ず戻す。
    struct FlashSlot {
        EntityRef target;
        uint32_t  slot = 0;
        Vector3   baseColor{ 0.0f, 0.0f, 0.0f };
        float     baseScale = 0.0f;
    };

    /// 体を描いている部位を集める。DLL リロードでも名前ではなく EntityRef で持つ。
    void CollectFlashTargets(GameObject& object);
    /// 明滅を書く。無敵でないフレームは元の値へ戻して、書いたことを忘れる。
    void DriveFlash();

    int   m_health       = 0;
    float m_invulnerable = 0.0f;
    PlayerControllerComponent* m_controllerOverride = nullptr;

    std::vector<FlashSlot> m_flash;
    /// 今フレーム自発光を上書きしているか。戻し忘れを 1 つの札で防ぐ。
    bool  m_flashing = false;
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

    // 明滅させる先は «体を描いているもの» 全部。武器も含めるのは、
    // 手だけ光らないと «剣は別のもの» に見えるため。
    m_flash.clear();
    m_flashing = false;
    if (GameObject* self = scene.Self()) CollectFlashTargets(*self);
    debugFlashParts = static_cast<int>(m_flash.size());
}

inline void PlayerHealthComponent::CollectFlashTargets(GameObject& object)
{
    if (object.GetComponent<SkinnedMeshRenderer>() || object.GetComponent<MeshRenderer>()) {
        FlashSlot entry{ EntityRef{ object.GetID() }, 0u, Vector3::ZERO, 0.0f };
        const MaterialInstance instance = material.Instance(entry.target, entry.slot);
        if (instance.IsValid()) {
            // 元の値が読めない材質もある (自発光を持たない)。0 から始めて 0 へ戻す。
            (void)instance.TryGetVector3(kEmissiveColorId, entry.baseColor);
            (void)instance.TryGetFloat(kEmissiveScaleId, entry.baseScale);
            m_flash.push_back(entry);
        }
    }

    for (int i = 0; i < object.GetChildCount(); ++i)
        if (GameObject* child = object.GetChild(i)) CollectFlashTargets(*child);
}

inline void PlayerHealthComponent::DriveFlash()
{
    const bool want = flashOnInvulnerable && IsInvulnerable() && IsAlive();
    if (!want && !m_flashing) return;   // 書いていないなら戻すものも無い

    // 明滅は矩形波。滑らかに上下させると «光っている» になり、点滅として読めない。
    const float wave = flashHz > 0.0f
        ? (std::sin(Time::time * flashHz * TWO_PI) >= 0.0f ? 1.0f : 0.0f) : 1.0f;

    for (const FlashSlot& entry : m_flash) {
        const MaterialInstance instance = material.Instance(entry.target, entry.slot);
        if (!instance.IsValid()) continue;
        if (want) {
            instance.SetVector3(kEmissiveColorId,
                                Vector3{ flashColor.x, flashColor.y, flashColor.z });
            instance.SetFloat(kEmissiveScaleId, Max(flashStrength, 0.0f) * wave);
        } else {
            instance.SetVector3(kEmissiveColorId, entry.baseColor);
            instance.SetFloat(kEmissiveScaleId, entry.baseScale);
        }
    }
    m_flashing = want;
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
    DriveFlash();
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
