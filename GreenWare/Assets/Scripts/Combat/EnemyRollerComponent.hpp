/// @file EnemyRollerComponent.hpp
/// @brief Enemy C「Roller」— 転がって突進する単輪ローラー (企画書 8)
/// @author Hasegawa Jin
/// @date 2026-08-24
///
/// WHY 突進を「予備動作 → 直進 → 硬直」の 3 段に割るか:
///   8 章は Roller を「重量級のため引力ではほとんど動かない」「他の敵を受け止める的」と
///   定義している。的である以上プレイヤーは Roller の近くに居続けることになるので、
///   突進が予告なく飛んでくると避けようがない。止まって溜める間を挟むことで、
///   「今なら組める / 今は逃げる」の判断がプレイヤー側に残る。
///
/// WHY 突進中に向きを変えないか:
///   曲がってくる突進は、避けたかどうかがプレイヤーから読めない。直線に固定すると、
///   横へ抜ければ必ず避けられる代わりに、避けた先が壁だと詰む、という位置の読み合いになる。
///   8 章がボスの突進で「突進中は方向転換できない」と決めているのと同じ理由。
///
/// WHY 引力で動かないのに PolarityTarget を持つか:
///   7.2 の持続 12 秒は「先に極を置いておける」ための値で、Roller はそれ自体が
///   集束の中心になる。動かない側は PolarityBodyComponent を «付けないこと» で宣言する
///   (PolarityBodyComponent の設計意図)。このスクリプトは動かないことを前提に書く。
///
/// WHY 壁に刺さったら余計に硬直させるか:
///   直進しか出来ない突進は «横へ抜ければ避けられる» が売りなのに、避けた結果が
///   「壁の前で 0.3 秒止まって向き直る」だけだと、避けても避けなくても盤面が変わらない。
///   避けさせた先に壁があることをプレイヤーが利用できて初めて、8 章が言う «位置の
///   読み合い» になる。避けた側の報酬は、壁へ突き刺さった重量級が起き上がるまでの間。
#pragma once

#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/EnemyAiBase.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/BodyBounds.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class EnemyRollerComponent : public EnemyAiBase {
    FBZZ_SCRIPT_DERIVED(EnemyRollerComponent, EnemyAiBase)

public:
    FBZZ_GROUP("Approach")
    FBZZ_FIELD_RANGE(float, chargeRange, 9.0f, "Charge Range", 1.0f, 30.0f)
    FBZZ_TOOLTIP("この距離まで詰めたら突進を始める。遠すぎると当たらず、近すぎると避けられない")
    FBZZ_FIELD_RANGE(float, keepDistance, 2.2f, "Keep Distance", 0.0f, 10.0f)
    FBZZ_TOOLTIP("突進が空いていないときに保つ間合い。密着したまま押し続けない")

    FBZZ_GROUP("Charge")
    FBZZ_FIELD_RANGE(float, telegraphSeconds, 0.45f, "Telegraph", 0.0f, 3.0f)
    FBZZ_TOOLTIP("踏み込む前に止まって溜める時間。ここが予兆になる")
    FBZZ_FIELD_RANGE(float, chargeSpeed, 11.0f, "Charge Speed", 1.0f, 40.0f)
    FBZZ_FIELD_RANGE(float, chargeSeconds, 0.75f, "Charge Seconds", 0.05f, 4.0f)
    FBZZ_TOOLTIP("直進している時間。Charge Speed × これが突進距離になる")
    FBZZ_FIELD_RANGE(float, recoverSeconds, 0.30f, "Recover", 0.0f, 3.0f)
    FBZZ_TOOLTIP("突進後に止まっている時間。プレイヤーが組み立てに使える隙")
    FBZZ_FIELD_RANGE(float, hitRadius, 1.6f, "Hit Radius", 0.2f, 8.0f)
    FBZZ_TOOLTIP("突進中にこの距離まで近づいたら轢いたことにする")

    FBZZ_GROUP("Slam")
    FBZZ_FIELD_RANGE(float, slamSpeedRatio, 0.35f, "Blocked Ratio", 0.05f, 1.0f)
    FBZZ_TOOLTIP("1 ステップで進むはずの距離のこの割合しか動けなければ «刺さった» と見なす")
    FBZZ_FIELD_RANGE(float, slamRecoverScale, 2.4f, "Recover Scale", 1.0f, 6.0f)
    FBZZ_TOOLTIP("壁に刺さった後の硬直を Recover の何倍にするか。避けた側の取り分")
    FBZZ_FIELD_RANGE(float, slamShake, 0.6f, "Shake", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, slamShakeRange, 16.0f, "Shake Range", 1.0f, 60.0f)
    FBZZ_TOOLTIP("この距離まで離れると揺れが 0 になる。画面外の激突で手元を揺らさない")

    void OnFixedUpdate() override;

protected:
    void OnEnemyStart() override;

    [[nodiscard]] const se::Bank* MoveVoiceBank()    const override
    { return &se::kRollerRollLoop; }
    [[nodiscard]] const se::Bank* DestroyVoiceBank() const override
    { return &se::kRollerDestroy; }
    /// 転がりの «全開» は突進の速さ。追跡速度を基準にすると、突進中ずっと振り切れる。
    [[nodiscard]] float VoiceSpeedReference() const override { return chargeSpeed; }

private:
    /// 突進の 3 段階。Chase 以外は途中で中断しない。
    enum class Phase : int { Chase = 0, Telegraph, Charge, Recover };

    void TickTelegraph(float dt);
    void TickCharge(float dt);
    void TickRecover(float dt);
    void BeginTelegraph();
    void BeginCharge();
    /// 進めなくなった突進を打ち切り、激突として鳴らす。
    void Slam();
    void Chase(float dt);

    Phase   m_phase      = Phase::Chase;
    float   m_timer      = 0.0f;
    /// 踏み込んだ瞬間に固定した突進方向。以後は変えない。
    Vector3 m_chargeDir  = Vector3::ZERO;
    bool    m_chargeHit  = false;
    /// 前の固定ステップの位置。突進が «進めているか» はここの差でしか分からない。
    Vector3 m_lastPosition = Vector3::ZERO;
    int     m_chargeSteps  = 0;
    /// 今の硬直が激突によるものか。調整中に «長い方の隙» を見分けるために出す。
    bool    m_slammed      = false;
};

FBZZ_REFLECT(EnemyRollerComponent)


inline void EnemyRollerComponent::OnEnemyStart()
{
    // 車輪が回って見えるのは Move / Attack クリップの Wheel ボーンで、剛体の回転ではない。
    // 物理に転がされると進行方向と車輪の向きが食い違う。
    physics.SetFreezeRotation(true, true, true);

    m_phase        = Phase::Chase;
    m_timer        = 0.0f;
    m_chargeDir    = Vector3::ZERO;
    m_chargeHit    = false;
    m_chargeSteps  = 0;
    m_slammed      = false;
    m_lastPosition = transform.worldPosition;
}

inline void EnemyRollerComponent::OnFixedUpdate()
{
    const float dt = time.FixedDeltaTime();

    if (!IsAlive()) {
        debugState = "Dead";
        m_phase    = Phase::Chase;
        m_slammed  = false;
        StopHorizontal();
        return;
    }

    switch (m_phase) {
    case Phase::Telegraph: TickTelegraph(dt); return;
    case Phase::Charge:    TickCharge(dt);    return;
    case Phase::Recover:   TickRecover(dt);   return;
    case Phase::Chase:     break;
    }

    // 突進の 3 段階に入る前だけ、被弾硬直や引力で止める。踏み込んだ後に止めると、
    // 予兆を出しておきながら何も来ないという最悪の読ませ方になる。
    if (IsMovementLocked()) {
        debugState = "Locked";
        StopHorizontal();
        return;
    }
    Chase(dt);
}

inline void EnemyRollerComponent::Chase(float dt)
{
    GameObject* player = Player();
    if (!player) {
        debugState = "No Player";
        StopHorizontal();
        return;
    }

    Vector3 direction = player->transform.worldPosition - transform.worldPosition;
    direction.y = 0.0f;
    const float distanceSq = direction.LengthSq();
    if (distanceSq < EPSILON) {
        debugState = "Overlap";
        StopHorizontal();
        return;
    }

    direction = direction.Normalized();
    FaceDirection(direction, dt);

    if (AttackReady() && distanceSq <= chargeRange * chargeRange) {
        BeginTelegraph();
        return;
    }

    if (distanceSq <= keepDistance * keepDistance) {
        debugState = "Hold";
        StopHorizontal();
        return;
    }

    Vector3 velocity = physics.GetVelocity();
    velocity.x = direction.x * std::max(moveSpeed, 0.0f);
    velocity.z = direction.z * std::max(moveSpeed, 0.0f);
    physics.SetVelocity(velocity);
    debugState = "Rolling";
}

inline void EnemyRollerComponent::BeginTelegraph()
{
    m_phase     = Phase::Telegraph;
    m_timer     = std::max(telegraphSeconds, 0.0f);
    m_chargeHit = false;
    debugState  = "Telegraph";

    // クールダウンは踏み込みではなく «構えた» 時点から数える。避けられて空振りしても
    // 間合いに居るかぎり即座に構え直す、という張り付きにならない。
    BeginAttackCooldown();
    animator.SetTrigger(enemyanim::kAttack);
    // 踏ん張って地面を噛む音。予兆は «止まった» という絵だけでは弱く、
    // 視線が別の敵へ向いている間はまず伝わらない。
    se::Play(audio, se::kRollerAnchor);
}

inline void EnemyRollerComponent::BeginCharge()
{
    m_phase        = Phase::Charge;
    m_timer        = std::max(chargeSeconds, 0.05f);
    m_chargeSteps  = 0;
    m_lastPosition = transform.worldPosition;
    se::Play(audio, se::kRollerCharge);
}

inline void EnemyRollerComponent::TickTelegraph(float dt)
{
    debugState = "Telegraph";
    StopHorizontal();

    // 溜めている間だけは向き直る。ここで狙いを定め切る。
    if (GameObject* player = Player()) {
        Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
        toPlayer.y = 0.0f;
        FaceDirection(toPlayer, dt);
        m_chargeDir = toPlayer.NormalizedOr(m_chargeDir);
    }

    m_timer -= dt;
    if (m_timer > 0.0f) return;

    if (m_chargeDir.LengthSq() < EPSILON) {
        // 狙う先が無いまま踏み込んでも意味が無い。構えだけで終わらせる。
        m_phase = Phase::Recover;
        m_timer = std::max(recoverSeconds, 0.0f);
        return;
    }

    BeginCharge();
}

inline void EnemyRollerComponent::TickCharge(float dt)
{
    debugState = "Charge";

    // WHY 進んだ距離で見るか (速度ではなく): 壁へ押し当てている間もこちらは毎ステップ
    //     速度を書き続けるので、GetVelocity は «出したい速さ» を返しうる。実際に
    //     体が動いたかどうかは位置の差にしか出ない。
    const Vector3 position = transform.worldPosition;
    Vector3       moved    = position - m_lastPosition;
    moved.y        = 0.0f;
    m_lastPosition = position;
    ++m_chargeSteps;

    // 最初の数ステップは «構えていた場所からの差» でしかなく、まだ動けていなくて当然。
    const float expected = std::max(chargeSpeed, 0.0f) * dt;
    if (m_chargeSteps >= 3 && expected > EPSILON &&
        moved.Length() < expected * Clamp01(slamSpeedRatio)) {
        Slam();
        return;
    }

    Vector3 velocity = physics.GetVelocity();
    velocity.x = m_chargeDir.x * std::max(chargeSpeed, 0.0f);
    velocity.z = m_chargeDir.z * std::max(chargeSpeed, 0.0f);
    physics.SetVelocity(velocity);

    // WHY 距離で轢くか: OnCollisionEnter は接触解決の順序によっては高速ですれ違った
    //     フレームを取りこぼす。轢かれたかどうかが «たまに» 変わると、避けたのか
    //     判定が抜けたのかプレイヤーには区別できない。
    if (!m_chargeHit) {
        if (GameObject* player = Player()) {
            Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
            toPlayer.y = 0.0f;
            if (toPlayer.LengthSq() <= hitRadius * hitRadius) {
                m_chargeHit = true;
                (void)HitPlayer(player);
            }
        }
    }

    m_timer -= dt;
    if (m_timer > 0.0f) return;

    m_phase = Phase::Recover;
    m_timer = std::max(recoverSeconds, 0.0f);
}

inline void EnemyRollerComponent::Slam()
{
    debugState = "Slam";
    StopHorizontal();
    m_phase   = Phase::Recover;
    m_slammed = true;
    m_timer   = std::max(recoverSeconds, 0.0f) * std::max(slamRecoverScale, 1.0f);

    // どこへ当たったかは取らない。轢く判定と同じで «止まった» ことさえ伝わればよく、
    // 面を取りに行くと壁・柱・他の敵で経路が分かれる。体の前面で鳴らす。
    GameObject*   self  = scene.Self();
    const Vector3 point = (self ? bodybounds::CenterWorld(*self, 1.0f)
                                : transform.worldPosition) + m_chargeDir * hitRadius;
    se::PlayAt(audio, se::kImpactWall, point);

    Polarity polarity = Polarity::None;
    if (const auto* target = scene.GetScript<PolarityTargetComponent>())
        polarity = target->Current();
    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayImpact(point, polarity, 1.0f, true);

    // WHY 手触りマネージャーを通さないか: あちらの配分にはヒットストップが入っている。
    //     これはプレイヤーが «避けきった» 瞬間なので、そこで操作を止めると
    //     せっかく作った隙の頭を自分で削ることになる。揺れだけを直に鳴らす。
    if (auto* shake = CameraShakeManagerComponent::Instance()) {
        float proximity = 1.0f;
        if (const GameObject* player = Player()) {
            const float distance = (player->transform.worldPosition - point).Length();
            proximity = Clamp01(1.0f - distance / std::max(slamShakeRange, 1.0f));
        }
        shake->Shake(std::max(slamShake, 0.0f) * proximity);
    }
}

inline void EnemyRollerComponent::TickRecover(float dt)
{
    debugState = m_slammed ? "Slam Recover" : "Recover";
    StopHorizontal();

    m_timer -= dt;
    if (m_timer > 0.0f) return;

    m_phase   = Phase::Chase;
    m_timer   = 0.0f;
    m_slammed = false;
}

} // namespace sandbox
