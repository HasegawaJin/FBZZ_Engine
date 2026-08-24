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
#include <Scripts/Combat/IDamageable.hpp>
#include <Scripts/Data/PlayerTuning.hpp>
#include <Scripts/Data/PolarityTuning.hpp>
#include <Scripts/Player/AimMarkerComponent.hpp>
#include <Scripts/Player/BeamScorchComponent.hpp>
#include <Scripts/Player/CrosshairComponent.hpp>
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Player/PlayerControllerComponent.hpp>
#include <Scripts/Player/PlayerHeadLookComponent.hpp>
#include <Scripts/Player/PlayerHealthComponent.hpp>
#include <Scripts/Player/PolarityGunComponent.hpp>
#include <Scripts/Player/WeaponRigComponent.hpp>
#include <functional>
#include <utility>

using namespace fbzz::scene;

namespace sandbox {

class PlayerComponent : public Script, public IDamageable {
    FBZZ_SCRIPT_DERIVED(PlayerComponent, Script, IDamageable)

    FBZZ_REQUIRE_COMPONENT(CharacterControllerComponent, RigidBodyComponent)
    FBZZ_OPTIONAL_COMPONENT(AnimatorComponent, IKSolverComponent, AudioSourceComponent)

public:
    // これらは Player の成立条件であり、PlayerComponent 以外には公開しない。
    FBZZ_REQUIRED_ASSET(PlayerTuning, tuning, "Player Tuning")
    FBZZ_REQUIRED_ASSET(PolarityTuning, polarityTuning, "Polarity Tuning")

    // ── IDamageable ─────────────────────────────────────────────────────────
    // 敵も樽もプレイヤーも同じ入口で殴れるようにする。CombatManager が
    // 「誰に何点入れたか」を 1 本の経路で記録できるのはこれによる。
    bool ApplyDamage(int amount) override { return m_health.TakeDamage(amount); }
    [[nodiscard]] int  CurrentHealth() const override { return m_health.Current(); }
    [[nodiscard]] int  MaxHealth()     const override { return m_health.MaxHealth(); }
    [[nodiscard]] bool IsAlive()       const override { return m_health.IsAlive(); }

    // 外部システムは個別モジュールを探さず、Player の公開 API だけを使う。
    [[nodiscard]] int Current() const { return m_health.Current(); }
    [[nodiscard]] float NormalizedHealth() const { return m_health.Normalized(); }
    [[nodiscard]] bool TakeDamage(int amount) { return m_health.TakeDamage(amount); }
    void Heal(int amount) { m_health.Heal(amount); }
    void ResetHealth() { m_health.ResetHealth(); }
    // 6.3 の照射バッテリー。左右で独立した 2 本を HUD がそのまま 2 本のゲージに出す。
    [[nodiscard]] float BatteryOf(Polarity polarity) const { return m_gun.BatteryOf(polarity); }
    [[nodiscard]] bool CanEmit(Polarity polarity) const { return m_gun.CanEmit(polarity); }
    [[nodiscard]] bool IsEmitting(Polarity polarity) const { return m_gun.IsEmitting(polarity); }
    void SetOnDeath(std::function<void()> callback) { m_health.onDeath = std::move(callback); }

    // 銃を構えているか。UI と照射判定が「今撃てるか」を知るための唯一の窓口。
    [[nodiscard]] bool AreWeaponsDrawn() const { return m_weaponRig.IsDrawn(); }
    // 今ビームの手前に居る相手。カメラ演出や UI が同じ 1 体を見るための窓口。
    [[nodiscard]] GameObject* CurrentTarget() const { return m_aim.CurrentTarget(); }

    void OnStart() override;
    void OnUpdate() override;
    void OnLateUpdate() override;
    void OnFixedUpdate() override;
    // 内部モジュールが作ったランタイム GameObject (照準枠・ビーム) はルートに置かれる。
    // Player と一緒には消えないため、ここから畳ませる。
    void OnDestroy() override;
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
    // 視線は狙いの姿勢から分ける。頭は銃を持たないので、コーンの配分にも
    // 発砲判定にも関わらない。首の速さを触っても腕の追従には波及しない。
    PlayerHeadLookComponent  m_headLook;
    // 対象の表示はエイムの選定から分ける。枠の見た目を触っても選び方には波及しない。
    AimMarkerComponent       m_aimMarker;
    // 画面中央の照準。枠が「線に入った 1 体」を指すのに対し、こちらは
    // 「これから線を引く 1 点」を指す。役割が違うので別モジュールにする。
    CrosshairComponent       m_crosshair;
    // 地形へ残る焼け跡。跡の見た目を触っても照射の作りには波及しない。
    BeamScorchComponent      m_scorch;
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
    m_headLook.Reflect(r_);
    m_aimMarker.Reflect(r_);
    m_crosshair.Reflect(r_);
    m_scorch.Reflect(r_);
    m_health.Reflect(r_);
    m_gun.Reflect(r_);
}

inline void PlayerComponent::BindModules()
{
    m_controller.AdoptContext(*this);
    m_weaponRig.AdoptContext(*this);
    m_aim.AdoptContext(*this);
    m_headLook.AdoptContext(*this);
    m_aimMarker.AdoptContext(*this);
    m_crosshair.AdoptContext(*this);
    m_scorch.AdoptContext(*this);
    m_health.AdoptContext(*this);
    m_gun.AdoptContext(*this);

    m_controller.tuning.ref = tuning.ref;
    m_health.tuning.ref = tuning.ref;
    // 照準は 6.2 のビーム半径と射程で線を引く。銃と同じアセットを見せないと、
    // 触れて見えた敵と塗れる敵がずれる。
    m_aim.tuning.ref = polarityTuning.ref;
    m_gun.tuning.ref = polarityTuning.ref;
    m_controller.SetAimComponent(&m_aim);
    m_controller.SetWeaponRig(&m_weaponRig);
    m_health.SetController(&m_controller);
    m_gun.SetAimComponent(&m_aim);
    m_gun.SetController(&m_controller);
    // 収納中に撃てないようにするため、銃の出し入れの状態を見せる。
    m_gun.SetWeaponRig(&m_weaponRig);
    m_aimMarker.SetAimComponent(&m_aim);
    m_crosshair.SetAimComponent(&m_aim);
    m_crosshair.SetGun(&m_gun);
    m_crosshair.SetWeaponRig(&m_weaponRig);
    m_scorch.SetAimComponent(&m_aim);
    m_scorch.SetGun(&m_gun);
    m_headLook.SetAimComponent(&m_aim);
    m_headLook.SetController(&m_controller);
    m_headLook.SetWeaponRig(&m_weaponRig);
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
    m_headLook.OnStart();
    m_aimMarker.OnStart();
    m_crosshair.OnStart();
    m_scorch.OnStart();
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

    updateModule(m_controller);
    // コントローラーが同じフレームで受けた抜く / 収める要求を、その場で進める。
    // 先に回すと要求が 1 フレーム寝てしまい、押した感触が鈍る。
    updateModule(m_weaponRig);
    // 視線は「今フレームに抜いたか」まで確定してから決める。銃を抜いた瞬間の
    // フレームで首だけ 1 フレーム遅れると、構えと視線の立ち上がりがずれる。
    updateModule(m_headLook);
    updateModule(m_health);
}

// WHY 照準まわりを丸ごと Late に置くか:
//   照準・レーザー・枠は「カメラが今どこを向いているか」から引かれる。カメラの向きは
//   TpsCameraComponent が Script フェーズで確定させるが、同じフェーズの中では
//   スクリプトの実行順が決まっていない。Script で読むと前フレームの向きを掴むことがあり、
//   マウスを振ったフレームだけレーザーがクロスヘアから外れる。
//   フェーズを 1 つ下げれば「カメラの向きは確定済み」が順序に依らず保証される。
//
//   枠だけは対象の座標も要る。対象は物理と CharacterController で動くため、
//   Script フェーズで読むと半歩遅れて付いてくるのが、動きの速い相手ほどはっきり見える。
inline void PlayerComponent::OnLateUpdate()
{
    if (!enabled)
        return;
    const auto lateUpdateModule = [](Script& module) {
        if (module.enabled)
            module.ExecuteCallback(&Script::OnLateUpdate, module.GetTypeName());
    };

    // 線を先に引き、それを銃・枠・照準の 3 者が読む。順番を崩すと、触れて見えた敵と
    // 極性が乗る敵がずれる (6.5 が「事故が続くとストレスになる」と書いている食い違い)。
    lateUpdateModule(m_aim);
    lateUpdateModule(m_gun);
    lateUpdateModule(m_aimMarker);
    lateUpdateModule(m_crosshair);
    // 跡は「今フレーム照射したか」と「線が地形に届いたか」の両方が要る。
    // 銃と照準の後に回して、同じフレームの結果だけを見る。
    lateUpdateModule(m_scorch);
}

inline void PlayerComponent::OnFixedUpdate()
{
    if (enabled && m_controller.enabled)
        m_controller.ExecuteCallback(&Script::OnFixedUpdate, m_controller.GetTypeName());
}

inline void PlayerComponent::OnDestroy()
{
    m_aimMarker.OnDestroy();
    m_crosshair.OnDestroy();
    m_scorch.OnDestroy();
    m_headLook.OnDestroy();
    m_gun.OnDestroy();
}

} // namespace sandbox
