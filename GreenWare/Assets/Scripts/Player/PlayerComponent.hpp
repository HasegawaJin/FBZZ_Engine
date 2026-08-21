// FBZZ Engine
// PlayerComponent.hpp | sandbox
// Player の実行入口。移動・エイム・体力・極性銃を 1 コンポーネントへ束ねる。
//
// WHY 1 コンポーネントにするか:
//   各責務は PlayerControllerComponent / PlayerAimComponent / PlayerHealthComponent /
//   PolarityGunComponent に分割したまま、シーンへは PlayerComponent だけをアタッチする。
//   これにより Player の構成漏れを減らし、モジュール単位の実装・レビュー・差し替えやすさを残す。
//
// WHY fzdata を必須にするか:
//   移動・HP・銃の成立値をコード側の既定値へフォールバックさせると、共有アセットを
//   編集しても一部だけ別の値で動く。Play 開始時に 2 枚の参照を検査し、未設定なら動作を止める。
#pragma once

#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Data/PlayerTuning.hpp>
#include <Scripts/Data/PolarityTuning.hpp>
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Player/PlayerControllerComponent.hpp>
#include <Scripts/Player/PlayerHealthComponent.hpp>
#include <Scripts/Player/PolarityGunComponent.hpp>
#include <Scripts/Player/WeaponRigComponent.hpp>
#include <functional>
#include <utility>

using namespace fbzz::scene;

namespace sandbox {

class PlayerComponent : public Script {
    FBZZ_SCRIPT(PlayerComponent)

    FBZZ_REQUIRE_COMPONENT(CharacterControllerComponent, RigidBodyComponent)
    FBZZ_OPTIONAL_COMPONENT(AnimatorComponent, IKSolverComponent, AudioSourceComponent)

public:
    // これらは Player の成立条件であり、PlayerComponent 以外には公開しない。
    FBZZ_REQUIRED_ASSET(PlayerTuning, tuning, "Player Tuning")
    FBZZ_REQUIRED_ASSET(PolarityTuning, polarityTuning, "Polarity Tuning")

    // 外部システムは個別モジュールを探さず、Player の公開 API だけを使う。
    [[nodiscard]] int Current() const { return m_health.Current(); }
    [[nodiscard]] int MaxHealth() const { return m_health.MaxHealth(); }
    [[nodiscard]] bool IsAlive() const { return m_health.IsAlive(); }
    [[nodiscard]] float NormalizedHealth() const { return m_health.Normalized(); }
    [[nodiscard]] bool TakeDamage(int amount) { return m_health.TakeDamage(amount); }
    void Heal(int amount) { m_health.Heal(amount); }
    void ResetHealth() { m_health.ResetHealth(); }
    [[nodiscard]] float ChargeOf(Polarity polarity) const { return m_gun.ChargeOf(polarity); }
    [[nodiscard]] bool CanFire(Polarity polarity) const { return m_gun.CanFire(polarity); }
    void SetOnDeath(std::function<void()> callback) { m_health.onDeath = std::move(callback); }

    // 銃を構えているか。UI と発砲判定が「今撃てるか」を知るための唯一の窓口。
    [[nodiscard]] bool AreWeaponsDrawn() const { return m_weaponRig.IsDrawn(); }

    void OnStart() override;
    void OnUpdate() override;
    void OnFixedUpdate() override;
    // 内部モジュールのギズモは自動では呼ばれないため、ここから中継する。
    void OnDrawGizmos() override { m_weaponRig.OnDrawGizmos(); }

    void PlayFireAnimation(bool rightHand) { m_controller.PlayFireAnimation(rightHand); }
    void PlayHitAnimation() { m_controller.PlayHitAnimation(); }

private:
    // 内部 Script は通常の ScriptSystem から呼ばれないため、親と同じ実行コンテキストを渡す。
    void BindModules();
    [[nodiscard]] bool HasRequiredAssets() const;

    PlayerControllerComponent m_controller;
    // 抜く / 収める はコントローラーから独立させる。入力の受け付け条件を触っても
    // 銃の付け替えには波及しない (逆も同じ)。
    WeaponRigComponent       m_weaponRig;
    PlayerAimComponent       m_aim;
    PlayerHealthComponent    m_health;
    PolarityGunComponent     m_gun;
};

// PlayerComponent 自身の fzdata 参照と、責務別モジュールの Inspector 項目を同じカードへ並べる。
// モジュール側の tuning 参照は反映せず、共有アセットのスロットを二重表示しない。
inline void PlayerComponent::Reflect(::fbzz::scene::IReflector& r_)
{
    r_.BeginField("tuning", "Player Tuning");
    r_.Field("Player Tuning", tuning.ref);
    r_.BeginField("polarityTuning", "Polarity Tuning");
    r_.Field("Polarity Tuning", polarityTuning.ref);
    m_controller.Reflect(r_);
    m_weaponRig.Reflect(r_);
    m_aim.Reflect(r_);
    m_health.Reflect(r_);
    m_gun.Reflect(r_);
}

inline void PlayerComponent::BindModules()
{
    m_controller.AdoptContext(*this);
    m_weaponRig.AdoptContext(*this);
    m_aim.AdoptContext(*this);
    m_health.AdoptContext(*this);
    m_gun.AdoptContext(*this);

    m_controller.tuning.ref = tuning.ref;
    m_health.tuning.ref = tuning.ref;
    m_gun.tuning.ref = polarityTuning.ref;
    m_controller.SetAimComponent(&m_aim);
    m_controller.SetWeaponRig(&m_weaponRig);
    m_health.SetController(&m_controller);
    m_gun.SetAimComponent(&m_aim);
    m_gun.SetController(&m_controller);
}

inline bool PlayerComponent::HasRequiredAssets() const
{
    if (tuning && polarityTuning)
        return true;
    debug.LogError(
        "PlayerComponent requires PlayerTuning and PolarityTuning .fzdata assets. "
        "Assign both assets before entering Play mode.");
    return false;
}

inline void PlayerComponent::OnStart()
{
    BindModules();
    if (!HasRequiredAssets()) {
        enabled = false;
        return;
    }

    // エイムを先に更新し、同じフレームの移動アニメーションと発射判定が同じ対象を見る。
    m_controller.OnStart();
    // 銃の追従設定はコントローラーの後。銃が腰へスナップしてから最初の描画が走る。
    m_weaponRig.OnStart();
    m_aim.OnStart();
    m_health.OnStart();
    m_gun.OnStart();
}

inline void PlayerComponent::OnUpdate()
{
    if (!enabled)
        return;
    // WHY: 内部モジュールも Script の一種だが、直接呼び出すとモジュール内の
    //      空 Ref / 無効な EntityRef が親 PlayerComponent の例外として扱われる。
    //      各モジュールを既存の ExecuteCallback 境界へ通し、問題の責務だけを停止して
    //      Console には実際の発生元の型名を残す。
    const auto updateModule = [](Script& module) {
        if (module.enabled)
            module.ExecuteCallback(&Script::OnUpdate, module.GetTypeName());
    };

    updateModule(m_aim);
    updateModule(m_controller);
    // コントローラーが同じフレームで受けた抜く / 収める要求を、その場で進める。
    // 先に回すと要求が 1 フレーム寝てしまい、押した感触が鈍る。
    updateModule(m_weaponRig);
    updateModule(m_health);
    updateModule(m_gun);
}

inline void PlayerComponent::OnFixedUpdate()
{
    if (enabled && m_controller.enabled)
        m_controller.ExecuteCallback(&Script::OnFixedUpdate, m_controller.GetTypeName());
}

} // namespace sandbox
