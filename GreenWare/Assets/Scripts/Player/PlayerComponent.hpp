/// @file    PlayerComponent.hpp
/// @brief   Player の実行入口。移動・エイム・体力・双剣を 1 コンポーネントへ束ねる。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY 1 コンポーネントにするか:
/// 各責務は PlayerControllerComponent / PlayerAimComponent / PlayerHealthComponent /
/// BladeComponent に分割したまま、シーンへは PlayerComponent だけをアタッチする。
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
#include <Scripts/Player/PlayerControllerComponent.hpp>
#include <Scripts/Player/PlayerHeadLookComponent.hpp>
#include <Scripts/Player/PlayerHealthComponent.hpp>
#include <Scripts/Player/PlayerParryComponent.hpp>
#include <Scripts/Player/BladeComponent.hpp>
#include <Scripts/Player/WeaponRigComponent.hpp>
#include <Scripts/Vfx/BladeChargeGlowComponent.hpp>
#include <Scripts/Vfx/BladeTrailComponent.hpp>
#include <Scripts/Vfx/DodgeAfterimageComponent.hpp>
#include <Scripts/Vfx/SlashScarComponent.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
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
    FBZZ_REQUIRED_ASSET(BladeTuning, bladeTuning, "Blade Tuning")

    // ── IDamageable ─────────────────────────────────────────────────────────
    // 敵も樽もプレイヤーも同じ入口で殴れるようにする。CombatManager が
    // 「誰に何点入れたか」を 1 本の経路で記録できるのはこれによる。
    bool ApplyDamage(int amount) override { return TakeDamage(amount); }
    /// 一撃の入口。弾き → ジャスト回避 → 体力、の順に通す。
    ///
    /// WHY 弾きを回避より先に見るか: 構えている最中は回避していない (回避中は構えられない)
    ///     ので実際には排他だが、順番を決めておかないと «どちらの手柄か» が組み方で変わる。
    ///     弾きは押した意思がはっきりしている側なので先に取る。
    PlayerHitResult ReceiveHit(int amount, const fbzz::math::Vector3* fromWorld,
                               PlayerHitKind kind) override
    {
        if (amount <= 0 || !m_health.IsAlive()) return PlayerHitResult::Ignored;
        if (kind == PlayerHitKind::Parryable && m_parry.IsParryActive()) {
            m_parry.OnParried(amount, fromWorld);
            return PlayerHitResult::Parried;
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

    // 外部システムは個別モジュールを探さず、Player の公開 API だけを使う。
    [[nodiscard]] int Current() const { return m_health.Current(); }
    [[nodiscard]] float NormalizedHealth() const { return m_health.Normalized(); }
    /// ダメージの唯一の入口。回避中なら弾いて、ジャスト回避の報酬を配る。
    ///
    /// WHY 体力側 (PlayerHealthComponent) ではなくここで弾くか: 回避しているのを
    ///     知っているのは移動側、報酬を受け取るのは剣。体力はどちらも知らない。
    ///     3 つを束ねているのはこの入口だけなので、判定もここに置く。
    [[nodiscard]] bool TakeDamage(int amount)
    {
        return ReceiveHit(amount, nullptr, PlayerHitKind::Unblockable) == PlayerHitResult::Damaged;
    }
    void Heal(int amount) { m_health.Heal(amount); }
    void ResetHealth() { m_health.ResetHealth(); }
    // ── 双剣 ────────────────────────────────────────────────────────────────
    [[nodiscard]] bool IsSwinging() const { return m_blades.IsSwinging(); }
    [[nodiscard]] BladeSide SwingSide() const { return m_blades.SwingSide(); }

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

    /// 回避中に攻撃が重なったか。重なっていれば (同じ回避で初めてなら) 報酬を配って true。
    /// fromWorld は «どこから来た一撃か» (無い経路もある)。
    bool TryPerfectDodge(const fbzz::math::Vector3* fromWorld);
    /// ジャスト回避の見返り。スロー・剣の Flux・手触り・集計。
    void OnPerfectDodge(const fbzz::math::Vector3* fromWorld);
    /// 通った一撃の後始末。縁に向きを渡し、続けていた弾きの積み上げを切る。
    void OnDamaged(const fbzz::math::Vector3* fromWorld);
    /// 土壇場 (残り HP が Last Stand 以下) を画面とボスの崩しへ申告する。
    void DriveLastStand();
    [[nodiscard]] bool IsLastStand() const
    { return tuning && tuning->lastStandHealth > 0 && m_health.IsAlive()
          && m_health.Current() <= tuning->lastStandHealth; }

    /// 最後に報酬を配った回避の番号。同じ 1 回の回避で複数回当たっても 1 度だけ。
    int m_lastFluxDodge = 0;
    /// 前フレームに申告した土壇場。変わったフレームだけボスを引き直す。
    bool m_lastStandSent = false;

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
    // 双剣。斬る判定を持つ唯一の入口。
    BladeComponent   m_blades;
    // 弾きと とどめ。«受ける» と «仕留める» はどちらもボスの状態で決まるので、剣から分ける。
    PlayerParryComponent     m_parry;
    // 刀身が通った面。剣の «判定» から分ける ─ 軌跡の見た目を触っても射程や発生には
    // 波及しないし、丸ごと外しても «斬る → 崩す → 仕留める» の芯は全部成立する。
    BladeTrailComponent      m_bladeTrail;
    // 回避中の残像。無敵の «時間» を体の絵で伝える層で、回避そのものには触らない。
    DodgeAfterimageComponent m_dodgeGhost;
    // 溜めている量を «剣そのもの» で伝える発光。剣の挙動には触らない。
    BladeChargeGlowComponent m_bladeGlow;
    // 斬った面へ残る痕。«何回斬ったか» を盤面に残す層で、切っても芯は成立する。
    SlashScarComponent       m_slashScar;
};

// PlayerComponent 自身の fzdata 参照と、責務別モジュールの Inspector 項目を同じカードへ並べる。
// モジュール側の tuning 参照は反映せず、共有アセットのスロットを二重表示しない。
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
    m_blades.Reflect(r_);
    m_parry.Reflect(r_);
    m_bladeTrail.Reflect(r_);
    m_dodgeGhost.Reflect(r_);
    m_bladeGlow.Reflect(r_);
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
    m_blades.AdoptContext(*this);
    m_parry.AdoptContext(*this);
    m_bladeTrail.AdoptContext(*this);
    m_dodgeGhost.AdoptContext(*this);
    m_bladeGlow.AdoptContext(*this);
    m_slashScar.AdoptContext(*this);

    m_controller.tuning.ref = tuning.ref;
    m_health.tuning.ref = tuning.ref;
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
    // 剣は弾き・とどめの最中は黙る。倒れた相手へは攻撃ボタンからもとどめが出る。
    m_blades.SetParry(&m_parry);
    m_parry.SetController(&m_controller);
    // 残像は «回避が出た / 終わった» を問い合わせるだけ。回避の側は残像を知らない。
    m_dodgeGhost.SetController(&m_controller);
    // 発光は溜めの «結果» を読むだけ。剣の側は発光を知らない。
    m_bladeGlow.SetBlades(&m_blades);
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
    if (!m_controller.IsDodging()) return false;

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
    if (GameObject* boss = FindBossOnBoard(scene))
        if (auto* brk = scene.GetScript<BossBreakComponent>(boss)) brk->ResetParryStreak();
    // 土壇場に入った瞬間は同じフレームで申告する。次の OnUpdate を待つと、
    // 落ちた HP の赤と鼓動が 1 フレームずれて «別々の出来事» に見える。
    DriveLastStand();
}

inline void PlayerComponent::DriveLastStand()
{
    const bool lastStand = IsLastStand();
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->SetDanger(lastStand ? 1.0f : 0.0f);
    if (lastStand == m_lastStandSent) return;
    m_lastStandSent = lastStand;
    // ボスの探索は状態が変わったフレームだけ。毎フレーム型で引くのは重い。
    if (GameObject* boss = FindBossOnBoard(scene))
        if (auto* brk = scene.GetScript<BossBreakComponent>(boss))
            brk->SetGainScale(lastStand ? (std::max)(tuning->lastStandBreakScale, 1.0f) : 1.0f);
}

inline void PlayerComponent::OnPerfectDodge(const fbzz::math::Vector3* fromWorld)
{
    float flux = (std::max)(tuning->perfectDodgeFluxSeconds, 0.0f);
    // 土壇場は猶予が伸びる。かわした後に «押す時間» が長いほど、最後の 1 から返せる。
    if (IsLastStand()) flux *= (std::max)(tuning->lastStandFluxScale, 1.0f);
    if (flux > 0.0f) m_blades.GrantFlux(flux);

    // 世界を «ゆっくり» にする。かわしたことを見せる時間であり、Flux を押す時間でもある。
    // ヒットストップ (Override) とは層が違うので、重なっても互いを消さない。
    //
    // WHY 止め (Hitstop) ではなくスローか: 止めは «当たった» の語で、回避は «当たらなかった»
    //     の語。同じ絵で言うと、避けたのに殴られたように読める。掛けは速く、戻しは長く。
    if (tuning->perfectDodgeSlowSeconds > 0.0f && tuning->perfectDodgeSlowScale < 1.0f)
        if (auto* timeManager = TimeManagerComponent::Instance())
            timeManager->SlowFor(tuning->perfectDodgeSlowScale,
                                 tuning->perfectDodgeSlowSeconds,
                                 tuning->perfectDodgeSlowIn, tuning->perfectDodgeSlowOut);

    // 縁をプレイヤー色に光らせる。被弾の赤 (Flash) とも極の赤青 (Surge) とも
    // 取り違えない色で、«自分に良いことが起きた» だけを言う。
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->Surge(kColorPlayer, 0.8f, 0.35f);
    // 画面の縁と同時に、体の残像も白熱させて長く引く。
    //
    // WHY 縁だけでは足りないか: 縁は «良いことが起きた» としか言えない。何が良かったのか
    //     ─ 攻撃をこの姿勢で潜り抜けたこと ─ は、そのとき並んでいる残像にしか写っていない。
    //     スロー (perfectDodgeSlowSeconds) の間ずっとその形が見えるよう、寿命も伸ばす。
    m_dodgeGhost.Flash();
    // 一撃が «体のすぐ横を通った» ことを床にも残す。
    //
    // WHY 盤面にも 1 発要るか: ここまでの見返りは縁・画角・スロー・残像で、どれも
    //     «画面» か «自分» に出る。攻撃してきた側とすれ違ったという出来事そのものは
    //     どこにも写っていないので、スローが明けると «なぜか良いことが起きた» が残る。
    //     通り過ぎた向きへ床を煽ると、避けた相手と避けた向きが 1 枚の絵で繋がる。
    if (auto* vfx = VfxManagerComponent::Instance()) {
        // どこから来たか分からない経路 (TakeDamage) では、転がっていった向きへ流す。
        fbzz::math::Vector3 away = fromWorld ? (transform.worldPosition - *fromWorld)
                                             : m_controller.DodgeDirection();
        away.y = 0.0f;
        vfx->PlayGroundDust(transform.worldPosition,
                            away.NormalizedOr(fbzz::math::Vector3::FORWARD), 0.7f, 1.15f);
    }
    // 画角も一瞬開く。スローで世界が遅くなる瞬間に画面が広がると、«時間が伸びた»
    // ではなく «かわして視界が開けた» に読める。締めの一撃より控えめ。
    if (auto* follow = CameraFollowManagerComponent::Instance())
        follow->PunchFov(0.45f);
    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(0.2f, 0.9f, 0.12f);
    // 満溜めの合図をそのまま使う。Flux は «満溜めが手に入った» ことなので、
    // 普段の溜めで覚えた音がここでも同じ意味で鳴る。
    se::Play(audio, se::kBladeChargeUpFull);

    if (auto* combat = CombatManagerComponent::Instance()) combat->AddPerfectDodge();

    // 弾けない手 (輪・ビーム・柱) にも崩しへの道を残す。弾きより薄い量。
    if (GameObject* boss = FindBossOnBoard(scene))
        if (auto* brk = scene.GetScript<BossBreakComponent>(boss)) brk->AddPerfectDodge();
}

inline void PlayerComponent::OnStart()
{
    BindModules();
    m_lastFluxDodge = 0;
    m_lastStandSent = false;
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
    m_blades.OnStart();
    m_parry.OnStart();
    m_bladeTrail.OnStart();
    m_dodgeGhost.OnStart();
    m_bladeGlow.OnStart();
    m_slashScar.OnStart();
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
    // 弾きは剣より先。同じフレームに押された弾きと攻撃は弾きが勝つ (剣は Busy を見る)。
    updateModule(m_parry);
    updateModule(m_blades);
    // 放電は纏いの «結果» を読むだけなので、纏いより後。先に回すと 1 フレーム前の
    // 極で走り、左右を斬り分けた瞬間だけ色が前の剣のまま出る。
    updateModule(m_controller);
    // 残像は回避の «結果» を読むだけなので、操作より後。先に回すと出だしの 1 枚が
    // 前フレームの姿勢で置かれ、跳び出した瞬間だけ残像が体より後ろにずれる。
    updateModule(m_dodgeGhost);
    // 発光は剣より後。先に回すと 1 フレーム前の溜め比で光り、押した瞬間だけ暗い。
    updateModule(m_bladeGlow);
    updateModule(m_slashScar);
    // コントローラーが同じフレームで受けた抜く / 収める要求を、その場で進める。
    // 先に回すと要求が 1 フレーム寝てしまい、押した感触が鈍る。
    updateModule(m_weaponRig);
    // 視線は「今フレームに抜いたか」まで確定してから決める。銃を抜いた瞬間の
    // フレームで首だけ 1 フレーム遅れると、構えと視線の立ち上がりがずれる。
    updateModule(m_headLook);
    updateModule(m_health);
    // 土壇場は «状態» なので毎フレーム申告する (回復・死・シーン跨ぎで自然に消える)。
    DriveLastStand();
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
    // 軌跡は «刀身が今どこに在るか» を読む。刀は SocketAttachment (ConstraintSystem)
    // で手のボーンに追従するので、Script フェーズで読むとアニメーションが当たる前の
    // 姿勢を掴む ─ 帯だけが 1 フレーム古い場所へ張られて、刃から離れて見える。
    lateUpdateModule(m_bladeTrail);
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
    m_bladeTrail.OnDestroy();
    m_dodgeGhost.OnDestroy();
    m_slashScar.OnDestroy();
    // 溜めの震え・唸り・パッドは «押している間» 続く。畳まずに消えると鳴りっぱなしになる。
    m_blades.OnDestroy();
}

} // namespace sandbox
