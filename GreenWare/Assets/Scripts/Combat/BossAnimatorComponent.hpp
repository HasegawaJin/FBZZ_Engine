/// @file    BossAnimatorComponent.hpp
/// @brief   Boss「ポラリティ・コア」のアニメーション駆動。AI から Animator への唯一の入口
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY AI と Animator の間に 1 枚挟むか:
///   Boss.animcontroller のパラメーター名は 14 個あり、しかも「Charge は Trigger で
///   Charging は Bool」のように 1 つの行動が 2 本のパラメーターで表される。AI が
///   SetTrigger("Charge") と SetBool("Charging", true) を直接叩く形にすると、綴りを
///   間違えても SetTrigger は黙って何もしないうえ、片方だけ書き忘れると「突進の溜めまでは
///   出るが走らない」という、コードを読んでも原因の出ない壊れ方をする。行動 1 つを
///   関数 1 つに閉じて、パラメーター名をこのファイルの外へ出さない。
///
/// WHY 全クリップがインプレースである前提を持つか:
///   Assets/Models/Boss/README.md の契約。21 クリップすべて root motion = zero で、
///   移動・旋回・ジャンプの放物線はエンジン側の Root が担当する。だからこのスクリプトは
///   Animator に「今どれくらいの速さで動いているか」を教えるだけで、逆に Animator から
///   移動量をもらうことは一切しない (Animator の Root Motion Mode は None)。
///
/// WHY 速度を Transform の差分から取るか:
///   Boss の移動を最終的に何が駆動するか (RigidBody / CharacterController / AI の直書き)
///   はまだ決まっていない。ワールド位置の差分なら、そのどれになっても同じ値が出る。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/BossAnimParams.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

/// Inspector のプレビュー用。AI が使うものではない。
enum class BossMotionPreview : int {
    StompFrontRight = 0,
    StompFrontLeft,
    StompBackRight,
    StompBackLeft,
    Jump,
    MagneticPulse,
    Charge,
    ChargeCrash,
    Beam,
    Hit,
    Death,
};

class BossAnimatorComponent : public Script {
    FBZZ_SCRIPT(BossAnimatorComponent)

public:
    FBZZ_GROUP("Locomotion")
    FBZZ_FIELD(bool, autoDriveLocomotion, true, "Auto Drive")
    FBZZ_TOOLTIP("ワールド位置と向きの差分から Speed / Turn を毎フレーム流す。"
                 "AI が SetLocomotion() で明示的に与えるなら切る")
    FBZZ_FIELD_RANGE(float, turnFullRate, 45.0f, "Turn Full Rate", 5.0f, 180.0f)
    FBZZ_TOOLTIP("この角速度 (度/秒) で Turn が ±1 になる。0.5 を超えるとその場旋回モーションへ入る")
    FBZZ_FIELD_RANGE(float, motionDamping, 8.0f, "Motion Damping", 0.0f, 40.0f)
    FBZZ_TOOLTIP("Speed / Turn の平滑化。差分から取った値は 1 フレーム単位で跳ねるので必ず要る")

    FBZZ_GROUP("Aim Layer")
    FBZZ_FIELD(std::string, aimLayerName, "Aim", "Layer Name")
    FBZZ_TOOLTIP("Beam_Aim を上半身だけに重ねる Override レイヤー。照射中だけ立ち上げる")
    FBZZ_FIELD_RANGE(float, aimWeight, 1.0f, "Aim Weight", 0.0f, 1.0f)
    FBZZ_TOOLTIP("照射中に到達する重み。1 で上半身が完全に照準クリップへ移る")
    FBZZ_FIELD_RANGE(float, aimFadeIn, 0.25f, "Fade In", 0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, aimFadeOut, 0.35f, "Fade Out", 0.0f, 2.0f)

    FBZZ_GROUP("Preview")
    FBZZ_FIELD_ENUM(BossMotionPreview, previewMotion, BossMotionPreview::StompFrontRight,
                    "Motion",
                    "Stomp FR", "Stomp FL", "Stomp BR", "Stomp BL",
                    "Jump (大ジャンプ踏みつけ)", "Magnetic Pulse",
                    "Charge (突進)", "Charge → Crash (激突)",
                    "Beam (照射)", "Hit (被弾)", "Death (撃破)")
    FBZZ_TOOLTIP("AI が無くても 1 本ずつ見て確認するための仮再生。Play 中に押すこと "
                 "(滞空やループの打ち切りは OnUpdate が進める)。ゲーム側からは使わない")
    void PlayPreview();
    FBZZ_BUTTON(PlayPreview, "Play Preview")
    void StopPreview();
    FBZZ_BUTTON(StopPreview, "Stop / Revive")
    FBZZ_FIELD_RANGE(float, previewAirTime, 1.6f, "Air Time", 0.0f, 8.0f)
    FBZZ_TOOLTIP("Jump プレビューで FallIdle を回し続ける秒数。この後に自動で接地させる")
    FBZZ_FIELD_RANGE(float, previewHoldTime, 2.5f, "Hold Time", 0.0f, 12.0f)
    FBZZ_TOOLTIP("Charge / Beam プレビューでループを回し続ける秒数")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(std::string, debugState, "", "State")
    FBZZ_FIELD_READ_ONLY(float, debugSpeed, 0.0f, "Speed (m/s)")
    FBZZ_FIELD_READ_ONLY(float, debugTurn, 0.0f, "Turn (-1..1)")

    void OnStart()  override;
    void OnUpdate() override;

    // ── AI から呼ぶ入口 ─────────────────────────────────────────────────────

    /// Speed / Turn を明示的に与える。Auto Drive を切っているときに使う。
    /// yawRate は度/秒で、正が左回り。
    void SetLocomotion(float speedMetersPerSecond, float yawRateDegrees);

    /// 単脚の踏みつけ。ダメージ判定は f23–26 (0.77–0.87 秒)。
    void Stomp(BossLeg leg);

    /// 大ジャンプの踏み切り。JumpUp → FallIdle (滞空) と繋がる。
    /// 上向き初速を与えるのは JumpUp の f33 = 1.07 秒後。
    /// WHY 接地を同時に落とすか: Grounded を true のまま跳ぶと、JumpUp が終わった
    ///     次のフレームに FallIdle → Land が成立して、滞空せずに着地モーションへ落ちる。
    void Jump();

    /// 接地状態。false の間 FallIdle を回し、true にした瞬間 Land へ入る。
    /// Land の接地・衝撃フレームは f22 (0.70 秒) なので、Root を地面高さへ合わせるのはそこ。
    void SetGrounded(bool grounded);

    /// 磁力パルス。パルス発生は f24–29 (0.80–0.97 秒)。
    void Pulse();

    /// 突進の溜め → ギャロップ。溜め切りは Charge_Windup の f38 (1.27 秒)。
    void BeginCharge();
    /// 突進を正常に終える。Locomotion へ戻る。
    void EndCharge();
    /// 突進が壁などへ激突して転倒する。5 秒間の隙 (Crash_Stun) に入る。
    void Crash();

    /// ビームの構え → 照射。EndBeam() を呼ぶまで Beam_Loop を回し続ける。
    void BeginBeam();
    void EndBeam();

    /// 被弾リアクション。加算レイヤーへ 1 発差し込む。
    void ReactToHit();

    /// 撃破。Death は崩れたまま最終フレームで止まり、Idle へは戻らない。
    void SetDead(bool dead);

    /// 今の移動速度 [m/s]。歩容を選ぶのに使った «均した後» の値。
    ///
    /// WHY 公開するか: 足音も同じ速さで歩容 (巡回クロール / 突進クロール) を選ぶ。
    ///     音の側が生の速度から選び直すと、平滑化の分だけ境目がずれ、脚が巡回の
    ///     動きをしているのに音だけ突進へ切り替わるフレームができる。
    [[nodiscard]] float Speed() const { return m_speed; }

    [[nodiscard]] bool IsDead()     const { return m_dead; }
    [[nodiscard]] bool IsBeaming()  const { return m_beaming; }
    [[nodiscard]] bool IsCharging() const { return m_charging; }
    [[nodiscard]] bool IsGrounded() const { return m_grounded; }
    /// Locomotion / その場旋回のいずれでもない = 何かのモーション中。
    [[nodiscard]] bool IsBusy() const;

private:
    /// ワールド位置と向きの差分から Speed / Turn を求める。
    void SampleLocomotion(float dt);
    /// 照射中かどうかに追従して Aim レイヤーの重みを動かす。
    void UpdateAimLayer(float dt);
    /// プレビューの後始末 (滞空の打ち切り・ループの停止) を進める。
    void UpdatePreview(float dt);

    /// 前フレームのワールド位置と平面上の前方向。初回は差分を取らない。
    Vector3 m_lastPosition = Vector3::ZERO;
    Vector3 m_lastForward  = Vector3::FORWARD;
    bool    m_hasLastPose  = false;

    float m_speed = 0.0f;
    float m_turn  = 0.0f;

    bool  m_beaming     = false;
    bool  m_charging    = false;
    bool  m_dead        = false;
    bool  m_grounded    = true;
    float m_currentAimWeight = 0.0f;

    /// プレビューの残り秒数と、時間切れで何をするか。負なら動いていない。
    enum class PreviewPending : int { None = 0, Land, EndCharge, Crash, EndBeam };
    float          m_previewRemaining = -1.0f;
    PreviewPending m_previewPending   = PreviewPending::None;
};

FBZZ_REFLECT(BossAnimatorComponent)


inline void BossAnimatorComponent::OnStart()
{
    m_hasLastPose      = false;
    m_speed            = 0.0f;
    m_turn             = 0.0f;
    m_beaming          = false;
    m_charging         = false;
    m_dead             = false;
    m_grounded         = true;
    m_currentAimWeight = 0.0f;
    m_previewRemaining = -1.0f;
    m_previewPending   = PreviewPending::None;

    // Play をまたぐと Animator は Controller の初期値へ戻る。こちらの真偽値と
    // 食い違ったまま始まると、「立ち上がりだけ照射している」ような状態が残る。
    animator.SetBool(bossanim::kGrounded, true);
    animator.SetBool(bossanim::kCharging, false);
    animator.SetBool(bossanim::kBeaming,  false);
    animator.SetBool(bossanim::kIsDead,   false);
    animator.SetLayerWeight(aimLayerName, 0.0f);
}

inline void BossAnimatorComponent::OnUpdate()
{
    const float dt = std::max(Time::deltaTime, 0.0f);

    if (autoDriveLocomotion) SampleLocomotion(dt);
    UpdatePreview(dt);
    UpdateAimLayer(dt);

    animator.SetFloat(bossanim::kSpeed, m_speed);
    animator.SetFloat(bossanim::kTurn,  m_turn);

    debugState = animator.GetCurrentState();
    debugSpeed = m_speed;
    debugTurn  = m_turn;
}

inline void BossAnimatorComponent::SampleLocomotion(float dt)
{
    const Vector3 position = transform.worldPosition;
    const Vector3 facing   = transform.worldRotation * Vector3::FORWARD;
    // 真上・真下を向いた姿勢では平面成分が消える。Normalized() は長さ 0 で assert
    // するので、前フレームの向きへ落として «回っていない» 扱いにする。
    const Vector3 forward  = Vector3{ facing.x, 0.0f, facing.z }.NormalizedOr(m_lastForward);

    if (!m_hasLastPose || dt <= 0.0f) {
        m_lastPosition = position;
        m_lastForward  = forward;
        m_hasLastPose  = true;
        return;
    }

    const Vector3 delta{ position.x - m_lastPosition.x, 0.0f, position.z - m_lastPosition.z };
    const float   speed = delta.Length() / dt;

    // 平面上の符号付き回転角。Cross の y 成分が回転方向をそのまま持っている。
    const float   cross = Vector3::Cross(m_lastForward, forward).y;
    const float   dot   = std::clamp(Vector3::Dot(m_lastForward, forward), -1.0f, 1.0f);
    const float   yawRate = std::atan2(cross, dot) * RAD2DEG / dt;

    m_lastPosition = position;
    m_lastForward  = forward;

    // 差分から取った値は 1 フレーム単位で跳ねる。指数補間で均さないと、止まっている
    // はずの Boss が Idle と Walk の間で震え続ける。
    const float alpha = 1.0f - std::exp(-std::max(motionDamping, 0.0f) * dt);
    m_speed += (speed - m_speed) * std::clamp(alpha, 0.0f, 1.0f);
    m_turn  += (std::clamp(yawRate / std::max(turnFullRate, 1.0f), -1.0f, 1.0f) - m_turn) *
               std::clamp(alpha, 0.0f, 1.0f);
}

inline void BossAnimatorComponent::UpdateAimLayer(float dt)
{
    const float target = m_beaming ? std::clamp(aimWeight, 0.0f, 1.0f) : 0.0f;
    const float fade   = m_beaming ? aimFadeIn : aimFadeOut;

    if (fade <= 0.0f) {
        m_currentAimWeight = target;
    } else {
        const float step = dt / fade;
        m_currentAimWeight += std::clamp(target - m_currentAimWeight, -step, step);
    }
    animator.SetLayerWeight(aimLayerName, m_currentAimWeight);
}

inline void BossAnimatorComponent::UpdatePreview(float dt)
{
    if (m_previewRemaining < 0.0f) return;

    m_previewRemaining -= dt;
    if (m_previewRemaining > 0.0f) return;

    const PreviewPending pending = m_previewPending;
    m_previewRemaining = -1.0f;
    m_previewPending   = PreviewPending::None;

    switch (pending) {
    case PreviewPending::Land:      SetGrounded(true); break;
    case PreviewPending::EndCharge: EndCharge();       break;
    case PreviewPending::Crash:     Crash();           break;
    case PreviewPending::EndBeam:   EndBeam();         break;
    case PreviewPending::None:      break;
    }
}

inline void BossAnimatorComponent::SetLocomotion(float speedMetersPerSecond, float yawRateDegrees)
{
    m_speed = std::max(speedMetersPerSecond, 0.0f);
    m_turn  = std::clamp(yawRateDegrees / std::max(turnFullRate, 1.0f), -1.0f, 1.0f);
}

inline void BossAnimatorComponent::Stomp(BossLeg leg)
{
    animator.SetInt(bossanim::kStompLeg, static_cast<int>(leg));
    animator.SetTrigger(bossanim::kStomp);
}

inline void BossAnimatorComponent::Jump()
{
    SetGrounded(false);
    animator.SetTrigger(bossanim::kJump);
}

inline void BossAnimatorComponent::SetGrounded(bool grounded)
{
    m_grounded = grounded;
    animator.SetBool(bossanim::kGrounded, grounded);
}

inline void BossAnimatorComponent::Pulse()
{
    animator.SetTrigger(bossanim::kPulse);
}

inline void BossAnimatorComponent::BeginCharge()
{
    m_charging = true;
    animator.SetBool(bossanim::kCharging, true);
    animator.SetTrigger(bossanim::kCharge);
}

inline void BossAnimatorComponent::EndCharge()
{
    m_charging = false;
    animator.SetBool(bossanim::kCharging, false);
}

inline void BossAnimatorComponent::Crash()
{
    // 激突した時点で突進は終わっている。Charging を残すと Crash_Stun を抜けた直後に
    // Charge_Run へ戻る経路は無いが、AI 側が「まだ突進中」と読む余地を残さない。
    EndCharge();
    animator.SetTrigger(bossanim::kCrash);
}

inline void BossAnimatorComponent::BeginBeam()
{
    m_beaming = true;
    animator.SetBool(bossanim::kBeaming, true);
    animator.SetTrigger(bossanim::kBeam);
}

inline void BossAnimatorComponent::EndBeam()
{
    m_beaming = false;
    animator.SetBool(bossanim::kBeaming, false);
}

inline void BossAnimatorComponent::ReactToHit()
{
    animator.SetTrigger(bossanim::kHit);
}

inline void BossAnimatorComponent::SetDead(bool dead)
{
    m_dead = dead;
    if (dead) {
        // 倒れる前に回っていたループを畳む。Beaming を残すと Death へ入った後も
        // 上半身の照準レイヤーが立ち上がったままになる。
        EndBeam();
        EndCharge();
    }
    animator.SetBool(bossanim::kIsDead, dead);
}

inline bool BossAnimatorComponent::IsBusy() const
{
    const std::string state = animator.GetCurrentState();
    return !state.empty() &&
           state != bossanim::kLocomotion &&
           state != bossanim::kTurnLeft &&
           state != bossanim::kTurnRight;
}

inline void BossAnimatorComponent::PlayPreview()
{
    m_previewRemaining = -1.0f;
    m_previewPending   = PreviewPending::None;

    switch (previewMotion) {
    case BossMotionPreview::StompFrontRight: Stomp(BossLeg::FrontRight); break;
    case BossMotionPreview::StompFrontLeft:  Stomp(BossLeg::FrontLeft);  break;
    case BossMotionPreview::StompBackRight:  Stomp(BossLeg::BackRight);  break;
    case BossMotionPreview::StompBackLeft:   Stomp(BossLeg::BackLeft);   break;
    case BossMotionPreview::MagneticPulse:   Pulse();                    break;
    case BossMotionPreview::Hit:             ReactToHit();               break;
    case BossMotionPreview::Death:           SetDead(true);              break;

    case BossMotionPreview::Jump:
        Jump();
        m_previewRemaining = std::max(previewAirTime, 0.0f);
        m_previewPending   = PreviewPending::Land;
        break;

    case BossMotionPreview::Charge:
        BeginCharge();
        m_previewRemaining = std::max(previewHoldTime, 0.0f);
        m_previewPending   = PreviewPending::EndCharge;
        break;

    case BossMotionPreview::ChargeCrash:
        BeginCharge();
        m_previewRemaining = std::max(previewHoldTime, 0.0f);
        m_previewPending   = PreviewPending::Crash;
        break;

    case BossMotionPreview::Beam:
        BeginBeam();
        m_previewRemaining = std::max(previewHoldTime, 0.0f);
        m_previewPending   = PreviewPending::EndBeam;
        break;
    }
}

inline void BossAnimatorComponent::StopPreview()
{
    m_previewRemaining = -1.0f;
    m_previewPending   = PreviewPending::None;
    EndBeam();
    EndCharge();
    SetGrounded(true);
    SetDead(false);
}

} // namespace sandbox
