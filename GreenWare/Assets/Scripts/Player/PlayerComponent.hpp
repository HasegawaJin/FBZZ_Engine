/// @file    PlayerComponent.hpp
/// @brief   Player の実行入口。移動・エイム・体力・双剣を 1 コンポーネントへ束ねる。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY 1 コンポーネントにするか:
/// 各責務は PlayerControllerComponent / PlayerAimComponent / PlayerHealthComponent /
/// PolarityBladeComponent に分割したまま、シーンへは PlayerComponent だけをアタッチする。
/// これにより Player の構成漏れを減らし、モジュール単位の実装・レビュー・差し替えやすさを残す。
///
/// WHY fzdata を必須にするか:
/// 移動・HP・銃の成立値をコード側の既定値へフォールバックさせると、共有アセットを
/// 編集しても一部だけ別の値で動く。Play 開始時に 2 枚の参照を検査し、未設定なら動作を止める。
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
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Player/PlayerControllerComponent.hpp>
#include <Scripts/Player/PlayerHeadLookComponent.hpp>
#include <Scripts/Player/PlayerHealthComponent.hpp>
#include <Scripts/Player/PlayerPolarityComponent.hpp>
#include <Scripts/Player/PolarityBladeComponent.hpp>
#include <Scripts/Player/WeaponRigComponent.hpp>
#include <Scripts/Vfx/PlayerArcVfxComponent.hpp>
#include <Scripts/Vfx/SlashArcComponent.hpp>
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
    /// 押されるのは移動の話なので、そのまま操作へ渡す。
    void ApplyKnockback(const fbzz::math::Vector3& fromWorld, float speed, float seconds) override
    {
        m_controller.Knockback(transform.worldPosition - fromWorld, speed, seconds);
    }
    [[nodiscard]] int  CurrentHealth() const override { return m_health.Current(); }
    [[nodiscard]] int  MaxHealth()     const override { return m_health.MaxHealth(); }
    [[nodiscard]] bool IsAlive()       const override { return m_health.IsAlive(); }

    // 外部システムは個別モジュールを探さず、Player の公開 API だけを使う。
    [[nodiscard]] int Current() const { return m_health.Current(); }
    [[nodiscard]] float NormalizedHealth() const { return m_health.Normalized(); }
    [[nodiscard]] bool TakeDamage(int amount) { return m_health.TakeDamage(amount); }
    void Heal(int amount) { m_health.Heal(amount); }
    void ResetHealth() { m_health.ResetHealth(); }
    // ── 双剣と «自分の極» ───────────────────────────────────────────────────
    [[nodiscard]] bool IsSwinging() const { return m_blades.IsSwinging(); }
    [[nodiscard]] Polarity SwingPolarity() const { return m_blades.SwingPolarity(); }
    /// 今その極を «自分に» 帯びているか。
    [[nodiscard]] bool IsSelfCharged(Polarity polarity) const
    { return m_polarity.IsChargedWith(polarity); }
    /// 纏いの残り [0,1]。
    [[nodiscard]] float SelfChargeRatio() const { return m_polarity.ChargeRatio(); }

    void SetOnDeath(std::function<void()> callback) { m_health.onDeath = std::move(callback); }

    // 武器を構えているか。UI が読む唯一の窓口。
    [[nodiscard]] bool AreWeaponsDrawn() const { return m_weaponRig.IsDrawn(); }
    // 今狙っている相手。カメラ演出・枠・剣が同じ 1 体を見るための窓口。
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
    PlayerHealthComponent    m_health;
    // 極を «自分に» 使う側。«いつ帯びるか» は剣が決め、こちらは «帯びている間
    // どう動くか» だけを持つ。
    PlayerPolarityComponent  m_polarity;
    // 双剣。極を乗せる唯一の入口で、振った瞬間に上の m_polarity を切り替える。
    PolarityBladeComponent   m_blades;
    // 刃が通った軌跡。剣の «判定» から分ける ─ 弧の見た目を触っても射程や発生には
    // 波及しないし、軌跡を外しても «斬る → 極が乗る → 飛ぶ» の芯は全部成立する。
    SlashArcComponent        m_slashArc;
    // 纏っている極を体の外へ出す放電。«いつ帯びるか» は m_polarity が持ち、
    // こちらはそれを読んで走らせるだけ ─ 切っても芯は成立する。
    PlayerArcVfxComponent    m_arcVfx;
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
    m_health.Reflect(r_);
    m_polarity.Reflect(r_);
    m_blades.Reflect(r_);
    m_slashArc.Reflect(r_);
    m_arcVfx.Reflect(r_);
}

inline void PlayerComponent::BindModules()
{
    m_controller.AdoptContext(*this);
    m_weaponRig.AdoptContext(*this);
    m_aim.AdoptContext(*this);
    m_headLook.AdoptContext(*this);
    m_aimMarker.AdoptContext(*this);
    m_health.AdoptContext(*this);
    m_polarity.AdoptContext(*this);
    m_blades.AdoptContext(*this);
    m_slashArc.AdoptContext(*this);
    m_arcVfx.AdoptContext(*this);

    m_controller.tuning.ref = tuning.ref;
    m_health.tuning.ref = tuning.ref;
    m_polarity.tuning.ref = polarityTuning.ref;
    // 照準は «誰を狙っているか» を決める。剣も枠も同じ 1 体を見ないと、
    // 枠が付いている相手と斬れる相手がずれる。
    m_aim.tuning.ref = polarityTuning.ref;
    // 枠は «反発半径» の環も出す。半径の正本は共有アセット 1 枚に保つ。
    m_aimMarker.tuning.ref = polarityTuning.ref;
    m_controller.SetAimComponent(&m_aim);
    m_controller.SetWeaponRig(&m_weaponRig);
    m_health.SetController(&m_controller);
    m_aimMarker.SetAimComponent(&m_aim);
    m_headLook.SetAimComponent(&m_aim);
    m_headLook.SetController(&m_controller);
    m_headLook.SetWeaponRig(&m_weaponRig);

    // 剣が «いつ極を帯びるか» を決め、纏う側はその結果を動きへ変える。
    m_blades.tuning.ref = polarityTuning.ref;
    m_blades.SetAimComponent(&m_aim);
    m_blades.SetController(&m_controller);
    m_blades.SetPolarity(&m_polarity);
    m_blades.SetSlashArc(&m_slashArc);
    m_arcVfx.SetPolarity(&m_polarity);
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

    // «殴られる側» として名乗る。CombatManager はこの名簿からしか引けない
    // (理由は IDamageable::Of のコメント)。
    IDamageable::Bind(scene.Self(), this);

    // エイムを先に更新し、同じフレームの移動アニメーションと発射判定が同じ対象を見る。
    m_controller.OnStart();
    // 銃の追従設定はコントローラーの後。銃が腰へスナップしてから最初の描画が走る。
    m_weaponRig.OnStart();
    m_aim.OnStart();
    m_headLook.OnStart();
    m_aimMarker.OnStart();
    m_health.OnStart();
    m_polarity.OnStart();
    m_blades.OnStart();
    m_slashArc.OnStart();
    m_arcVfx.OnStart();
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

    // 剣 → 纏い → コントローラー、の順で回す。
    //
    // WHY この順か: 剣は振った «瞬間» に纏いを切り替える (Charge)。纏いを先に回すと、
    //     振ったフレームの引力/斥力が 1 フレーム遅れて始まり、«斬った勢いで飛ぶ» の
    //     勢いと移動が繋がらない。コントローラーは OnUpdate の頭で速度の要求を
    //     取り込むので、両方より後でなければ要求が 1 フレーム寝る。
    updateModule(m_blades);
    // 軌跡は剣の直後。同じフレームに張られた弧へその場で progress を書けば、
    // 判定が出た瞬間と光が走り出す瞬間が 1 フレームもずれない。
    updateModule(m_slashArc);
    updateModule(m_polarity);
    // 放電は纏いの «結果» を読むだけなので、纏いより後。先に回すと 1 フレーム前の
    // 極で走り、左右を斬り分けた瞬間だけ色が前の剣のまま出る。
    updateModule(m_arcVfx);
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

    // 誰を狙っているかを先に決め、それを枠が読む。順番を崩すと、枠が付いている相手と
    // 斬れる相手が 1 フレームずれる。
    lateUpdateModule(m_aim);
    lateUpdateModule(m_aimMarker);
}

inline void PlayerComponent::OnFixedUpdate()
{
    if (enabled && m_controller.enabled)
        m_controller.ExecuteCallback(&Script::OnFixedUpdate, m_controller.GetTypeName());
}

inline void PlayerComponent::OnDestroy()
{
    IDamageable::Unbind(scene.Self(), this);
    m_aimMarker.OnDestroy();
    m_headLook.OnDestroy();
    m_slashArc.OnDestroy();
    m_arcVfx.OnDestroy();
    // 溜めの震え・唸り・パッドは «押している間» 続く。畳まずに消えると鳴りっぱなしになる。
    m_blades.OnDestroy();
}

} // namespace sandbox
