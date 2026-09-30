/// @file    PlayerHealthComponent.hpp
/// @brief   プレイヤーの HP・被弾・死亡。
/// @author  Hasegawa Jin
/// @date    2026-08-19

#pragma once
#include <Scripts/Game/TimeManagerComponent.hpp>

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
#include <Scripts/Utils/WeaponSockets.hpp>
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
    /// @note PlayerComponent が必須 PlayerTuning を注入する。HP 値をこの Script に複製しない。
    fbzz::Asset<PlayerTuning> tuning{};

    FBZZ_GROUP("手応え")
    FBZZ_FIELD_AUDIO(sfxHurt, "", "SFX Hurt")
    FBZZ_FIELD_AUDIO(sfxDeath, "", "撃破の効果音")
    /// @note 被弾時の揺れ・振動・画面効果は ImpactFeedbackManagerComponent の Player Hurt が持つ。
    /// @note 12 章の「強さは 1 箇所で持つ」方針を、被弾以外の出来事も含めた形へ広げたもの。

    /// @note 無敵の «見せ方»。被弾直後の 0.6 秒 (PlayerTuning の hitInvulnerable) に掛かる。
    FBZZ_GROUP("Invulnerable Flash")
    FBZZ_FIELD(bool, flashOnInvulnerable, true, "閃光")
    FBZZ_FIELD_RANGE(float, flashHz, 9.0f, "Hz", 0.0f, 30.0f)
    FBZZ_TOOLTIP("明滅の速さ。遅いと «光っている» に見え、速すぎるとちらつく")
    FBZZ_FIELD_COLOR(flashColor, (Vector4{ 1.00f, 0.42f, 0.38f, 1.0f }), "Color")
    FBZZ_TOOLTIP("被弾の赤。極の赤 (＋) と紛れないよう、彩度を落とした肌色寄りにしてある")
    FBZZ_FIELD_RANGE(float, flashStrength, 3.2f, "強度", 0.0f, 20.0f)
    FBZZ_TOOLTIP("自発光の強さ。ブルームのしきい値 (4.0) より少し下 ─ "
                 "越えると画面が滲んで «攻撃を受けている» に見える")

    FBZZ_GROUP("デバッグ")
    FBZZ_OBSERVE(int, debugHealth, m_health, "HP")
    FBZZ_OBSERVE(int, debugFlashParts, static_cast<int>(m_flash.size()), "Flash Parts")

    [[nodiscard]] int   Current() const { return m_health; }
    /// @note Max() でなく MaxHealth(): 同名だと math::Max がクラス内から隠れる。
    [[nodiscard]] int   MaxHealth() const;
    [[nodiscard]] bool  IsAlive() const { return m_health > 0; }
    [[nodiscard]] bool  IsInvulnerable() const { return m_invulnerable > 0.0f; }
    /// @note UI のライフゲージ用。1 = 満タン / 0 = 死亡。
    [[nodiscard]] float Normalized() const;

    /// @note 敵の攻撃から呼ばれる唯一の入口。無敵中や死亡後は無視して false を返す。
    bool TakeDamage(int amount);

    /// @note «ここより下へは減らない» を 1 フレームぶん要求する。0 で解除。
    /// @note 無敵でなく下限にする (チュートリアル用): 無敵は被弾自体を無かったことにするが、
    /// @note 下限なら痛み・のけぞり・赤い縁は出したまま死亡だけを止められる。
    /// @note 下限は 1 を想定。崩し ×1.25 / Flux 窓 ×1.5 / 画面縁の鼓動という
    /// @note 残り HP 1 の演出をそのまま使い、教えている間も緊張感を落とさない。
    /// @note RequestSuspend と同じ「毎フレーム要求」形式。掛けっぱなしにできると
    /// @note 外し忘れたまま死なないゲームになる。
    void RequestDamageFloor(int minHealth)
    { m_requestedFloor = m_requestedFloor > minHealth ? m_requestedFloor : minHealth; }
    void Heal(int amount);
    void ResetHealth();

    /// @note 死亡通知。Wave 管理やリザルト画面が購読する。
    /// @note コールバックにして遷移を分離: 直接呼ぶと体力管理と進行管理が同居し、
    /// @note 未決のリトライ仕様変更のたびにこのファイルを触ることになる。
    std::function<void()> onDeath;

    void OnStart()  override;
    void OnUpdate() override;
    void SetController(PlayerControllerComponent* controller) { m_controllerOverride = controller; }

private:
    /// @note 明滅させる 1 スロット。元の自発光を控えて、無敵が明けたら必ず戻す。
    struct FlashSlot {
        EntityRef target;
        uint32_t  slot = 0;
        Vector3   baseColor{ 0.0f, 0.0f, 0.0f };
        float     baseScale = 0.0f;
    };

    /// @note 体を描いている部位を集める。DLL リロードでも名前ではなく EntityRef で持つ。
    void CollectFlashTargets(GameObject& object);
    /// @note 明滅を書く。無敵でないフレームは元の値へ戻して、書いたことを忘れる。
    void DriveFlash();

    int   m_health       = 0;
    float m_invulnerable = 0.0f;
    /// @note 今フレーム効いている体力の下限と、次フレームぶんの要求 (0 で無し)。
    int   m_damageFloor    = 0;
    int   m_requestedFloor = 0;
    PlayerControllerComponent* m_controllerOverride = nullptr;

    std::vector<FlashSlot> m_flash;
    /// @note 今フレーム自発光を上書きしているか。戻し忘れを 1 つの札で防ぐ。
    bool  m_flashing = false;
};

FBZZ_REFLECT(PlayerHealthComponent)


inline int PlayerHealthComponent::MaxHealth() const
{
    /// @note 未割り当てを 0 で返す。OnStart が名指しで報告するので、ここは落とさないだけでよい。
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
    /// @note 上限 0 は開始時点で «死んでいる» 扱いになり TakeDamage が常に false を返す。
    /// @note 画面には «HP が減らない» としか出ず原因が追えないため、ここで検算する。
    if (!tuning || tuning->maxHealth <= 0) {
        debug.LogError("PlayerHealthComponent has no usable PlayerTuning (Max Health must "
                       "be 1 or more). The player can never take damage.");
    }

    ResetHealth();
    se::EnsureSource(scene);

    /// @note 明滅させる先は «体を描いているもの» 全部 (武器含む。手だけ光ると剣が別物に見える)。
    /// @note 刀は Player の子でなくルート直下の実体 (SocketAttachment で手のボーンへ追従) のため、
    /// @note 部分木探索でなく名前で明示的に足す。
    /// @see WeaponRigComponent
    m_flash.clear();
    m_flashing = false;
    if (GameObject* self = scene.Self()) CollectFlashTargets(*self);
    const HandSide hands[] = { HandSide::Right };
    for (const HandSide hand : hands)
        if (GameObject* sword = scene.Find(SwordObjectName(hand), true))
            CollectFlashTargets(*sword);
}

inline void PlayerHealthComponent::CollectFlashTargets(GameObject& object)
{
    if (object.GetComponent<SkinnedMeshRenderer>() || object.GetComponent<MeshRenderer>()) {
        FlashSlot entry{ EntityRef{ object.GetID() }, 0u, Vector3::ZERO, 0.0f };
        const MaterialInstance instance = material.Instance(entry.target, entry.slot);
        if (instance.IsValid()) {
            /// @note 元の値が読めない材質もある (自発光を持たない)。0 から始めて 0 へ戻す。
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
    /// @note 書いていないなら戻すものも無い
    if (!want && !m_flashing) return;

    /// @note 明滅は矩形波。滑らかに上下させると «光っている» になり、点滅として読めない。
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
}

inline void PlayerHealthComponent::OnUpdate()
{
    if (m_invulnerable > 0.0f)
        m_invulnerable = fbzz::math::Max(0.0f, m_invulnerable - TimeManagerComponent::PlayerDeltaTime());
    /// @note 下限は «押し続けている間だけ» 効く。要求を受け取って空にするのはここ 1 か所
    /// @note (RequestSuspend と同じ形。掛けっぱなしにできると外し忘れが死なないゲームになる)。
    m_damageFloor    = m_requestedFloor;
    m_requestedFloor = 0;
    DriveFlash();
}

inline bool PlayerHealthComponent::TakeDamage(int amount)
{
    if (amount <= 0 || !IsAlive() || IsInvulnerable()) return false;

    m_health = m_health > amount ? m_health - amount : 0;
    /// @note 下限が要求されているあいだは、そこで止める (`RequestDamageFloor` 参照)。
    if (m_damageFloor > 0 && m_health < m_damageFloor) m_health = m_damageFloor;
    m_invulnerable = tuning ? tuning->hitInvulnerable : 0.0f;

    /// @note 揺れ・振動・画面の赤は ImpactFeedbackManagerComponent が配分を持つ。
    /// @note ここは「被弾した」と「どれくらい深手か」だけを渡す。
    if (auto* feedback = ImpactFeedbackManagerComponent::Instance()) {
        /// @note 残り体力が少ないほど強く返す。同じ 1 ダメージでも、後がない一撃の方が重い。
        const float remaining = Normalized();
        feedback->Play(FeedbackEvent::PlayerHurt, Lerp(1.0f, 0.55f, remaining));
    }

    if (!IsAlive()) {
        se::Play(audio, sfxDeath, se::kPlayerDeath);
        if (onDeath) onDeath();
        return true;
    }

    /// @note 被弾音は既定を持たない: ImpactFeedbackManager の配分表が PlayerHurt で既に鳴らすため、
    /// @note 既定を足すと同じ音が二重に重なり音量だけ倍になる。指定時だけ重ねて鳴らす。
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
}

} /// @note namespace sandbox
