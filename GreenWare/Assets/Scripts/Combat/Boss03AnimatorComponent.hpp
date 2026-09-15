/// @file    Boss03AnimatorComponent.hpp
/// @brief   Boss03 のアニメーション駆動。AI から Animator への唯一の入口
/// @author  Hasegawa Jin
/// @date    2026-09-15
///
/// WHY AI と Animator の間に 1 枚挟むか (BossAnimatorComponent と同じ):
///   Boss03.animcontroller のパラメーターは 20 個あり、うち 6 個は «翼ごとの Detach
///   トリガーと同名のレイヤー» という対応まで持っている。AI が名前で直に叩く形に
///   すると、綴りを間違えても SetTrigger は黙って何もしないので、症状が
///   「その翼だけ落ちない」という追えない形で出る。
///
/// WHY クリップの «当たる時刻» をここが持つか:
///   Slam_L は 3.0 秒、Slam_Combo は 5.0 秒あるが、連撃の拍は 0.62 秒
///   (Docs/boss03.md)。素の速さでは 1 拍のあいだに振り切れず、絵と判定が必ずずれる。
///   叩きつけの瞬間が拍へ重なる再生速度を出して、そのステートだけに当てる。
///   尺を知っているのはクリップ、拍を知っているのは AI なので、掛け合わせる場所を
///   1 つに決める ── 2 か所から書くと «絵だけ拍から外れる» が残る。
///
/// ⚠ 叩きつけ・発射の瞬間 (Impact) は Blender 側に印が無い。既定値は尺からの当たりを
///   付けた仮の値で、手触りを詰めるときは **ここだけ**を触ること。AI の秒数は
///   «拍» の設計値なので、絵を合わせるために動かすと遊びの方が壊れる。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/Boss03AnimParams.hpp>
#include <Scripts/Game/HitstopManagerComponent.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

/// Inspector のプレビュー用。AI が使うものではない。
enum class Boss03MotionPreview : int {
    SlamLeft = 0,
    SlamRight,
    SlamCombo,
    LaserFan,
    Pulse,
    Dash,
    TurnLeft,
    TurnRight,
    Cocoon,
    Hit,
    Stagger,
    Death,
    DetachWing,
};

class Boss03AnimatorComponent : public Script {
    FBZZ_SCRIPT(Boss03AnimatorComponent)

public:
    FBZZ_GROUP("浮遊")
    FBZZ_FIELD(bool, autoDriveMotion, true, "Auto Drive")
    FBZZ_TOOLTIP("ワールド位置の差分から «前 / 左 / 右» を毎フレーム選ぶ。"
                 "AI が SetMotion() で明示的に与えるなら切る")
    FBZZ_FIELD_RANGE(float, motionThreshold, 0.45f, "動き出す速さ [m/s]", 0.01f, 5.0f)
    FBZZ_TOOLTIP("これより遅いと Idle のまま。低すぎると止まっていても浮遊クリップが混じる")
    FBZZ_FIELD_RANGE(float, motionDamping, 8.0f, "Motion Damping", 0.0f, 40.0f)
    FBZZ_TOOLTIP("差分から取った速度の平滑化。生の値は 1 フレーム単位で跳ねるので必ず要る")

    FBZZ_GROUP("Hit Layer")
    FBZZ_FIELD(std::string, hitLayerName, "Hit", "レイヤー名")
    FBZZ_TOOLTIP("Hit を加算する Additive レイヤー。繭・転倒・撃破の間は鳴らない"
                 "(条件は .animcontroller 側が持っている)")
    FBZZ_FIELD_RANGE(float, hitLightWeight, 0.40f, "Light Weight", 0.0f, 1.0f)
    FBZZ_TOOLTIP("軽い一撃 (strength 0) でのレイヤー重み。1 にすると全段が締めと同じ深さになる")

    // WHY 尺と «当たる時刻» を別々に持つか: 再生速度は «当たる時刻 ÷ 欲しい秒数» で
    //   決まる。尺だけ持つと «クリップの真ん中で当たる» を前提にすることになり、
    //   溜めの長いクリップほど早く当たってしまう。
    FBZZ_GROUP("クリップ")
    FBZZ_FIELD_RANGE(float, slamClipSeconds, 3.0f, "Slam_L/R の尺 [秒]", 0.1f, 20.0f)
    FBZZ_TOOLTIP("ExportManifest.json の seconds。Slam_L / Slam_R は 3.0 秒")
    FBZZ_FIELD_RANGE(float, slamImpact01, 0.55f, "Slam の叩きつけ", 0.05f, 1.0f)
    FBZZ_TOOLTIP("クリップのどこで翼が床へ届くか [0,1]。**Blender に印が無いので仮の値**")
    FBZZ_FIELD_RANGE(float, slamComboClipSeconds, 5.0f, "Slam_Combo の尺 [秒]", 0.1f, 20.0f)
    FBZZ_FIELD_RANGE(float, slamComboImpact01, 0.62f, "Slam_Combo の叩きつけ", 0.05f, 1.0f)
    FBZZ_FIELD_RANGE(float, laserClipSeconds, 3.33f, "Laser_Fan の尺 [秒]", 0.1f, 20.0f)
    FBZZ_FIELD_RANGE(float, laserFire01, 0.45f, "Laser_Fan の発射", 0.05f, 1.0f)
    FBZZ_FIELD_RANGE(float, pulseClipSeconds, 3.33f, "Pulse の尺 [秒]", 0.1f, 20.0f)
    FBZZ_FIELD_RANGE(float, pulseBurst01, 0.50f, "Pulse の弾け", 0.05f, 1.0f)
    FBZZ_FIELD_RANGE(float, dashClipSeconds, 3.0f, "Dash_InPlace の尺 [秒]", 0.1f, 20.0f)
    FBZZ_FIELD_RANGE(float, dashLaunch01, 0.35f, "Dash の踏み出し", 0.05f, 1.0f)
    FBZZ_FIELD_RANGE(float, clipSpeedMin, 0.35f, "再生の下限", 0.05f, 1.0f)
    FBZZ_FIELD_RANGE(float, clipSpeedMax, 5.00f, "再生の上限", 1.0f, 12.0f)
    FBZZ_TOOLTIP("拍 0.62 秒に対して Slam (3.0 秒 × 0.55) は 2.7 倍。"
                 "上限を下げると «拍に間に合わない絵» を選ぶことになる")

    FBZZ_GROUP("プレビュー")
    FBZZ_FIELD_ENUM(Boss03MotionPreview, previewMotion, Boss03MotionPreview::SlamLeft,
                    "動き",
                    "Slam L", "Slam R", "Slam Combo (締め)",
                    "Laser Fan (焼き払い)", "Pulse (衝撃波)", "Dash (低空突進)",
                    "Turn L", "Turn R", "Cocoon (閉じる / 開く)",
                    "Hit (被弾)", "Stagger (崩れる)", "Death (撃破)",
                    "Detach (翼を 1 枚落とす)")
    FBZZ_TOOLTIP("AI が無くても 1 本ずつ見るための仮再生。Play 中に押すこと。"
                 "ゲーム側からは使わない")
    void PlayPreview();
    FBZZ_BUTTON(PlayPreview, "Play Preview")
    void StopPreview();
    FBZZ_BUTTON(StopPreview, "Stop / Revive")
    FBZZ_FIELD_RANGE(float, previewBeatSeconds, 0.62f, "拍 [秒]", 0.05f, 4.0f)
    FBZZ_TOOLTIP("プレビューで «当たる» までの秒数。既定は連撃 1 拍ぶん")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugState, "", "状態")
    FBZZ_FIELD_READ_ONLY(float, debugSpeed, 0.0f, "速さ (m/s)")
    FBZZ_FIELD_READ_ONLY(int, debugMotion, 0, "Motion")
    FBZZ_FIELD_READ_ONLY(int, debugWingsDetached, 0, "落ちた翼")

    void OnStart()  override;
    void OnUpdate() override;

    /// 片翼の叩きつけ。secondsToImpact の後に床へ届くよう再生速度を合わせる。
    void Slam(bool left, float secondsToImpact);
    /// 両翼の叩きつけ (連撃の締め)。
    void SlamCombo(float secondsToImpact);
    /// 扇の焼き払い。secondsToFire の後に線が通る。
    void LaserFan(float secondsToFire);
    /// 衝撃波。secondsToBurst の後に輪が広がる。
    void Pulse(float secondsToBurst);
    /// 低空の突進。secondsToLaunch の後に踏み出す。
    void Dash(float secondsToLaunch);
    /// その場旋回。大きく向き直るときだけ。
    void Turn(bool left);

    /// 浮遊の向き。Auto Drive を切っているときに使う。
    void SetMotion(Boss03Motion motion);

    /// 繭。true で Close → Cocoon_Idle、false で Deploy → Idle。
    ///
    /// WHY 被弾を止める口を別に持たないか: Boss03.animcontroller は Hit レイヤーの
    ///     遷移条件に «IsClosed が false» を持っている。繭の間は絵の側が鳴らさない。
    void SetClosed(bool closed);
    /// 崩れて倒れている。Stagger の最終フレームで晒したまま止まる。
    void SetStaggered(bool staggered);
    /// 撃破。Death は崩れたまま止まり、Idle へは戻らない。
    void SetDead(bool dead);

    /// 被弾リアクション。加算レイヤーへ 1 発差し込む。strength は一撃の重さ [0,1]。
    void ReactToHit(float strength = 1.0f);

    /// 翼を 1 枚落とす。畳んだ姿はその翼のレイヤーが保持する。
    /// 既に落ちている翼なら何もしない。
    void DetachWing(int wing);
    /// 翼を 1 枚だけ戻す。投げた翼が帰ってきたときに使う。
    ///
    /// WHY トリガーで戻せないか: Detach レイヤーの Detached ステートは出口を持たない
    ///     (畳んだ姿をそのまま保持するため)。ステートを名指しで指すしかない。
    void RestoreWing(int wing);
    /// 落ちた翼を全部戻す (プレビューと Play のやり直し用)。
    void RestoreWings();
    [[nodiscard]] bool IsWingDetached(int wing) const
    { return wing >= 0 && wing < kBoss03WingCount && m_detached[wing]; }

    /// 盤面のテンポ (Boss03AiComponent の Tempo) を預ける。
    ///
    /// WHY AI から直に Animator の速度を書かせないか: 再生速度は 1 つしか無く、
    ///     こちらは拍へ合わせるために «ステートごとの速度» を書く。全体の速度を
    ///     2 か所から書くと、後から書いた方が相手を消す。
    void SetTempo(float tempo) { m_tempo = std::max(tempo, 0.0f); }

    [[nodiscard]] float Speed()      const { return m_speed; }
    [[nodiscard]] bool  IsDead()     const { return m_dead; }
    [[nodiscard]] bool  IsClosed()   const { return m_closed; }
    [[nodiscard]] bool  IsStaggered()const { return m_staggered; }
    /// 浮遊でも Idle でもない = 何かの手の最中。
    [[nodiscard]] bool  IsBusy() const;

private:
    /// ワールド位置の差分から «前 / 左 / 右» を選ぶ。
    void SampleMotion(float dt);
    /// «当たる時刻» を欲しい秒数へ合わせる再生速度。
    [[nodiscard]] float StateSpeedFor(float clipSeconds, float impact01, float wantSeconds) const;
    /// ステートの速度を当ててからトリガーを引く。順番を逆にすると 1 フレームだけ素の速さで鳴る。
    void Fire(const char* stateName, float speed, const char* trigger);

    Vector3 m_lastPosition = Vector3::ZERO;
    bool    m_hasLastPose  = false;
    float   m_speed        = 0.0f;
    float   m_tempo        = 1.0f;

    bool m_closed    = false;
    bool m_staggered = false;
    bool m_dead      = false;

    Boss03Motion m_motion = Boss03Motion::Hold;
    bool         m_detached[kBoss03WingCount] = {};
    /// プレビューで次に落とす翼。押すたびに 1 枚ずつ進む。
    int          m_previewWing = 0;
};

FBZZ_REFLECT(Boss03AnimatorComponent)


inline void Boss03AnimatorComponent::OnStart()
{
    m_hasLastPose = false;
    m_speed       = 0.0f;
    m_closed      = false;
    m_staggered   = false;
    m_dead        = false;
    m_motion      = Boss03Motion::Hold;
    m_previewWing = 0;

    // Play をまたぐと Animator は Controller の初期値へ戻る。こちらの真偽値と
    // 食い違ったまま始まると、«開いているのに繭の判定» のような状態が残る。
    animator.SetBool(boss03anim::kIsClosed,    false);
    animator.SetBool(boss03anim::kIsStaggered, false);
    animator.SetBool(boss03anim::kIsDead,      false);
    animator.SetFloat(boss03anim::kMotion,     0.0f);
    if (!hitLayerName.empty()) animator.SetLayerWeight(hitLayerName, 1.0f);

    RestoreWings();
}

inline void Boss03AnimatorComponent::OnUpdate()
{
    const float dt = std::max(Time::deltaTime, 0.0f);

    if (autoDriveMotion) SampleMotion(dt);
    animator.SetFloat(boss03anim::kMotion, static_cast<float>(static_cast<int>(m_motion)));

    debugState  = animator.GetCurrentState();
    debugSpeed  = m_speed;
    debugMotion = static_cast<int>(m_motion);

    // 当事者の凍結 (ヒットストップの手応え) が掛かっている間は速度を書かない。
    // 凍結は «この相手の再生速度を 0 にする» で作られているので、毎フレーム書き
    // 続けると次のフレームで解けて凍結が一切効かなくなる (BossAnimatorComponent と同じ)。
    const auto* stop = HitstopManagerComponent::Instance();
    if (stop && stop->IsAnimationFrozen(scene.Self())) return;

    animator.SetSpeed(m_tempo);
}

inline void Boss03AnimatorComponent::SampleMotion(float dt)
{
    const Vector3 position = transform.worldPosition;

    if (!m_hasLastPose || dt <= 0.0f) {
        m_lastPosition = position;
        m_hasLastPose  = true;
        return;
    }

    const Vector3 delta{ position.x - m_lastPosition.x, 0.0f, position.z - m_lastPosition.z };
    m_lastPosition = position;

    const float raw   = delta.Length() / dt;
    const float alpha = Clamp01(1.0f - std::exp(-std::max(motionDamping, 0.0f) * dt));
    m_speed += (raw - m_speed) * alpha;

    if (m_speed < std::max(motionThreshold, 0.01f) || delta.LengthSq() <= EPSILON) {
        m_motion = Boss03Motion::Hold;
        return;
    }

    // «前» はモデルの正面。素の +Z で測ると、このモデルでは前後が入れ替わって
    // 前進のたびに Hover_Forward ではなく «後退＝その場» になる
    // (kBoss03FacingOffsetDegrees ─ AI の向き直りと同じ定数から引く)。
    const Vector3 localFacing = Quaternion::FromAxisAngle(Vector3::UP,
                                    ToRad(kBoss03FacingOffsetDegrees)) * Vector3::FORWARD;
    const Vector3 facing  = transform.worldRotation * localFacing;
    const Vector3 forward = Vector3{ facing.x, 0.0f, facing.z }.NormalizedOr(Vector3::FORWARD);
    const Vector3 right   = Vector3{ forward.z, 0.0f, -forward.x };
    const Vector3 step    = delta.NormalizedOr(forward);

    const float ahead = Vector3::Dot(step, forward);
    const float side  = Vector3::Dot(step, right);

    // WHY 後退を Hold にするか: 後ろへ下がるクリップは無い。前進で代用すると
    //     «前へ向かって後ろへ滑る» になるので、下がる間は浮いたまま見せる。
    if (std::fabs(side) > std::fabs(ahead)) m_motion = side > 0.0f ? Boss03Motion::Right
                                                                   : Boss03Motion::Left;
    else                                    m_motion = ahead > 0.0f ? Boss03Motion::Forward
                                                                   : Boss03Motion::Hold;
}

inline float Boss03AnimatorComponent::StateSpeedFor(float clipSeconds, float impact01,
                                                    float wantSeconds) const
{
    const float want = std::max(wantSeconds, 0.01f);
    const float hit  = std::max(clipSeconds, 0.01f) * Clamp01(impact01);
    return std::clamp(hit / want, std::max(clipSpeedMin, 0.01f), std::max(clipSpeedMax, 0.01f));
}

inline void Boss03AnimatorComponent::Fire(const char* stateName, float speed, const char* trigger)
{
    animator.SetStateSpeed(stateName, speed);
    animator.SetTrigger(trigger);
}

inline void Boss03AnimatorComponent::Slam(bool left, float secondsToImpact)
{
    const float speed = StateSpeedFor(slamClipSeconds, slamImpact01, secondsToImpact);
    Fire(left ? boss03anim::kSlamL : boss03anim::kSlamR, speed,
         left ? boss03anim::kSlamL : boss03anim::kSlamR);
}

inline void Boss03AnimatorComponent::SlamCombo(float secondsToImpact)
{
    Fire(boss03anim::kSlamCombo,
         StateSpeedFor(slamComboClipSeconds, slamComboImpact01, secondsToImpact),
         boss03anim::kSlamCombo);
}

inline void Boss03AnimatorComponent::LaserFan(float secondsToFire)
{
    Fire(boss03anim::kLaserFan, StateSpeedFor(laserClipSeconds, laserFire01, secondsToFire),
         boss03anim::kLaserFan);
}

inline void Boss03AnimatorComponent::Pulse(float secondsToBurst)
{
    Fire(boss03anim::kPulse, StateSpeedFor(pulseClipSeconds, pulseBurst01, secondsToBurst),
         boss03anim::kPulse);
}

inline void Boss03AnimatorComponent::Dash(float secondsToLaunch)
{
    Fire(boss03anim::kDash, StateSpeedFor(dashClipSeconds, dashLaunch01, secondsToLaunch),
         boss03anim::kDash);
}

inline void Boss03AnimatorComponent::Turn(bool left)
{
    animator.SetTrigger(left ? boss03anim::kTurnL : boss03anim::kTurnR);
}

inline void Boss03AnimatorComponent::SetMotion(Boss03Motion motion)
{
    m_motion = motion;
}

inline void Boss03AnimatorComponent::SetClosed(bool closed)
{
    m_closed = closed;
    animator.SetBool(boss03anim::kIsClosed, closed);
}

inline void Boss03AnimatorComponent::SetStaggered(bool staggered)
{
    m_staggered = staggered;
    animator.SetBool(boss03anim::kIsStaggered, staggered);
}

inline void Boss03AnimatorComponent::SetDead(bool dead)
{
    m_dead = dead;
    if (dead) {
        // 倒れる前に立っていた状態を畳む。繭のまま死ぬと、開かずに止まった絵になる。
        SetClosed(false);
        SetStaggered(false);
    }
    animator.SetBool(boss03anim::kIsDead, dead);
}

inline void Boss03AnimatorComponent::ReactToHit(float strength)
{
    // 重みは «次の 1 発» の深さ。鳴り終わりまで持ち越すので、軽い一撃の直後に
    // 締めが来れば深く、逆なら浅くなる (同時には鳴らないので取り合いは起きない)。
    if (!hitLayerName.empty())
        animator.SetLayerWeight(hitLayerName,
                                Lerp(Clamp01(hitLightWeight), 1.0f, Clamp01(strength)));
    animator.SetTrigger(boss03anim::kHit);
}

inline void Boss03AnimatorComponent::DetachWing(int wing)
{
    if (wing < 0 || wing >= kBoss03WingCount || m_detached[wing]) return;
    m_detached[wing] = true;
    ++debugWingsDetached;
    animator.SetTrigger(kBoss03WingTriggers[wing]);
}

inline void Boss03AnimatorComponent::RestoreWing(int wing)
{
    if (wing < 0 || wing >= kBoss03WingCount || !m_detached[wing]) return;
    m_detached[wing] = false;
    debugWingsDetached = std::max(debugWingsDetached - 1, 0);
    animator.PlayLayerState(kBoss03WingTriggers[wing], "Attached");
}

inline void Boss03AnimatorComponent::RestoreWings()
{
    for (int i = 0; i < kBoss03WingCount; ++i) {
        m_detached[i] = false;
        // トリガーでは戻せない (Detached からの遷移を持たないレイヤー)。
        // ステートを直接指して畳んだ姿を解く。
        animator.PlayLayerState(kBoss03WingTriggers[i], "Attached");
    }
    debugWingsDetached = 0;
}

inline bool Boss03AnimatorComponent::IsBusy() const
{
    const std::string state = animator.GetCurrentState();
    return !state.empty() &&
           state != boss03anim::kIdle &&
           state != boss03anim::kHoverForward &&
           state != boss03anim::kHoverLeft &&
           state != boss03anim::kHoverRight;
}

inline void Boss03AnimatorComponent::PlayPreview()
{
    const float beat = std::max(previewBeatSeconds, 0.05f);

    switch (previewMotion) {
    case Boss03MotionPreview::SlamLeft:   Slam(true,  beat);        break;
    case Boss03MotionPreview::SlamRight:  Slam(false, beat);        break;
    case Boss03MotionPreview::SlamCombo:  SlamCombo(beat);          break;
    case Boss03MotionPreview::LaserFan:   LaserFan(beat);           break;
    case Boss03MotionPreview::Pulse:      Pulse(beat);              break;
    case Boss03MotionPreview::Dash:       Dash(beat);               break;
    case Boss03MotionPreview::TurnLeft:   Turn(true);               break;
    case Boss03MotionPreview::TurnRight:  Turn(false);              break;
    case Boss03MotionPreview::Cocoon:     SetClosed(!m_closed);     break;
    case Boss03MotionPreview::Hit:        ReactToHit();             break;
    case Boss03MotionPreview::Stagger:    SetStaggered(!m_staggered); break;
    case Boss03MotionPreview::Death:      SetDead(true);            break;

    case Boss03MotionPreview::DetachWing:
        DetachWing(m_previewWing);
        m_previewWing = (m_previewWing + 1) % kBoss03WingCount;
        break;
    }
}

inline void Boss03AnimatorComponent::StopPreview()
{
    SetDead(false);
    SetClosed(false);
    SetStaggered(false);
    SetMotion(Boss03Motion::Hold);
    RestoreWings();
    m_previewWing = 0;
}

} // namespace sandbox
