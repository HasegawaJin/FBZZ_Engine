/// @file    PlayerComponent.hpp
/// @brief   Player の実行入口。移動・エイム・体力・双剣を 1 コンポーネントへ束ねる。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// @note 1 コンポーネントにする: 各責務は PlayerControllerComponent / PlayerAimComponent /
///       PlayerHealthComponent / BladeComponent に分割したまま、シーンへは PlayerComponent
///       だけをアタッチし、構成漏れを減らしつつモジュール単位の実装・差し替えは残す。
/// @note fzdata を必須にする: 移動・HP・銃の成立値をコード側の既定値へフォールバックさせると、
///       共有アセットを編集しても一部だけ別の値で動く。Play 開始時に 2 枚の参照を検査し、
///       未設定なら動作を止める。
#pragma once

#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/BossBreakComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Combat/IDamageable.hpp>
#include <Scripts/Game/CameraFollowManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Game/TimeManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Data/PlayerTuning.hpp>
#include <Scripts/Data/BladeTuning.hpp>
#include <Scripts/Player/AimMarkerComponent.hpp>
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Player/PlayerBreathComponent.hpp>
#include <Scripts/Player/PlayerControllerComponent.hpp>
#include <Scripts/Player/PlayerHeadLookComponent.hpp>
#include <Scripts/Player/PlayerHealthComponent.hpp>
#include <Scripts/Player/PlayerParryComponent.hpp>
#include <Scripts/Player/BladeComponent.hpp>
#include <Scripts/Player/WeaponRigComponent.hpp>
#include <Scripts/Vfx/BladeChargeGlowComponent.hpp>
#include <Scripts/Vfx/BladeSteelComponent.hpp>
#include <Scripts/Vfx/BladeTrailComponent.hpp>
#include <Scripts/Vfx/SlashCutFxComponent.hpp>
#include <Scripts/Vfx/SpinSlashFxComponent.hpp>
#include <Scripts/Vfx/DodgeAfterimageComponent.hpp>
#include <Scripts/Vfx/SlashScarComponent.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

using namespace fbzz::scene;

namespace sandbox {

class PlayerComponent : public Script, public IDamageable {
    FBZZ_SCRIPT_DERIVED(PlayerComponent, Script, IDamageable)

    FBZZ_REQUIRE_COMPONENT(CharacterControllerComponent, RigidBodyComponent)
    FBZZ_OPTIONAL_COMPONENT(AnimatorComponent, IKSolverComponent, AudioSourceComponent)

public:
    /// これらは Player の成立条件であり、PlayerComponent 以外には公開しない。
    FBZZ_REQUIRED_ASSET(PlayerTuning, tuning, "Player Tuning")
    FBZZ_REQUIRED_ASSET(BladeTuning, bladeTuning, "Blade Tuning")

    /// @name IDamageable
    /// @{
    /// 敵も樽もプレイヤーも同じ入口で殴れるようにする。CombatManager が
    /// 「誰に何点入れたか」を 1 本の経路で記録できるのはこれによる。
    bool ApplyDamage(int amount) override { return TakeDamage(amount); }
    /// 一撃の入口。弾き → ジャスト回避 → 体力、の順に通す。
    /// @note 弾きは回避より先: 排他だが、順番を決めないと «どちらの手柄か» が組み方で変わる。
    /// @note ガードは弾きより後: 先に見ると同じボタンで押した瞬間に必ずガードが勝ち
    ///       `AddParry` に届かず崩しが溜まらないまま «防げてはいる» ように見えていた。弾きは
    ///       «頭の 0.22 秒»、ガードはその後の押しっぱなしなので、この順でも食われない。
    /// @note ガードでは崩しは溜まらない: 溜まると押しっぱなしで崩せ «崩すのは弾きだけ»
    ///       (Docs/break-parry.md) が壊れる。弾けない手 (ビーム・パルス・扇) は Unblockable
    ///       で構えていても素通りする。
    PlayerHitResult ReceiveHit(int amount, const fbzz::math::Vector3* fromWorld,
                               PlayerHitKind kind) override
    {
        if (amount <= 0 || !m_health.IsAlive()) return PlayerHitResult::Ignored;
        if (kind == PlayerHitKind::Parryable && m_parry.IsParryActive()) {
            m_parry.OnParried(amount, fromWorld);
            return PlayerHitResult::Parried;
        }
        if (kind == PlayerHitKind::Parryable && m_parry.IsGuardActive()) {
            m_parry.OnGuarded(amount, fromWorld);
            return PlayerHitResult::Guarded;
        }
        if (TryPerfectDodge(fromWorld)) return PlayerHitResult::Dodged;
        if (!m_health.TakeDamage(amount)) return PlayerHitResult::Ignored;
        OnDamaged(fromWorld);
        return PlayerHitResult::Damaged;
    }
    /// 押されるのは移動の話なので、そのまま操作へ渡す。
    void ApplyKnockback(const fbzz::math::Vector3& fromWorld, float speed, float seconds) override
    {
        m_controller.Knockback(transform.worldPosition - fromWorld, speed, seconds);
    }
    [[nodiscard]] int  CurrentHealth() const override { return m_health.Current(); }
    [[nodiscard]] int  MaxHealth()     const override { return m_health.MaxHealth(); }
    [[nodiscard]] bool IsAlive()       const override { return m_health.IsAlive(); }

    /// 外部システムは個別モジュールを探さず、Player の公開 API だけを使う。
    [[nodiscard]] int Current() const { return m_health.Current(); }
    [[nodiscard]] float NormalizedHealth() const { return m_health.Normalized(); }
    /// 息の残量 (1 = 満タン)。HUD が読む。
    [[nodiscard]] float NormalizedBreath() const { return m_breath.Normalized(); }
    [[nodiscard]] float DodgeStaminaSpent() const { return m_breath.DodgeSpent(); }
    [[nodiscard]] float GuardStaminaSpent() const { return m_breath.GuardSpent(); }
    [[nodiscard]] float MaxStamina() const { return m_breath.MaxBreath(); }
    /// 息が尽きて回避もガードも出せない状態か。
    [[nodiscard]] bool IsBreathExhausted() const { return m_breath.IsExhausted(); }
    /// 直近 seconds 秒に «息が無くて出せなかった» があったか。
    [[nodiscard]] bool BreathDeniedWithin(float seconds) const
    { return m_breath.DeniedWithin(seconds); }
    /// ダメージの唯一の入口。回避中なら弾いて、ジャスト回避の報酬を配る。
    /// @note 体力側 (PlayerHealthComponent) でなくここで弾く: 回避を知るのは移動側、報酬を
    ///       受け取るのは剣で、体力はどちらも知らない。3 つを束ねるのはこの入口だけ。
    [[nodiscard]] bool TakeDamage(int amount)
    {
        return ReceiveHit(amount, nullptr, PlayerHitKind::Unblockable) == PlayerHitResult::Damaged;
    }
    void Heal(int amount) { m_health.Heal(amount); }
    /// 仕切り直しは息も戻す。体力だけ満タンで息が空だと、立て直した最初の 1 回が転がれない。
    void ResetHealth() { m_health.ResetHealth(); m_breath.ResetBreath(); }
    /// @}
    /// @name 双剣
    /// @{
    /// これまでに転がった回数。差を見て «今 1 回避けた» を知る
    /// (PlayerParryComponent::ParryCount と同じ «通知ではなく数» の形)。
    [[nodiscard]] int DodgeSerial() const { return m_controller.DodgeSerial(); }
    [[nodiscard]] bool IsSwinging() const { return m_blades.IsSwinging(); }
    [[nodiscard]] BladeSide SwingSide() const { return m_blades.SwingSide(); }

    void SetOnDeath(std::function<void()> callback) { m_health.onDeath = std::move(callback); }

    /// 武器を構えているか。UI が読む唯一の窓口。
    [[nodiscard]] bool AreWeaponsDrawn() const { return m_weaponRig.IsDrawn(); }
    /// 今狙っている相手。カメラ演出・枠・剣が同じ 1 体を見るための窓口。
    [[nodiscard]] GameObject* CurrentTarget() const { return m_aim.CurrentTarget(); }

    FBZZ_FIELD(bool, terrainRecovery, false, "地形下への貫通から復帰")
    void RecoverBelowTerrain();
    void OnStart() override;
    void OnUpdate() override;
    void OnLateUpdate() override;
    void OnFixedUpdate() override;
    /// 内部モジュールが作ったランタイム GameObject (照準枠・ビーム) はルートに置かれる。
    /// Player と一緒には消えないため、ここから畳ませる。
    void OnDestroy() override;
    void OnDisable() override
    {
        if (auto* manager = TimeManagerComponent::Instance()) manager->EndParryRush();
        animator.SetLocalTimeScale(1.0f);
        physics.SetLocalTimeScale(1.0f);
        if (auto* label = scene.Find("HUD_ParryRush")) label->SetActive(false);
        if (auto* fill = scene.Find("HUD_ParryRushFill")) fill->SetActive(false);
    }
    /// 内部モジュールのギズモは自動では呼ばれないため、ここから中継する。
    void OnDrawGizmos() override { m_weaponRig.OnDrawGizmos(); }

    void PlayHitAnimation() { m_controller.PlayHitAnimation(); }
    /// @}

    /// @name 座標を外から置く演出 (登攀)
    /// @{
    /// @note Player 越しに渡す: 操作は内部モジュールが抱えており scene からは見つからない。
    ///       個別モジュールを探させない方針に合わせ、押し続ける形の要求だけをここから通す。

    /// 移動・跳躍・回避・重力の適用を 1 フレームぶん止める。
    /// lockInput を立てると刀の入力 (斬撃・弾き・とどめ) まで止まる。
    void RequestSuspend(bool lockInput) { m_controller.RequestSuspend(lockInput); }
    /// この向きへ体を向けるよう 1 フレームぶん要求する。
    void RequestFacing(const fbzz::math::Vector3& direction) { m_controller.RequestFacing(direction); }
    /// 体力がここより下へ減らないよう 1 フレームぶん要求する (PlayerHealthComponent)。
    void RequestDamageFloor(int minHealth) { m_health.RequestDamageFloor(minHealth); }
    /// 拘束中の体を倒す角度 [度]。pitch は前へ、roll は右へ。1 フレームぶん。
    void RequestLean(float pitchDegrees, float rollDegrees)
    { m_controller.RequestLean(pitchDegrees, rollDegrees); }

    /// 刀を背へ納める / 手へ戻す。
    /// @note 演出側がクリップを流すだけでは足りない: 納刀は «クリップ» でなく «刀がどの
    ///       ソケットに付いているか» の変更で、移し替える時刻も左右で違う (WeaponRigComponent)。
    ///       クリップだけ流すと芝居中も刀は手に握られたまま ── 登攀で壁を貫く。
    /// 刀が手にあるかは AreWeaponsDrawn() が既に答える。
    /// 刀の入力まで止まっているか。
    [[nodiscard]] bool IsInputLocked() const { return m_controller.IsInputLocked(); }
    /// @}


private:
    /// 内部 Script は通常の ScriptSystem から呼ばれないため、親と同じ実行コンテキストを渡す。
    void BindModules();
    [[nodiscard]] bool HasRequiredAssets() const;

    /// 回避中に攻撃が重なったか。重なっていれば (同じ回避で初めてなら) 報酬を配って true。
    /// fromWorld は «どこから来た一撃か» (無い経路もある)。
    bool TryPerfectDodge(const fbzz::math::Vector3* fromWorld);
    /// ジャスト回避の見返り。スロー・剣の Flux・手触り・集計。
    void OnPerfectDodge(const fbzz::math::Vector3* fromWorld);
    /// 通った一撃の後始末。縁に向きを渡し、続けていた弾きの積み上げを切る。
    void OnDamaged(const fbzz::math::Vector3* fromWorld);
    /// 土壇場 (残り HP が Last Stand 以下) を画面とボスの崩しへ申告する。
    void DriveLastStand();

    /// 盤面に立っているボスの崩しゲージを全部なぞる。
    /// @note 1 体を引く口は使わない: 被弾・ジャスト回避・土壇場は «盤面の出来事» で相手を
    ///       問わない。名簿は順序を持たないので 1 体だけ引くと、2 体居る盤面では毎回
    ///       どちらかが乱数で選ばれ片方のゲージだけが育つ。
    template <class Fn>
    void ForEachBoss(Fn&& apply) const
    {
        std::vector<GameObject*> bosses;
        CollectBossesOnBoard(scene, bosses);
        for (GameObject* boss : bosses)
            if (auto* brk = scene.GetScript<BossBreakComponent>(boss)) apply(*brk);
    }
    [[nodiscard]] bool IsLastStand() const
    { return tuning && tuning->lastStandHealth > 0 && m_health.IsAlive()
          && m_health.Current() <= tuning->lastStandHealth; }

    /// 最後に報酬を配った回避の番号。同じ 1 回の回避で複数回当たっても 1 度だけ。
    int m_lastFluxDodge = 0;
    /// 前フレームに申告した土壇場。変わったフレームだけボスを引き直す。
    bool m_lastStandSent = false;
    bool m_deathAnimationPlayed = false;

    PlayerControllerComponent m_controller;
    /// 抜く / 収める はコントローラーから独立させる。入力の受け付け条件を触っても
    /// 銃の付け替えには波及しない (逆も同じ)。
    WeaponRigComponent       m_weaponRig;
    PlayerAimComponent       m_aim;
    /// 視線は狙いの姿勢から分ける。頭は銃を持たないので、コーンの配分にも
    /// 発砲判定にも関わらない。首の速さを触っても腕の追従には波及しない。
    PlayerHeadLookComponent  m_headLook;
    /// 対象の表示はエイムの選定から分ける。枠の見た目を触っても選び方には波及しない。
    AimMarkerComponent       m_aimMarker;
    PlayerHealthComponent    m_health;
    /// 息 (スタミナ)。体力と分けるのは、減らすものも戻すものもまったく違うため ─
    /// 体力は敵だけが減らし、息は自分の選んだ手が減らして自分の攻めが戻す。
    PlayerBreathComponent    m_breath;
    /// 双剣。斬る判定を持つ唯一の入口。
    BladeComponent   m_blades;
    /// 弾きと とどめ。«受ける» と «仕留める» はどちらもボスの状態で決まるので、剣から分ける。
    PlayerParryComponent     m_parry;
    /// 刀身が通った面。剣の «判定» から分ける ─ 軌跡の見た目を触っても射程や発生には
    /// 波及しないし、丸ごと外しても «斬る → 崩す → 仕留める» の芯は全部成立する。
    /// @note 刀身の残像 (MeshTrail) から戻した: 残像は «刀の形» を並べるため、0.2 秒の振りでは
    ///       数本ばらばらに浮いて «斬った弧» として繋がらなかった。掃過面の帯なら切っ先の弧が
    ///       1 本の線として残る。
    BladeTrailComponent      m_bladeTrail;
    /// 当たった瞬間の一閃。«振った» (残像) と «斬れた» (線) を分ける ─ 空振りでは出ない。
    SlashCutFxComponent      m_slashCut;
    SpinSlashFxComponent     m_spinFx;
    /// 回転斬りの «一周»。刃が体の裏へ回る区間は帯が自分に隠れるので、そこを輪で補う。
    /// 回避中の残像。無敵の «時間» を体の絵で伝える層で、回避そのものには触らない。
    DodgeAfterimageComponent m_dodgeGhost;
    /// 溜めている量を «剣そのもの» で伝える発光。剣の挙動には触らない。
    BladeChargeGlowComponent m_bladeGlow;
    /// 刀身の焼き。溜め・振り・段・弾きを «刃» で言う層で、剣の挙動には触れない。
    BladeSteelComponent      m_bladeSteel;
    /// 斬った面へ残る痕。«何回斬ったか» を盤面に残す層で、切っても芯は成立する。
    SlashScarComponent       m_slashScar;
};

/// PlayerComponent 自身の fzdata 参照と、責務別モジュールの Inspector 項目を同じカードへ並べる。
/// モジュール側の tuning 参照は反映せず、共有アセットのスロットを二重表示しない。
inline void PlayerComponent::Reflect(::fbzz::scene::IReflector& r_)
{
    r_.BeginField("tuning", "Player Tuning");
    r_.Field("Player Tuning", tuning.ref);
    r_.BeginField("bladeTuning", "BladeSide Tuning");
    r_.Field("BladeSide Tuning", bladeTuning.ref);
    m_controller.Reflect(r_);
    m_weaponRig.Reflect(r_);
    m_aim.Reflect(r_);
    m_headLook.Reflect(r_);
    m_aimMarker.Reflect(r_);
    m_health.Reflect(r_);
    m_breath.Reflect(r_);
    m_blades.Reflect(r_);
    m_parry.Reflect(r_);
    m_bladeTrail.Reflect(r_);
    m_slashCut.Reflect(r_);
    m_spinFx.Reflect(r_);
    m_dodgeGhost.Reflect(r_);
    m_bladeGlow.Reflect(r_);
    m_bladeSteel.Reflect(r_);
    m_slashScar.Reflect(r_);
}

inline void PlayerComponent::BindModules()
{
    m_controller.AdoptContext(*this);
    m_weaponRig.AdoptContext(*this);
    m_aim.AdoptContext(*this);
    m_headLook.AdoptContext(*this);
    m_aimMarker.AdoptContext(*this);
    m_health.AdoptContext(*this);
    m_breath.AdoptContext(*this);
    m_blades.AdoptContext(*this);
    m_parry.AdoptContext(*this);
    m_bladeTrail.AdoptContext(*this);
    m_slashCut.AdoptContext(*this);
    m_spinFx.AdoptContext(*this);
    m_dodgeGhost.AdoptContext(*this);
    m_bladeGlow.AdoptContext(*this);
    m_bladeSteel.AdoptContext(*this);
    m_slashScar.AdoptContext(*this);

    m_controller.tuning.ref = tuning.ref;
    m_health.tuning.ref = tuning.ref;
    m_breath.tuning.ref = tuning.ref;
    /// @note 息を払うのは回避と構え、戻すのは当たった斬撃と弾き。3 つとも «要求する側» が
    ///       息を知っていて、息の側は誰が払ったかを知らない。
    m_controller.SetBreath(&m_breath);
    m_parry.SetBreath(&m_breath);
    m_blades.SetBreath(&m_breath);
    m_controller.SetAimComponent(&m_aim);
    m_controller.SetWeaponRig(&m_weaponRig);
    m_health.SetController(&m_controller);
    m_aimMarker.SetAimComponent(&m_aim);
    m_headLook.SetAimComponent(&m_aim);
    m_headLook.SetController(&m_controller);
    m_headLook.SetWeaponRig(&m_weaponRig);

    m_blades.tuning.ref = bladeTuning.ref;
    m_blades.SetAimComponent(&m_aim);
    m_blades.SetController(&m_controller);
    m_blades.SetBladeTrail(&m_bladeTrail);
    m_blades.SetSlashCut(&m_slashCut);
    m_blades.SetSpinFx(&m_spinFx);
    /// @note 剣は弾き・とどめの最中は黙る。倒れた相手へは攻撃ボタンからもとどめが出る。
    m_blades.SetParry(nullptr);
    m_parry.SetController(&m_controller);
    /// @note 残像は «回避が出た / 終わった» を問い合わせるだけ。回避の側は残像を知らない。
    m_dodgeGhost.SetController(&m_controller);
    /// @note 発光は溜めの «結果» を読むだけ。剣の側は発光を知らない。
    m_bladeGlow.SetBlades(&m_blades);
    /// @note 刃も «結果» を読むだけ。弾きが無くても刃文と帯は動く。
    m_bladeSteel.SetBlades(&m_blades);
    m_bladeSteel.SetParry(&m_parry);
    m_blades.SetSlashScar(&m_slashScar);
}

inline bool PlayerComponent::HasRequiredAssets() const
{
    if (tuning && bladeTuning)
        return true;
    debug.LogError(
        "PlayerComponent requires PlayerTuning and BladeTuning .fzdata assets. "
        "Assign both assets before entering Play mode.");
    return false;
}

inline bool PlayerComponent::TryPerfectDodge(const fbzz::math::Vector3* fromWorld)
{
    if (!tuning || !tuning->dodgeInvulnerable) return false;
    if (!m_controller.InDodgeIFrames(tuning->dodgeInvulnerableSeconds)) return false;

    const int serial = m_controller.DodgeSerial();
    if (serial != m_lastFluxDodge) {
        m_lastFluxDodge = serial;
        OnPerfectDodge(fromWorld);
    }
    return true;
}

inline void PlayerComponent::OnDamaged(const fbzz::math::Vector3* fromWorld)
{
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        if (fromWorld) screen->SetHurtSource(*fromWorld);
    /// @note 被弾・回避・土壇場は «誰か 1 体との» 出来事ではなく盤面の出来事なので、
    ///       立っている相手の全部へ配る。名簿は順序を持たないので、1 体だけ引くと
    ///       2 体居る盤面では乱数で選ばれた片方だけが育つ (理由は IBoss.hpp を参照)。
    ForEachBoss([](BossBreakComponent& brk) { brk.ResetParryStreak(); });
    /// @note 土壇場に入った瞬間は同じフレームで申告する。次の OnUpdate を待つと、
    ///       落ちた HP の赤と鼓動が 1 フレームずれて «別々の出来事» に見える。
    DriveLastStand();

}

inline void PlayerComponent::DriveLastStand()
{
    const bool lastStand = IsLastStand();
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->SetDanger(m_health.IsAlive()
            ? Clamp01((0.30f - m_health.Normalized()) / 0.30f) : 0.0f);
    if (lastStand == m_lastStandSent) return;
    m_lastStandSent = lastStand;
    /// @note ボスの探索は状態が変わったフレームだけ。毎フレーム型で引くのは重い。
    const float scale = lastStand ? (std::max)(tuning->lastStandBreakScale, 1.0f) : 1.0f;
    ForEachBoss([scale](BossBreakComponent& brk) { brk.SetGainScale(scale); });
}

inline void PlayerComponent::OnPerfectDodge(const fbzz::math::Vector3* fromWorld)
{
    float flux = (std::max)(tuning->perfectDodgeFluxSeconds, 0.0f);
    /// @note 土壇場は猶予が伸びる。かわした後に «押す時間» が長いほど、最後の 1 から返せる。
    if (IsLastStand()) flux *= (std::max)(tuning->lastStandFluxScale, 1.0f);
    if (flux > 0.0f) m_blades.GrantFlux(flux);

    /// @note 息も満タンへ戻す。読み切った 1 回だけが «呼吸ごと» 返り、報酬を Flux と同じ
    ///       瞬間へ集める。一定量でなく満タンにするのは、ジャスト回避が出るのは息の薄い
    ///       場面でもあり、«次の一振りは満溜め» でも転がる息が残らなければ受け取れないため。
    m_breath.Refill();

    /// @note 世界を «ゆっくり» にする。かわしたことを見せつつ Flux を押す時間で、ヒットストップ
    ///       (Override) とは層が違うので重なっても互いを消さない。止め (Hitstop) でなくスロー
    ///       にするのは、止めは «当たった» の語、回避は «当たらなかった» の語だから
    ///       (同じ絵だと避けたのに殴られたように読める)。掛けは速く、戻しは長く。
    if (tuning->perfectDodgeSlowSeconds > 0.0f && tuning->perfectDodgeSlowScale < 1.0f)
        if (auto* timeManager = TimeManagerComponent::Instance())
            timeManager->SlowFor(tuning->perfectDodgeSlowScale,
                                 tuning->perfectDodgeSlowSeconds,
                                 tuning->perfectDodgeSlowIn, tuning->perfectDodgeSlowOut);

    /// @note 縁をプレイヤー色に光らせる。被弾の赤 (Flash) とも極の赤青 (Surge) とも
    ///       取り違えない色で、«自分に良いことが起きた» だけを言う。
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->Surge(kColorPlayer, 0.8f, 0.35f);
    /// @note 画面の縁と同時に体の残像も白熱させ長く引く。縁は «良いことが起きた» としか
    ///       言えないため、何が良かったか (この姿勢で潜り抜けたこと) は並んだ残像にしか
    ///       写らない。スロー (perfectDodgeSlowSeconds) 中ずっと見えるよう寿命も伸ばす。
    m_dodgeGhost.Flash();
    /// @note 一撃が «体のすぐ横を通った» ことを床にも残す。ここまでの見返り (縁・画角・スロー・
    ///       残像) はどれも «画面» か «自分» に出て、すれ違った出来事そのものは写らないため、
    ///       通り過ぎた向きへ床を煽り相手と避けた向きを 1 枚の絵で繋げる。
    if (auto* vfx = VfxManagerComponent::Instance()) {
        /// @note どこから来たか分からない経路 (TakeDamage) では、転がっていった向きへ流す。
        fbzz::math::Vector3 away = fromWorld ? (transform.worldPosition - *fromWorld)
                                             : m_controller.DodgeDirection();
        away.y = 0.0f;
        vfx->PlayGroundDust(transform.worldPosition,
                            away.NormalizedOr(fbzz::math::Vector3::FORWARD), 0.7f, 1.15f);
    }
    /// @note 画角も一瞬開く。スローで世界が遅くなる瞬間に画面が広がると、«時間が伸びた»
    ///       ではなく «かわして視界が開けた» に読める。締めの一撃より控えめ。
    if (auto* follow = CameraFollowManagerComponent::Instance())
        follow->PunchFov(0.45f);
    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(0.2f, 0.9f, 0.12f);
    /// @note 満溜めの合図をそのまま使う。Flux は «満溜めが手に入った» ことなので、
    ///       普段の溜めで覚えた音がここでも同じ意味で鳴る。
    se::Play(audio, se::kSwordFlux);

    if (auto* combat = CombatManagerComponent::Instance()) combat->AddPerfectDodge();

    /// @note 弾けない手 (輪・ビーム・柱) にも崩しへの道を残す。弾きより薄い量。
    ForEachBoss([](BossBreakComponent& brk) { brk.AddPerfectDodge(); });
}

inline void PlayerComponent::OnStart()
{
    BindModules();
    m_lastFluxDodge    = 0;
    m_lastStandSent    = false;
    m_deathAnimationPlayed = false;
    /// @note 弾きを «切った状態» から始めない: m_parry.enabled を切ったままにすると
    ///       updateModule が弾きを一度も回さず、崩しがガードの下に隠れたまま溜まらない
    ///       (本作の中核 Docs/break-parry.md)。既定では切らない。
    if (!HasRequiredAssets()) {
        enabled = false;
        return;
    }

    /// @note 再生速度を素へ戻す: ヒットストップ (`animator.SetSpeed(go, 0)`) を戻すのは
    ///       固めた側 (HitstopManagerComponent) の残り秒数計算だが、その最中に Script DLL
    ///       をリロードすると管理側だけ作り直され、誰も 0 を戻せなくなる。ボスは毎フレーム
    ///       速度を書くため自力で戻るが、プレイヤーは書き手が居ないので入口で 1 度戻す。
    animator.SetSpeed(1.0f);
    if (ragdoll.IsEnabled()) ragdoll.SetEnabled(false);

    /// @note «殴られる側» として名乗る。CombatManager はこの名簿からしか引けない
    ///       (理由は IDamageable::Of のコメント)。
    IDamageable::Bind(scene.Self(), this);

    /// @note エイムを先に更新し、同じフレームの移動アニメーションと発射判定が同じ対象を見る。
    m_controller.OnStart();
    /// @note 銃の追従設定はコントローラーの後。銃が腰へスナップしてから最初の描画が走る。
    m_weaponRig.OnStart();
    m_aim.OnStart();
    m_headLook.OnStart();
    m_aimMarker.OnStart();
    m_health.OnStart();
    m_breath.OnStart();
    m_blades.OnStart();
    m_parry.OnStart();
    m_bladeTrail.OnStart();
    m_slashCut.OnStart();
    m_spinFx.OnStart();
    m_dodgeGhost.OnStart();
    m_bladeGlow.OnStart();
    m_bladeSteel.OnStart();
    m_slashScar.OnStart();
}

inline void PlayerComponent::OnUpdate()
{
    if (!enabled)
        return;
    if (!m_health.IsAlive())
        if (auto* manager = TimeManagerComponent::Instance()) manager->EndParryRush();
    const float playerClockScale = TimeManagerComponent::PlayerTimeScale();
    animator.SetLocalTimeScale(playerClockScale);
    physics.SetLocalTimeScale(playerClockScale);
    /// @note 内部モジュールも Script の一種だが、直接呼び出すとモジュール内の空 Ref /
    ///       無効な EntityRef が親 PlayerComponent の例外として扱われる。既存の
    ///       ExecuteCallback 境界へ通し、問題の責務だけを停止して発生元の型名を残す。
    const auto updateModule = [](Script& module) {
        if (module.enabled)
            module.ExecuteCallback(&Script::OnUpdate, module.GetTypeName());
    };

    /// @note 撃破後は立ったまま結果画面を待つ。RequestSuspend は «1 フレームぶんの要求»
    ///       なので毎フレーム言い直す。1 度きりだと次のフレームでコントローラーが入力を
    ///       当て直し、結果画面の手前で移動できてしまう。
    if (!m_health.IsAlive()) {
        m_controller.RequestSuspend(true);
        if (!m_deathAnimationPlayed) {
            animator.StopSlot(m_blades.slashLayerName, 0.0f);
            animator.StopSlot(m_blades.slashLayerNameB, 0.0f);
            animator.StopSlot(m_blades.airSlashLayerName, 0.0f);
            animator.StopSlot(m_controller.hitLayerName, 0.0f);
            animator.Play("Death");
            m_deathAnimationPlayed = true;
        }
    }
    {
        /// @note 剣を止めるのは «倒れたとき» だけ。構えている間の停止は弾きの側が
        ///       RequestMoveSpeedScale で持っている (構えは 0.22 秒しか続かない)。
        m_blades.SetGuarding(!m_health.IsAlive());
        /// @note Controller の AnyState は «倒れていない間だけ» 割り込みを通す門にこれを使う。
        ///       立てないと、撃破後に残っている Trigger が Death を蹴り出して起き上がる。
        animator.SetBool("IsDead", !m_health.IsAlive());
    }

    /// @note 剣 → 纏い → コントローラー、の順で回す: 剣は振った «瞬間» に纏いを切り替える
    ///       (Charge) ため、纏いを先に回すと引力/斥力が 1 フレーム遅れ «斬った勢いで飛ぶ»
    ///       が移動と繋がらない。コントローラーは OnUpdate の頭で速度の要求を取り込むので
    ///       両方より後でなければ要求が 1 フレーム寝る。弾きは剣より先 (剣は Busy を見る)。
    updateModule(m_parry);
    updateModule(m_blades);
    /// @note 放電は纏いの «結果» を読むだけなので、纏いより後。先に回すと 1 フレーム前の
    ///       極で走り、左右を斬り分けた瞬間だけ色が前の剣のまま出る。
    updateModule(m_controller);
    /// @note 一閃は剣より後。当たったフレームのうちに 1 コマ目を張る。
    updateModule(m_slashCut);
    /// @note 回転の輪も同じ理由で剣より後。振り出したフレームから開き始める。
    updateModule(m_spinFx);
    /// @note 残像は回避の «結果» を読むだけなので、操作より後。先に回すと出だしの 1 枚が
    ///       前フレームの姿勢で置かれ、跳び出した瞬間だけ残像が体より後ろにずれる。
    updateModule(m_dodgeGhost);
    /// @note 発光は剣より後。先に回すと 1 フレーム前の溜め比で光り、押した瞬間だけ暗い。
    updateModule(m_bladeGlow);
    updateModule(m_bladeSteel);
    updateModule(m_slashScar);
    /// @note コントローラーが同じフレームで受けた抜く / 収める要求を、その場で進める。
    ///       先に回すと要求が 1 フレーム寝てしまい、押した感触が鈍る。
    updateModule(m_weaponRig);
    /// @note 視線は「今フレームに抜いたか」まで確定してから決める。銃を抜いた瞬間の
    ///       フレームで首だけ 1 フレーム遅れると、構えと視線の立ち上がりがずれる。
    updateModule(m_headLook);
    updateModule(m_health);
    /// @note 息は払う側 (回避・構え) より後。先に回すと、同じフレームに払ったぶんの
    ///       待ち時間 (Regen Delay) が 1 フレーム寝て、転がった直後に回復が始まる。
    updateModule(m_breath);
    /// @note 土壇場は «状態» なので毎フレーム申告する (回復・死・シーン跨ぎで自然に消える)。
    DriveLastStand();
}

/// @note 照準まわりを丸ごと Late に置く: 照準・レーザー・枠はカメラの向きから引かれるが、
///       TpsCameraComponent が Script フェーズで確定させても同フェーズ内の実行順は
///       決まっておらず、Script で読むと前フレームの向きを掴みマウスを振ったフレームだけ
///       レーザーがクロスヘアから外れる。フェーズを 1 つ下げれば順序に依らず確定済みになる。
///       枠は対象の座標も要り、対象は物理/CharacterController で動くため Script フェーズで
///       読むと半歩遅れて見える。
inline void PlayerComponent::RecoverBelowTerrain()
{
    if (!terrainRecovery || !transform || !m_health.IsAlive()) return;
    const Vector3 current = transform.worldPosition;
    const float surface = scene.GetTerrainHeightAt(current);
    if (!std::isfinite(surface) || surface < -100000.0f || current.y >= surface - 0.05f) return;
    Vector3 local = transform.position;
    local.y += surface + 0.08f - current.y;
    transform.position = local;
    Vector3 velocity = physics.GetVelocity();
    velocity.y = std::max(velocity.y, 0.0f);
    physics.SetVelocity(velocity);
}

inline void PlayerComponent::OnLateUpdate()
{
    if (!enabled)
        return;
    RecoverBelowTerrain();
    const auto lateUpdateModule = [](Script& module) {
        if (module.enabled)
            module.ExecuteCallback(&Script::OnLateUpdate, module.GetTypeName());
    };

    /// @note 誰を狙っているかを先に決め、それを枠が読む。順番を崩すと、枠が付いている相手と
    ///       斬れる相手が 1 フレームずれる。
    lateUpdateModule(m_aim);
    lateUpdateModule(m_aimMarker);
    /// @note 軌跡は «刀身が今どこに在るか» を読む。刀は SocketAttachment (ConstraintSystem)
    ///       で手のボーンに追従するので、Script フェーズで読むとアニメーションが当たる前の
    ///       姿勢を掴む ─ 帯だけが 1 フレーム古い場所へ張られて、刃から離れて見える。
    lateUpdateModule(m_bladeTrail);
}

inline void PlayerComponent::OnFixedUpdate()
{
    physics.SetLocalTimeScale(TimeManagerComponent::PlayerTimeScale());
    if (enabled && m_controller.enabled)
        m_controller.ExecuteCallback(&Script::OnFixedUpdate, m_controller.GetTypeName());
}

inline void PlayerComponent::OnDestroy()
{
    OnDisable();
    IDamageable::Unbind(scene.Self(), this);
    m_aimMarker.OnDestroy();
    m_headLook.OnDestroy();
    m_bladeTrail.OnDestroy();
    m_slashCut.OnDestroy();
    m_spinFx.OnDestroy();
    m_dodgeGhost.OnDestroy();
    m_slashScar.OnDestroy();
    /// @note 溜めの震え・唸り・パッドは «押している間» 続く。畳まずに消えると鳴りっぱなしになる。
    m_blades.OnDestroy();
}

} // namespace sandbox
