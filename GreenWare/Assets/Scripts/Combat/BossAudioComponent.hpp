/// @file    BossAudioComponent.hpp
/// @brief   Boss「ポラリティ・コア」の SE 駆動。AI から音への唯一の入口
/// @author  Hasegawa Jin
/// @date    2026-08-27
///
/// @note AI と音の間に 1 枚挟む (BossAnimatorComponent と同じ理由)。1 つの行動が
///       5 本のファイルへ割れており、AI が直に並べるとファイル名と間隔の表が散らばる。
/// @note 間隔は秒でなくフレームで持つ。素材は 30fps のフレーム番号そのままの長さで
///       書き出してあり (README)、秒へ均すと «置かれた無音» が丸め方次第で埋まる。
/// @note 鳴り続ける音は LoopVoice で持つ。主 voice は 1 本で volume/pitch が
///       PlayOneShot にも掛かるため、土台・照射・スタン・突進を別の口から出す。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/BossAnimatorComponent.hpp>
#include <Scripts/Combat/BossCoreComponent.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Utils/LoopVoice.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

/// @note 入れ子の namespace へ畳まない。ScriptCodeGen はファイルで最後に開いている
///       namespace をスクリプトの所属として ScriptList.inl へ書くため、1 枚挟むと
///       存在しない型が登録され DLL がコンパイルできなくなる (BossAnimParams.hpp のように
///       FBZZ_SCRIPT の無い別ファイルへ逃がすこと)。

/// 素材の表は 30fps で書かれている。
inline constexpr float kBossSeFps = 30.0f;
[[nodiscard]] inline constexpr float BossFrameSeconds(float frames)
{ return frames / kBossSeFps; }

/// 踏みつけ 60F の表 (素材 README)。着弾 F23 を基準に、他の 3 点は比で付いてくる。
/// @note F16–20 には何も置かない。予備動作は F16 で音が切れ、着弾までの 0.13 秒の
///       «完全静止» が予兆そのものになっているため、埋める音を足さない。
inline constexpr float kBossStompRaiseFrame  = 4.0f;
inline constexpr float kBossStompImpactFrame = 23.0f;
inline constexpr float kBossStompHoldFrame   = 35.0f;
inline constexpr float kBossStompPullFrame   = 46.0f;

/// 巡回クロールの 1 周期 (40F) と突進クロールの 1 周期 (30F)。
/// 素材 1 ファイルがちょうどこの 1 周期ぶんなので、間隔もこれで刻む。
inline constexpr float kBossWalkCycleFrames   = 40.0f;
inline constexpr float kBossChargeCycleFrames = 30.0f;

/// 予備動作から磁力パルスまでの間 (素材 README)。
inline constexpr float kBossMagPulseDelay = 0.020f;

/// 復帰音の長さ。スタンが明ける «前» に流し始めないと、立ち上がってから復帰音が鳴る。
inline constexpr float kBossStunRecoverSeconds = 1.40f;

class BossAudioComponent : public Script {
    FBZZ_SCRIPT(BossAudioComponent)

public:
    FBZZ_GROUP("Appearance")
    FBZZ_FIELD(bool, playAppearOnStart, true, "Play Appear")
    FBZZ_TOOLTIP("開始と同時に登場音 (5 秒) を鳴らす。既に盤面に立っている配置では切る")

    FBZZ_GROUP("Servo Bed")
    FBZZ_TOOLTIP("常時鳴らす土台。これがあると «そこに居る» が姿を見る前に届く")
    FBZZ_FIELD_RANGE(float, servoIdleVolume, 0.45f, "待機", 0.0f, 1.0f)
    FBZZ_TOOLTIP("停止中の音量。素材 README の «停止中は 0.4〜0.5 で敷く»")
    FBZZ_FIELD_RANGE(float, servoMoveVolume, 0.90f, "Moving", 0.0f, 1.0f)
    FBZZ_TOOLTIP("全速で動いているときの音量")

    FBZZ_GROUP("Footfall")
    FBZZ_FIELD_RANGE(float, stepVolume, 1.0f, "Volume", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, stepMinSpeed, 0.35f, "Step Above (m/s)", 0.0f, 5.0f)
    FBZZ_TOOLTIP("これ以下の速さでは足を運んでいない扱いにする。止まった瞬間に刻みも止まる")
    FBZZ_FIELD_RANGE(float, chargeStepSpeed, 3.8f, "Fast Crawl Above (m/s)", 0.5f, 20.0f)
    FBZZ_TOOLTIP("これを超えたら突進クロールの束と 30F 周期へ切り替える。"
                 "巡回 2.4 と突進 5.0 の間に置く")

    FBZZ_GROUP("Layer Levels")
    FBZZ_FIELD_RANGE(float, chargeRunVolume, 1.0f, "Charge Run", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, beamLoopVolume, 0.85f, "Beam Loop", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, stunLoopVolume, 0.70f, "Stun Loop", 0.0f, 1.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(float, debugSpeed, 0.0f, "速さ (m/s)")
    FBZZ_FIELD_READ_ONLY(int, debugStompCue, -1, "Stomp Cue")

    void OnStart()  override;
    void OnUpdate() override;
    /// 無効化で土台と照射が鳴りっぱなしになるのを防ぐ。
    void OnDisable() override { SilenceLoops(); }


    /// 降下してきた登場。4 本の脚が 0.06 秒ずつずれて接地するところまで 1 本に入っている。
    void Appear();

    /// 踏みつけの 60F 表を頭から流す。
    /// @param hitSeconds AI が持つ着弾時刻 [秒]。表の F23 がここへ来るよう全体を伸縮させる。
    /// @note 表ごと伸縮させる。着弾だけ AI の時刻へ寄せて他を固定すると、無音の 0.13 秒が
    ///       着弾の後ろへ回り込んで予兆にならない。
    void BeginStomp(float hitSeconds);

    /// 突進の溜め (45F)。
    void BeginCharge();
    /// 突進中のクロール (20F ループ)。走っている間だけ true。
    void ChargeRunning(bool running);
    /// 壁への激突とそれに続くスタン。
    /// @param stunSeconds スタンの長さ [秒]。明ける手前で復帰音を流し始める。
    void Crash(float stunSeconds);

    /// ビームの構え (アパーチャが開く → 充電 → アーク)。
    void BeginBeam();
    /// 地面を焼き続けている土台。照射中だけ true。
    void BeamFiring(bool firing);
    /// 旋回して薙ぐ層。Loop に重ねる。薙ぎ始めに 1 回。
    void BeamSweep();
    /// 芯が落ちて絞りが閉じるまで。
    void EndBeam();

    /// 磁力パルス。
    void MagneticPulse();

    /// 大ジャンプの着地。
    void JumpLand();

private:
    /// サーボの土台と足音を速さから駆動する。
    void DriveLocomotion(float dt);
    /// 踏みつけの表を 1 つずつ消化する。
    void TickStomp(float dt);
    /// フェーズ移行を BossCoreComponent から拾う。
    void TickPhase(float dt);
    /// スタンの復帰と磁力パルスの遅延を進める。
    void TickPending(float dt);
    /// 鳴り続けている音を全部畳む。倒れたときと無効化のとき。
    void SilenceLoops();

    /// 今の移動速度 [m/s]。Animator が均した値があればそれを使う。
    [[nodiscard]] float Speed() const;
    [[nodiscard]] bool  IsAlive() const;

    [[nodiscard]] BossCoreComponent* Core() const
    { return scene.GetScript<BossCoreComponent>(); }

    /// @note 4 本に分ける。土台・突進・照射・スタンは互いに重なりうる (突進から激突で
    ///       クロールとスタンが 1 フレーム重なる等)。1 本で回すと後から来た方が前を切る。
    se::LoopVoice m_servo;
    se::LoopVoice m_run;
    se::LoopVoice m_beam;
    se::LoopVoice m_stun;

    /// 次の接地までの残り [秒]。負なら «今すぐ»。
    float m_stepRemaining = 0.0f;

    /// 踏みつけの表の進み。-1 は «動いていない»。
    int   m_stompCue   = -1;
    float m_stompTime  = 0.0f;
    /// 表全体の伸縮率。AI の着弾時刻 ÷ 表の F23。
    float m_stompScale = 1.0f;

    /// 磁力パルスまでの残り。負なら予約されていない。
    float m_pulseDelay = -1.0f;
    /// スタンが明けるまでの残り。復帰音を流す時刻を測るためだけに持つ。
    float m_stunRemaining = -1.0f;
    bool  m_stunRecoverPlayed = false;

    /// 前フレームのフェーズ。移行は «変わった瞬間» にしか鳴らさない。
    int      m_lastPhase    = 1;

    /// 突進のクロールが鳴っている間。単発の足音を止めるためだけに持つ。
    bool m_charging = false;
    bool m_silenced = false;
};

FBZZ_REFLECT(BossAudioComponent)


inline void BossAudioComponent::OnStart()
{
    m_stepRemaining     = 0.0f;
    m_stompCue          = -1;
    m_stompTime         = 0.0f;
    m_stompScale        = 1.0f;
    m_pulseDelay        = -1.0f;
    m_stunRemaining     = -1.0f;
    m_stunRecoverPlayed = false;
    m_charging          = false;
    m_silenced          = false;

    /// @note ボスは盤面の «どこかに» 居る。距離と方向が読める 3D で鳴らす。
    se::EnsureSource(scene, "SE", 1.0f);

    /// @note 鳴り続ける 4 本は別々の子から出る。キーが被ると音源を奪い合う。
    m_servo.SetKey("BossServo");
    m_run.SetKey("BossRun");
    m_beam.SetKey("BossBeam");
    m_stun.SetKey("BossStun");

    /// @note 被弾と撃破は EnemyHealthComponent が鳴らす経路を既に持っている。ここは
    ///       «ボスのときはこの束» を預けるだけにして、鳴らす場所を 2 つに増やさない
    ///       («鳴らす側» ではなく «束を預ける側» になる)。
    if (auto* health = scene.GetScript<EnemyHealthComponent>()) {
        health->SetFlinchVoice(&se::kBossDamaged);
        health->SetDestroyVoice(&se::kBossDestroy);
    } else {
        debug.LogError("BossAudioComponent requires an EnemyHealthComponent on the same object "
                       "(damage and destruction sounds are played through it).");
    }

    if (const auto* core = Core()) {
        m_lastPhase = core->CurrentPhase();
    } else {
        debug.LogError("BossAudioComponent requires a BossCoreComponent on the same "
                       "object (phase cues are read from it).");
    }

    if (playAppearOnStart) Appear();
}

inline bool BossAudioComponent::IsAlive() const
{
    const auto* health = scene.GetScript<EnemyHealthComponent>();
    return !health || health->IsAlive();
}

inline float BossAudioComponent::Speed() const
{
    /// @note Animator は同じ速さから歩容を選んでいる。そこから取れば、足音と脚の動きが
    ///       同じ 1 つの値で決まり、Walk と Charge の境目で音だけ先に切り替わることがない。
    if (const auto* anim = scene.GetScript<BossAnimatorComponent>()) return anim->Speed();

    const Vector3 velocity = physics.GetVelocity();
    return Vector3{ velocity.x, 0.0f, velocity.z }.Length();
}

inline void BossAudioComponent::OnUpdate()
{
    const float dt = std::max(Time::deltaTime, 0.0f);

    if (!IsAlive()) {
        /// @note 崩れ落ちている最中にサーボが回り続けていると «まだ動いている» に聞こえる。
        ///       撃破音そのものは EnemyHealthComponent が場所へ残して鳴らす。
        SilenceLoops();
        m_stompCue    = -1;
        debugStompCue = -1;
        return;
    }
    m_silenced = false;

    DriveLocomotion(dt);
    TickStomp(dt);
    TickPhase(dt);
    TickPending(dt);
}

inline void BossAudioComponent::DriveLocomotion(float dt)
{
    const float speed = Speed();
    debugSpeed = speed;

    /// @note 土台は止まっていても鳴らし続ける。無音まで落とすと、待ち構えているボスが
    ///       耳から消えて、振り向いた瞬間に湧いたように見える。
    const float speed01 = Clamp01(speed / std::max(chargeStepSpeed, 0.01f));
    /// @note ピッチは振らない。巡回と突進は «常に 3 本接地» の重なり方自体が別の音として
    ///       用意されており、1 本を伸縮させると歩容の作り分けと二重になる。
    m_servo.Update(*this, se::kBossServoLoop.First(),
                   Lerp(servoIdleVolume, servoMoveVolume, speed01));

    /// @note 突進中はクロールのループが接地を持っている。同時に単発の足音も刻むと、
    ///       1 周期に接地が 8 回あることになって歩容が崩れる。
    if (m_charging || speed < stepMinSpeed) {
        /// @note 貯めた残りは捨てる。残すと、止まって歩き出した 1 歩目が周期を待たずに鳴る。
        m_stepRemaining = 0.0f;
        return;
    }

    const bool  fast   = speed >= chargeStepSpeed;
    const float period = BossFrameSeconds(fast ? kBossChargeCycleFrames
                                              : kBossWalkCycleFrames);

    m_stepRemaining -= dt;
    if (m_stepRemaining > 0.0f) return;

    /// @note 束は抽選せず順に送る。01→04 が 4 周期ぶんの歩容として書かれている。
    se::PlayNext(audio, fast ? se::kBossStepCharge : se::kBossStepWalk, stepVolume);
    m_stepRemaining = period;
}

inline void BossAudioComponent::BeginStomp(float hitSeconds)
{
    m_stompCue  = 0;
    m_stompTime = 0.0f;

    /// @note 表の F23 が AI の着弾時刻に重なるよう、全体をこの比で伸縮させる。
    const float tableHit = BossFrameSeconds(kBossStompImpactFrame);
    m_stompScale = tableHit > 0.0f ? std::max(hitSeconds, 0.0f) / tableHit : 1.0f;
    if (m_stompScale <= 0.0f) m_stompScale = 1.0f;

    debugStompCue = m_stompCue;
}

inline void BossAudioComponent::TickStomp(float dt)
{
    if (m_stompCue < 0) return;
    m_stompTime += dt;

    /// @note 1 フレームで 2 つ以上跨いだ場合 (低フレームレート) も取りこぼさない。
    while (m_stompCue >= 0) {
        float frame = 0.0f;
        switch (m_stompCue) {
        case 0:  frame = kBossStompRaiseFrame;  break;
        case 1:  frame = kBossStompImpactFrame; break;
        case 2:  frame = kBossStompHoldFrame;   break;
        default: frame = kBossStompPullFrame;   break;
        }
        if (m_stompTime < BossFrameSeconds(frame) * m_stompScale) break;

        switch (m_stompCue) {
        case 0:
            se::Play(audio, se::kBossStompRaise);
            break;
        case 1:
            /// @note 着弾と、そこから始まる跳ね返りの減衰振動。減衰の側は着弾の «後» では
            ///       なく «同時» に始まる (F23–35 が 1 本のファイルになっている)。
            se::Play(audio, se::kBossStompImpact);
            se::Play(audio, se::kBossStompSettle);
            break;
        case 2:
            se::Play(audio, se::kBossStompHold);
            break;
        default:
            se::Play(audio, se::kBossStompPull);
            m_stompCue    = -1;
            debugStompCue = -1;
            return;
        }
        ++m_stompCue;
        debugStompCue = m_stompCue;
    }
}

inline void BossAudioComponent::Appear()
{
    se::Play(audio, se::kBossAppear);
}

inline void BossAudioComponent::BeginCharge()
{
    se::Play(audio, se::kBossChargeWindup);
}

inline void BossAudioComponent::ChargeRunning(bool running)
{
    m_charging = running;
    if (running) m_run.Update(*this, se::kBossChargeRun.First(), chargeRunVolume);
    else         m_run.Stop(*this);
}

inline void BossAudioComponent::Crash(float stunSeconds)
{
    m_charging = false;
    m_run.Stop(*this);

    se::Play(audio, se::kBossChargeCrash);

    m_stunRemaining     = std::max(stunSeconds, 0.0f);
    m_stunRecoverPlayed = false;
    m_stun.Update(*this, se::kBossStunLoop.First(), stunLoopVolume);
}

inline void BossAudioComponent::BeginBeam()
{
    se::Play(audio, se::kBossBeamCharge);
}

inline void BossAudioComponent::BeamFiring(bool firing)
{
    if (firing) m_beam.Update(*this, se::kBossBeamLoop.First(), beamLoopVolume);
    else        m_beam.Stop(*this);
}

inline void BossAudioComponent::BeamSweep()
{
    /// @note 薙ぎは土台に «重ねる» 層。床の継ぎ目を横切るたびに床が鳴る側だけが入っている。
    /// @note 再生速度は振らない。PlayOneShot に速度の口が無く AudioSource の pitch は
    ///       他の一発ものにも掛かるため、1 回しか薙がない今の AI では振らずに済ませる。
    se::Play(audio, se::kBossBeamSweep);
}

inline void BossAudioComponent::EndBeam()
{
    BeamFiring(false);
    se::Play(audio, se::kBossBeamEnd);
}

inline void BossAudioComponent::MagneticPulse()
{
    /// @note 切替の音とパルスの衝撃波を «同時» に出すと、切り替わったことと撃たれたことが
    ///       1 つの音塊になって、どちらが起きたのか読めない。素材 README の 20ms だけずらす。
    m_pulseDelay = kBossMagPulseDelay;
}

inline void BossAudioComponent::JumpLand()
{
    se::Play(audio, se::kBossLanding);
}

inline void BossAudioComponent::TickPhase(float dt)
{
    (void)dt;
    const auto* core = Core();
    if (!core) return;

    /// @note フェーズが上がった瞬間に 1 度だけ。
    const int phase = core->CurrentPhase();
    if (phase > m_lastPhase) se::Play(audio, se::kBossPhaseShift);
    m_lastPhase = phase;
}

inline void BossAudioComponent::TickPending(float dt)
{
    if (m_pulseDelay >= 0.0f) {
        m_pulseDelay -= dt;
        if (m_pulseDelay < 0.0f) se::Play(audio, se::kBossMagPulse);
    }

    if (m_stunRemaining < 0.0f) return;
    m_stunRemaining -= dt;

    /// @note 復帰は明ける «前» に流し始める。明けてから鳴らすと、立ち上がったボスの背中で
    ///       復帰音が鳴り、反撃機会が終わった合図として遅れて届く。
    if (!m_stunRecoverPlayed && m_stunRemaining <= kBossStunRecoverSeconds) {
        m_stunRecoverPlayed = true;
        m_stun.Stop(*this);
        se::Play(audio, se::kBossStunRecover);
    }
    if (m_stunRemaining <= 0.0f) {
        m_stunRemaining = -1.0f;
        m_stun.Stop(*this);
    }
}

inline void BossAudioComponent::SilenceLoops()
{
    if (m_silenced) return;
    m_silenced = true;

    m_servo.Stop(*this);
    m_run.Stop(*this);
    m_beam.Stop(*this);
    m_stun.Stop(*this);

    m_charging      = false;
    m_stunRemaining = -1.0f;
    m_pulseDelay    = -1.0f;
}

} // namespace sandbox
