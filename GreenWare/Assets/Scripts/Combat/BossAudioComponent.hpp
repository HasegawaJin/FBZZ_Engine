/// @file    BossAudioComponent.hpp
/// @brief   Boss「ポラリティ・コア」の SE 駆動。AI から音への唯一の入口
/// @author  Hasegawa Jin
/// @date    2026-08-27
///
/// WHY AI と音の間に 1 枚挟むか:
///   BossAnimatorComponent が Animator に対してそうしているのと同じ理由。ボスの素材は
///   35 ファイルあり、しかも 1 つの行動が «予備動作 → 着弾 → 減衰 → 硬直 → 引き抜き» の
///   ように 5 本のファイルへ割れている。AI が se::Play を直に並べると、行動を 1 つ
///   足すたびに «どのファイルを何秒後に» という表が AI の中へ散らばる。
///   行動 1 つを関数 1 つに閉じて、ファイル名も間隔もこのファイルの外へ出さない。
///
/// WHY 間隔を秒ではなくフレームで書くか:
///   素材 (Assets/Sound/SE/README) は 30fps のフレーム番号にそのまま乗る長さで
///   書き出してあり、踏みつけの «F16–20 は完全な無音» のように、間そのものが
///   設計されている。秒へ均してしまうと、その «置かれた無音» が誰かの丸め方次第で
///   埋まる。表の側の単位のまま持って、使う直前に秒へ直す。
///
/// WHY 鳴り続ける音を LoopVoice で持つか:
///   AudioSourceComponent の主 voice は 1 本しかなく、その volume と pitch は
///   PlayOneShot にもそのまま掛かる (LoopVoice.hpp)。サーボの定常音を速さで絞ると、
///   同じ体から出る着弾音や被弾音まで一緒に小さくなる。土台 (サーボ) / 照射 / スタン /
///   突進は互いに重なりうるので、それぞれ別の口から出す。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/BossAnimatorComponent.hpp>
#include <Scripts/Combat/BossPolarityCoreComponent.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Utils/LoopVoice.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

// WHY 入れ子の namespace へ畳まないか:
//   ScriptCodeGen は FBZZ_SCRIPT を見つけたとき、そのファイルで最後に開いている
//   namespace をスクリプトの所属として ScriptList.inl へ書く。ここに namespace を
//   1 枚挟むと ::bossse::BossAudioComponent という存在しない型が登録され、
//   «スクリプトを 1 つ足しただけで DLL がコンパイルできない» という壊れ方をする。
//   まとめたいなら BossAnimParams.hpp のように FBZZ_SCRIPT の無い別ファイルへ置くこと。

/// 素材の表は 30fps で書かれている。
inline constexpr float kBossSeFps = 30.0f;
[[nodiscard]] inline constexpr float BossFrameSeconds(float frames)
{ return frames / kBossSeFps; }

// 踏みつけ 60F の表 (素材 README)。着弾 F23 を基準に、他の 3 点は比で付いてくる。
//
// WHY F16–20 に何も置かないか:
//   予備動作のファイルは F16 で音が切れるように書き出してあり、そこから着弾までの
//   0.13 秒は «完全静止» が予兆そのものになっている。埋めるための音を足さないこと。
inline constexpr float kBossStompRaiseFrame  = 4.0f;
inline constexpr float kBossStompImpactFrame = 23.0f;
inline constexpr float kBossStompHoldFrame   = 35.0f;
inline constexpr float kBossStompPullFrame   = 46.0f;

/// 巡回クロールの 1 周期 (40F) と突進クロールの 1 周期 (30F)。
/// 素材 1 ファイルがちょうどこの 1 周期ぶんなので、間隔もこれで刻む。
inline constexpr float kBossWalkCycleFrames   = 40.0f;
inline constexpr float kBossChargeCycleFrames = 30.0f;

/// 切替の «瞬間» から磁力パルスまでの間 (素材 README)。
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
    FBZZ_FIELD_RANGE(float, servoIdleVolume, 0.45f, "Idle", 0.0f, 1.0f)
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

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(float, debugSpeed, 0.0f, "Speed (m/s)")
    FBZZ_FIELD_READ_ONLY(int, debugStompCue, -1, "Stomp Cue")

    void OnStart()  override;
    void OnUpdate() override;
    // 無効化で土台と照射が鳴りっぱなしになるのを防ぐ。
    void OnDisable() override { SilenceLoops(); }


    /// 降下してきた登場。4 本の脚が 0.06 秒ずつずれて接地するところまで 1 本に入っている。
    void Appear();

    /// 踏みつけの 60F 表を頭から流す。
    /// @param hitSeconds AI が持つ着弾時刻 [秒]。表の F23 がここへ来るよう全体を伸縮させる。
    ///
    /// WHY 着弾に合わせて表ごと伸縮させるか: 予備動作・無音・着弾・硬直の比は
    ///     «予備動作より着弾が 13dB 大きい» という設計とセットで作られている。
    ///     着弾だけ AI の時刻へ寄せて他を固定すると、無音の 0.13 秒が着弾の後ろへ
    ///     回り込んで予兆にならない。
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

    /// 磁力パルス。極の切替そのものは OnUpdate が拾うので、ここは «撃った» だけを言う。
    void MagneticPulse();

    /// 大ジャンプの着地。
    void JumpLand();

private:
    /// サーボの土台と足音を速さから駆動する。
    void DriveLocomotion(float dt);
    /// 踏みつけの表を 1 つずつ消化する。
    void TickStomp(float dt);
    /// 極の切替・予告・フェーズ移行を BossPolarityCoreComponent から拾う。
    void TickPolarity(float dt);
    /// スタンの復帰と磁力パルスの遅延を進める。
    void TickPending(float dt);
    /// 鳴り続けている音を全部畳む。倒れたときと無効化のとき。
    void SilenceLoops();

    /// 今の移動速度 [m/s]。Animator が均した値があればそれを使う。
    [[nodiscard]] float Speed() const;
    [[nodiscard]] bool  IsAlive() const;

    [[nodiscard]] BossPolarityCoreComponent* Core() const
    { return scene.GetScript<BossPolarityCoreComponent>(); }

    // WHY 4 本に分けるか: 土台・突進・照射・スタンは互いに重なりうる。
    //     突進からそのまま激突すればクロールとスタンが 1 フレーム重なるし、
    //     照射中もサーボは鳴り続けている。1 本で回すと、後から来た方が前を切る。
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

    /// 前フレームの極とフェーズ。切替と移行は «変わった瞬間» にしか鳴らさない。
    Polarity m_lastPolarity = Polarity::None;
    int      m_lastPhase    = 1;
    /// 今の周期で予告を鳴らしたか。1 周期に 1 度だけ。
    bool     m_warnedThisCycle = false;

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
    m_warnedThisCycle   = false;
    m_charging          = false;
    m_silenced          = false;

    // ボスは盤面の «どこかに» 居る。距離と方向が読める 3D で鳴らす。
    se::EnsureSource(scene, "SE", 1.0f);

    // 鳴り続ける 4 本は別々の子から出る。キーが被ると音源を奪い合う。
    m_servo.SetKey("BossServo");
    m_run.SetKey("BossRun");
    m_beam.SetKey("BossBeam");
    m_stun.SetKey("BossStun");

    // 被弾と撃破は EnemyHealthComponent が鳴らす経路を既に持っている。ここは
    // «ボスのときはこの束» を預けるだけにして、鳴らす場所を 2 つに増やさない
    // («鳴らす側» ではなく «束を預ける側» になる)。
    if (auto* health = scene.GetScript<EnemyHealthComponent>()) {
        health->SetFlinchVoice(&se::kBossDamaged);
        health->SetDestroyVoice(&se::kBossDestroy);
    } else {
        debug.LogError("BossAudioComponent requires an EnemyHealthComponent on the same object "
                       "(damage and destruction sounds are played through it).");
    }

    if (const auto* core = Core()) {
        m_lastPolarity = core->CurrentPolarity();
        m_lastPhase    = core->CurrentPhase();
    } else {
        debug.LogError("BossAudioComponent requires a BossPolarityCoreComponent on the same "
                       "object (polarity switch cues are read from it).");
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
    // Animator は同じ速さから歩容を選んでいる。そこから取れば、足音と脚の動きが
    // 同じ 1 つの値で決まり、Walk と Charge の境目で音だけ先に切り替わることがない。
    if (const auto* anim = scene.GetScript<BossAnimatorComponent>()) return anim->Speed();

    const Vector3 velocity = physics.GetVelocity();
    return Vector3{ velocity.x, 0.0f, velocity.z }.Length();
}

inline void BossAudioComponent::OnUpdate()
{
    const float dt = std::max(Time::deltaTime, 0.0f);

    if (!IsAlive()) {
        // 崩れ落ちている最中にサーボが回り続けていると «まだ動いている» に聞こえる。
        // 撃破音そのものは EnemyHealthComponent が場所へ残して鳴らす。
        SilenceLoops();
        m_stompCue    = -1;
        debugStompCue = -1;
        return;
    }
    m_silenced = false;

    DriveLocomotion(dt);
    TickStomp(dt);
    TickPolarity(dt);
    TickPending(dt);
}

inline void BossAudioComponent::DriveLocomotion(float dt)
{
    const float speed = Speed();
    debugSpeed = speed;

    // 土台は止まっていても鳴らし続ける。無音まで落とすと、待ち構えているボスが
    // 耳から消えて、振り向いた瞬間に湧いたように見える。
    const float speed01 = Clamp01(speed / std::max(chargeStepSpeed, 0.01f));
    // WHY ピッチを振らないか: 巡回と突進で «常に 3 本接地» の重なり方そのものが
    //     違う音として別に用意されている。1 本を伸縮させて速さを表すと、素材が
    //     作り分けている歩容の違いと二重になる。速さは音量だけで伝える。
    m_servo.Update(*this, se::kBossServoLoop.First(),
                   Lerp(servoIdleVolume, servoMoveVolume, speed01));

    // 突進中はクロールのループが接地を持っている。同時に単発の足音も刻むと、
    // 1 周期に接地が 8 回あることになって歩容が崩れる。
    if (m_charging || speed < stepMinSpeed) {
        // 貯めた残りは捨てる。残すと、止まって歩き出した 1 歩目が周期を待たずに鳴る。
        m_stepRemaining = 0.0f;
        return;
    }

    const bool  fast   = speed >= chargeStepSpeed;
    const float period = BossFrameSeconds(fast ? kBossChargeCycleFrames
                                              : kBossWalkCycleFrames);

    m_stepRemaining -= dt;
    if (m_stepRemaining > 0.0f) return;

    // 束は抽選せず順に送る。01→04 が 4 周期ぶんの歩容として書かれている。
    se::PlayNext(audio, fast ? se::kBossStepCharge : se::kBossStepWalk, stepVolume);
    m_stepRemaining = period;
}

inline void BossAudioComponent::BeginStomp(float hitSeconds)
{
    m_stompCue  = 0;
    m_stompTime = 0.0f;

    // 表の F23 が AI の着弾時刻に重なるよう、全体をこの比で伸縮させる。
    const float tableHit = BossFrameSeconds(kBossStompImpactFrame);
    m_stompScale = tableHit > 0.0f ? std::max(hitSeconds, 0.0f) / tableHit : 1.0f;
    if (m_stompScale <= 0.0f) m_stompScale = 1.0f;

    debugStompCue = m_stompCue;
}

inline void BossAudioComponent::TickStomp(float dt)
{
    if (m_stompCue < 0) return;
    m_stompTime += dt;

    // 1 フレームで 2 つ以上跨いだ場合 (低フレームレート) も取りこぼさない。
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
            // 着弾と、そこから始まる跳ね返りの減衰振動。減衰の側は着弾の «後» では
            // なく «同時» に始まる (F23–35 が 1 本のファイルになっている)。
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
    // 薙ぎは土台に «重ねる» 層。床の継ぎ目を横切るたびに床が鳴る側だけが入っている。
    //
    // WHY 再生速度を振らないか: 素材 README は «2 回薙ぐなら 1.1 倍ほど変えて重ねる»
    //     と書いているが、PlayOneShot に速度の口は無く、AudioSource の pitch は
    //     同じ口から出る他の一発ものにも掛かる。1 回の照射で 1 回しか薙がない
    //     今の AI では同じ音が続かないので、専用の音源を足してまで振らない。
    se::Play(audio, se::kBossBeamSweep);
}

inline void BossAudioComponent::EndBeam()
{
    BeamFiring(false);
    se::Play(audio, se::kBossBeamEnd);
}

inline void BossAudioComponent::MagneticPulse()
{
    // 切替の音とパルスの衝撃波を «同時» に出すと、切り替わったことと撃たれたことが
    // 1 つの音塊になって、どちらが起きたのか読めない。素材 README の 20ms だけずらす。
    m_pulseDelay = kBossMagPulseDelay;
}

inline void BossAudioComponent::JumpLand()
{
    // WHY 踏みつけと同じ着弾音か: 大ジャンプ専用の素材は無く、鳴っているのは
    //     どちらも «脚が地面を貫いた» という同じ出来事。違うのは «4 本同時» という
    //     重さで、そこは着地の衝撃波と揺れ (BossAiComponent の landRumble) が担当する。
    se::Play(audio, se::kBossStompImpact);
    se::Play(audio, se::kBossStompSettle);
}

inline void BossAudioComponent::TickPolarity(float dt)
{
    (void)dt;
    const auto* core = Core();
    if (!core) return;

    // 10.6 の «HP50% でフェーズが上がる»。上がった瞬間に 1 度だけ。
    const int phase = core->CurrentPhase();
    if (phase > m_lastPhase) se::Play(audio, se::kBossPhaseShift);
    m_lastPhase = phase;

    const Polarity now = core->CurrentPolarity();

    // 切替の予告。終端が切替の瞬間に合うよう書かれているので、残り時間が
    // 予告の長さを切った «その瞬間» に流す。リングの明滅と同じしきい値を読むので、
    // 見えている予兆と聞こえている予兆が必ず一致する。
    const float remaining = core->PolaritySwitchRemaining();
    const float lead      = std::max(core->switchWarnSeconds, 0.0f);
    if (!m_warnedThisCycle && lead > 0.0f && remaining > 0.0f && remaining <= lead &&
        now != Polarity::None) {
        m_warnedThisCycle = true;
        se::Play(audio, se::kBossPolSwitchWarn);
    }
    // 周期が入れ替わったら予告を張り直す。残り時間が予告の長さより戻った時点が
    // 新しい周期の始まり。
    if (remaining > lead) m_warnedThisCycle = false;

    // 極が入れ替わった «瞬間»。硬直中の消灯 (None) は切替ではないので、
    // None を挟んだ往復では鳴らさない。
    if (now != m_lastPolarity && now != Polarity::None && m_lastPolarity != Polarity::None)
        se::Play(audio, se::BossPolSwitch(now));
    m_lastPolarity = now;
}

inline void BossAudioComponent::TickPending(float dt)
{
    if (m_pulseDelay >= 0.0f) {
        m_pulseDelay -= dt;
        if (m_pulseDelay < 0.0f) se::Play(audio, se::kBossMagPulse);
    }

    if (m_stunRemaining < 0.0f) return;
    m_stunRemaining -= dt;

    // 復帰は明ける «前» に流し始める。明けてから鳴らすと、立ち上がったボスの背中で
    // 復帰音が鳴り、反撃機会が終わった合図として遅れて届く。
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
