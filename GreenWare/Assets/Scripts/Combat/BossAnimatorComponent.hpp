/// @file    BossAnimatorComponent.hpp
/// @brief   Boss「ポラリティ・コア」のアニメーション駆動。AI から Animator への唯一の入口
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// @note AI と Animator の間に挟む理由。14 個のパラメーター名を AI に直接叩かせると、
///       綴りミスは黙って無視され、対のパラメーターの片方だけ書き忘れても気付けない。
///       行動 1 つを関数 1 つに閉じ、パラメーター名はこのファイルの外へ出さない。
/// @note 全 21 クリップは root motion = zero が前提 (Assets/Models/Boss/README.md)。
///       移動・旋回はエンジン側の Root が担当するため、速度は Transform の位置差分から
///       取る (駆動方式が RigidBody / CharacterController のどれでも同じ値になる)。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Utils/RagdollPresentation.hpp>
#include <Scripts/Combat/BossAnimParams.hpp>
#include <Scripts/Game/HitstopManagerComponent.hpp>
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
    FBZZ_GROUP("ロコモーション")
    FBZZ_FIELD(bool, autoDriveLocomotion, true, "Auto Drive")
    FBZZ_TOOLTIP("ワールド位置と向きの差分から Speed / Turn を毎フレーム流す。"
                 "AI が SetLocomotion() で明示的に与えるなら切る")
    FBZZ_FIELD_RANGE(float, turnFullRate, 45.0f, "Turn Full Rate", 5.0f, 180.0f)
    FBZZ_TOOLTIP("この角速度 (度/秒) で Turn が ±1 になる。0.5 を超えるとその場旋回モーションへ入る")
    FBZZ_FIELD_RANGE(float, motionDamping, 8.0f, "Motion Damping", 0.0f, 40.0f)
    FBZZ_TOOLTIP("Speed / Turn の平滑化。差分から取った値は 1 フレーム単位で跳ねるので必ず要る")

    FBZZ_GROUP("Aim Layer")
    FBZZ_FIELD(std::string, aimLayerName, "Aim", "レイヤー名")
    FBZZ_TOOLTIP("Beam_Aim を上半身だけに重ねる Override レイヤー。照射中だけ立ち上げる")
    FBZZ_FIELD_RANGE(float, aimWeight, 1.0f, "Aim Weight", 0.0f, 1.0f)
    FBZZ_TOOLTIP("照射中に到達する重み。1 で上半身が完全に照準クリップへ移る")
    FBZZ_FIELD_RANGE(float, aimFadeIn, 0.25f, "フェードイン", 0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, aimFadeOut, 0.35f, "フェードアウト", 0.0f, 2.0f)

    /// @note 深さは重みで持つ。毎段満額で鳴らすと同じ深さの仰け反りが連続して «痙攣» に
    ///       見えるため、軽い一撃は重みを下げて «触れた» に留め、締めだけ満額にする。
    FBZZ_GROUP("Hit Layer")
    FBZZ_FIELD(std::string, hitLayerName, "Add_Hit", "レイヤー名")
    FBZZ_TOOLTIP("Hit_Add を加算する Additive レイヤー。Trigger «Hit» で 1 発鳴る")
    FBZZ_FIELD_RANGE(float, hitLightWeight, 0.40f, "Light Weight", 0.0f, 1.0f)
    FBZZ_TOOLTIP("strength 0 の被弾でのレイヤー重み。1 で全段が締めと同じ深さになる")

    FBZZ_GROUP("プレビュー")
    FBZZ_FIELD_ENUM(BossMotionPreview, previewMotion, BossMotionPreview::StompFrontRight,
                    "動き",
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
    FBZZ_FIELD_RANGE(float, previewAirTime, 1.6f, "滞空時間", 0.0f, 8.0f)
    FBZZ_TOOLTIP("Jump プレビューで FallIdle を回し続ける秒数。この後に自動で接地させる")
    FBZZ_FIELD_RANGE(float, previewHoldTime, 2.5f, "Hold Time", 0.0f, 12.0f)
    FBZZ_TOOLTIP("Charge / Beam プレビューでループを回し続ける秒数")

    /// @note 再生速度は実速に比例させる。クリップは 1.0 固定で歩調がしきい値 2 m/s 基準の
    ///       ため、速く動かすと脚は同じ速さで掻くのに体だけ滑る。実速 ÷ 想定速度で追従させる。
    /// @note 対象は走りと突進だけ。踏みつけ・照射・跳躍は当たる時刻をクリップのフレームと
    ///       秒数で合わせてある (README の f23 等) ため、速度を変えると判定と絵がずれる。
    FBZZ_GROUP("再生速度")
    FBZZ_FIELD(bool, scaleClipToSpeed, true, "速さに再生を比例させる")
    FBZZ_TOOLTIP("走り / 突進の再生速度を «実速 ÷ 想定速度» にする。"
                 "切ると 1.0 固定 (速く動かすほど足が滑る)")
    FBZZ_FIELD_RANGE(float, walkClipSpeed, 2.0f, "Walk_Crawl の実速 [m/s]", 0.2f, 20.0f)
    FBZZ_TOOLTIP("Walk_Crawl を 1.0 倍で再生したときに足が滑らない速さ。"
                 "Boss.animcontroller のブレンドしきい値 (2.0) と同じ値にしてある")
    FBZZ_FIELD_RANGE(float, chargeClipSpeed, 5.0f, "Charge_Run の実速 [m/s]", 0.5f, 30.0f)
    FBZZ_TOOLTIP("Charge_Run を 1.0 倍で再生したときに足が滑らない速さ。"
                 "BossAiComponent の突進速度をこれで割った値が再生速度になる")
    FBZZ_FIELD_RANGE(float, clipSpeedMin, 0.35f, "再生の下限", 0.05f, 1.0f)
    FBZZ_FIELD_RANGE(float, clipSpeedMax, 2.80f, "再生の上限", 1.0f, 6.0f)
    FBZZ_TOOLTIP("上限を下げると «滑らない» を捨てて «脚の速さが自然» を採ることになる。"
                 "突進 13 m/s ÷ 5 m/s = 2.6 なので、2.6 以上で完全に追従する")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugState, "", "状態")
    FBZZ_FIELD_READ_ONLY(float, debugSpeed, 0.0f, "速さ (m/s)")
    FBZZ_FIELD_READ_ONLY(float, debugTurn, 0.0f, "Turn (-1..1)")
    FBZZ_FIELD_READ_ONLY(float, debugClipSpeed, 1.0f, "再生速度")

    void OnStart()  override;
    void OnUpdate() override;

    /// @name AI から呼ぶ入口
    /// @{

    /// Speed / Turn を明示的に与える。Auto Drive を切っているときに使う。
    /// yawRate は度/秒で、正が左回り。
    void SetLocomotion(float speedMetersPerSecond, float yawRateDegrees);

    /// 単脚の踏みつけ。ダメージ判定は f23–26 (0.77–0.87 秒)。
    void Stomp(BossLeg leg);

    /// 大ジャンプの踏み切り。JumpUp → FallIdle (滞空) と繋がる。
    /// 上向き初速を与えるのは JumpUp の f33 = 1.07 秒後。
    /// @note 接地も同時に落とす。Grounded を true のまま跳ぶと、JumpUp が終わった
    ///       次のフレームに FallIdle → Land が成立して滞空せず着地モーションへ落ちる。
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

    /// 被弾リアクション。加算レイヤーへ 1 発差し込む。strength は一撃の重さ [0,1] で、
    /// レイヤーの重み (Light Weight 〜 1) になる。
    /// @note Add_Hit の Hit ステートは自己遷移しない (AnimatorSystem の自己遷移スキップ) ため、
    ///       鳴っている最中の Trigger は HitIdle へ戻った直後に後回しで 1 発として出る。
    void ReactToHit(float strength = 1.0f);

    /// 撃破。Death は崩れたまま最終フレームで止まり、Idle へは戻らない。
    void SetDead(bool dead);

    /// 盤面のテンポ (BossAiComponent の Tempo) を預ける。
    /// @note 再生速度は 1 つしか無く、足の運びを実速へ合わせる値と 2 か所から書くと
    ///       後から書いた方が相手を消す。掛け合わせる場所をここ 1 つに決める。
    void SetTempo(float tempo) { m_tempo = std::max(tempo, 0.0f); }

    /// 今の移動速度 [m/s]。歩容を選ぶのに使った «均した後» の値。
    /// @note 足音も同じ値で歩容を選ぶ。音の側が生の速度から選び直すと平滑化の分だけ境目が
    ///       ずれ、脚は巡回のまま音だけ突進へ切り替わるフレームができる。
    [[nodiscard]] float Speed() const { return m_speed; }

    [[nodiscard]] bool IsDead()     const { return m_dead; }
    [[nodiscard]] bool IsBeaming()  const { return m_beaming; }
    [[nodiscard]] bool IsCharging() const { return m_charging; }
    [[nodiscard]] bool IsGrounded() const { return m_grounded; }
    /// Locomotion / その場旋回のいずれでもない = 何かのモーション中。
    [[nodiscard]] bool IsBusy() const;
    /// @}

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
    /// 預かっている盤面のテンポ。再生速度は これ × 足の運びの比。
    float m_tempo = 1.0f;

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

    /// @note Play をまたぐと Animator は Controller の初期値へ戻る。こちらの真偽値と
    ///       食い違ったまま始まると、「立ち上がりだけ照射している」ような状態が残る。
    animator.SetBool(bossanim::kGrounded, true);
    animator.SetBool(bossanim::kCharging, false);
    animator.SetBool(bossanim::kBeaming,  false);
    animator.SetBool(bossanim::kIsDead,   false);
    animator.SetLayerWeight(aimLayerName, 0.0f);
    /// @note 被弾レイヤーは満額から。前の Play が軽い一撃で終わっていても、最初の被弾が浅くならない。
    if (!hitLayerName.empty()) animator.SetLayerWeight(hitLayerName, 1.0f);
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

    /// @note 対象は «走っている» 状態だけ。突進は Locomotion ではない別のステートなので、
    ///       走り (IsBusy() が false) と突進 (m_charging) の 2 つを明示的に採る。
    float scale = 1.0f;
    /// @note 止まっているときに比を掛けると Idle の呼吸まで遅くなる。
    ///       動いていない間は素の 1.0 に戻す。
    if (scaleClipToSpeed && (m_charging || !IsBusy()) && m_speed > 0.2f) {
        const float reference = std::max(m_charging ? chargeClipSpeed : walkClipSpeed, 0.1f);
        scale = std::clamp(m_speed / reference,
                           std::max(clipSpeedMin, 0.01f), std::max(clipSpeedMax, 0.01f));
    }
    /// @note 当事者の凍結 (ヒットストップの手応え) が掛かっている間は書かない。凍結は
    ///       «再生速度を 0 にする» で作られているため、毎フレーム書き続けると次のフレームで
    ///       解けて丸ごと効かなくなる。秒数を数えるのは固めた側の仕事、ここは状態だけ聞く。
    const auto* stop = HitstopManagerComponent::Instance();
    if (stop && stop->IsAnimationFrozen(scene.Self())) {
        debugClipSpeed = 0.0f;
        return;
    }

    /// @note テンポと足の運びの比を掛け合わせるのはここ 1 か所に閉じる。再生速度は 1 つしか
    ///       無く、別々に書くと後から書いた方が相手を消すため。トグル off でもここが書くので
    ///       テンポが «黙って 1.0 に戻る» 経路は残らない。
    animator.SetSpeed(m_tempo * scale);
    debugClipSpeed = scale;
}

inline void BossAnimatorComponent::SampleLocomotion(float dt)
{
    const Vector3 position = transform.worldPosition;
    const Vector3 facing   = transform.worldRotation * Vector3::FORWARD;
    /// @note 真上・真下を向いた姿勢では平面成分が消える。Normalized() は長さ 0 で assert
    ///       するので、前フレームの向きへ落として «回っていない» 扱いにする。
    const Vector3 forward  = Vector3{ facing.x, 0.0f, facing.z }.NormalizedOr(m_lastForward);

    if (!m_hasLastPose || dt <= 0.0f) {
        m_lastPosition = position;
        m_lastForward  = forward;
        m_hasLastPose  = true;
        return;
    }

    const Vector3 delta{ position.x - m_lastPosition.x, 0.0f, position.z - m_lastPosition.z };
    const float   speed = delta.Length() / dt;

    /// @note 平面上の符号付き回転角。Cross の y 成分が回転方向をそのまま持っている。
    const float   cross = Vector3::Cross(m_lastForward, forward).y;
    const float   dot   = std::clamp(Vector3::Dot(m_lastForward, forward), -1.0f, 1.0f);
    const float   yawRate = std::atan2(cross, dot) * RAD2DEG / dt;

    m_lastPosition = position;
    m_lastForward  = forward;

    /// @note 差分から取った値は 1 フレーム単位で跳ねる。指数補間で均さないと、止まっている
    ///       はずの Boss が Idle と Walk の間で震え続ける。
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
    /// @note 激突した時点で突進は終わっている。Charging を残すと Crash_Stun を抜けた直後に
    ///       Charge_Run へ戻る経路は無いが、AI 側が「まだ突進中」と読む余地を残さない。
    EndCharge();
    /// @note スタンのゲーム状態はAIが保持する。見た目は立ったまま衝撃をこらえる。
    ReactToHit(1.0f);
    ragdoll.BeginActive();
    PushRagdollReaction(ragdoll, -(transform.worldRotation * Vector3::FORWARD) * 3.0f, 1.5f);
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

inline void BossAnimatorComponent::ReactToHit(float strength)
{
    /// @note 重みは «次の 1 発» の深さ。鳴り終わりまで持ち越すので、軽い一撃の直後に
    ///       締めが来れば深く、逆なら浅くなる (同時には鳴らないので取り合いは起きない)。
    if (!hitLayerName.empty())
        animator.SetLayerWeight(hitLayerName,
                                Lerp(std::clamp(hitLightWeight, 0.0f, 1.0f), 1.0f,
                                     std::clamp(strength, 0.0f, 1.0f)));
    animator.SetTrigger(bossanim::kHit);
}

inline void BossAnimatorComponent::SetDead(bool dead)
{
    m_dead = dead;
    if (dead) {
        /// @note 倒れる前に回っていたループを畳む。Beaming を残すと Death へ入った後も
        ///       上半身の照準レイヤーが立ち上がったままになる。
        EndBeam();
        EndCharge();
    }
    /// @note 撃破後も倒れない演出。死亡と消滅の進行は m_dead とゲーム側に残す。
    animator.SetBool(bossanim::kIsDead, false);
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
