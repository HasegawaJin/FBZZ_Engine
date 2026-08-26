/// @file BossAiComponent.hpp
/// @brief Boss「ポラリティ・コア」の行動選択と当たり判定 (企画書 8 章 / 10.6)
/// @author Hasegawa Jin
/// @date 2026-08-26
///
/// WHY 攻撃を距離で選ぶか:
///   8 章が「距離で役割を分けることで、重複を避けつつ AI の選択を単純にする」と決めている。
///   突進 18m 以上 / コアビーム 8〜18m / 踏みつけ 6m 以下、という表がそのまま実装になる。
///   確率で選ぶと、同じ間合いから何が来るか読めなくなり、避け方を覚える余地が消える。
///
/// WHY 当たり判定を «時刻» で出すか (物理の接触ではなく):
///   ボスのモーションはすべてインプレースで、脚も胴体もコライダーを持たない
///   (Assets/Models/Boss/README.md の契約)。接触で判定しようとすると脚 1 本ごとに
///   コライダーを足して回ることになり、しかも «踏みつけたのに当たらない» が
///   アニメーションの再生速度に依存して出る。README がフレーム単位で書いている
///   ダメージ判定の時刻を、そのまま秒として持つ方が再現する。
///
/// WHY BossAnimatorComponent を経由するか:
///   Animator のパラメーター名を知っているのはあちらだけ、という約束
///   (BossAnimatorComponent.hpp の冒頭)。ここは «何をするか» を決めるだけで、
///   どのパラメーターを叩くかは知らない。
#pragma once

#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/BossAnimatorComponent.hpp>
#include <Scripts/Combat/BossBeamComponent.hpp>
#include <Scripts/Combat/BossHitboxRigComponent.hpp>
#include <Scripts/Combat/BossPolarityCoreComponent.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Polarity/PolarityBodyComponent.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossAiComponent : public Script {
    FBZZ_SCRIPT(BossAiComponent)

    // 巡回も突進も速度で動かす。インプレースのモーションに合わせて Root を運ぶのは物理側。
    FBZZ_REQUIRE_COMPONENT(RigidBodyComponent)

public:
    FBZZ_GROUP("Target")
    FBZZ_FIELD_TAG(playerTag, "Player", "Player Tag")

    FBZZ_GROUP("Locomotion")
    FBZZ_FIELD_RANGE(float, patrolSpeed, 2.4f, "Patrol Speed", 0.0f, 12.0f)
    FBZZ_TOOLTIP("8 章の巡回速度 2.4 m/s。Walk_Crawl の歩調はこの速さを想定している")
    FBZZ_FIELD_RANGE(float, turnSpeed, 45.0f, "Turn Speed", 5.0f, 360.0f)
    FBZZ_TOOLTIP("巡回中の旋回速度 [度/秒]。速すぎるとその場旋回モーションが出ない")
    FBZZ_FIELD_RANGE(float, keepDistance, 4.0f, "Keep Distance", 0.0f, 20.0f)
    FBZZ_TOOLTIP("これより近づいたら詰めるのをやめる。腹下へ潜られる余地を残す")

    FBZZ_GROUP("Attack Table (8章)")
    FBZZ_FIELD_RANGE(float, chargeMinRange, 18.0f, "Charge From", 5.0f, 60.0f)
    FBZZ_TOOLTIP("この距離以上なら突進。距離を詰めさせないための攻撃")
    FBZZ_FIELD_RANGE(float, beamMinRange, 8.0f, "Beam From", 2.0f, 40.0f)
    FBZZ_TOOLTIP("ここから Charge From までがコアビーム。移動を強制する")
    FBZZ_FIELD_RANGE(float, stompMaxRange, 6.0f, "Stomp Within", 1.0f, 20.0f)
    FBZZ_TOOLTIP("これ以下なら踏みつけ。腹下へ潜った罰")
    FBZZ_FIELD_RANGE(float, attackInterval, 3.0f, "Attack Interval", 0.2f, 20.0f)
    FBZZ_TOOLTIP("攻撃を出し終えてから次を選ぶまでの間。隙とは別に置く «呼吸»")

    FBZZ_GROUP("Stomp")
    FBZZ_FIELD_RANGE(float, stompHitTime, 0.80f, "Hit Time", 0.0f, 3.0f)
    FBZZ_TOOLTIP("README: 接地 f23 / 潰れ最下点 f25。判定はその間の 0.77〜0.87 秒")
    FBZZ_FIELD_RANGE(float, stompTotalTime, 2.00f, "Total", 0.1f, 6.0f)
    FBZZ_FIELD_RANGE(float, stompStaggerFrom, 1.17f, "Stagger From", 0.0f, 6.0f)
    FBZZ_TOOLTIP("README: f35–46 は足が地面に刺さったままの硬直。反撃を取らせる区間")
    FBZZ_FIELD_RANGE(float, stompReach, 4.6f, "Reach", 0.5f, 15.0f)
    FBZZ_TOOLTIP("胴体中心から踏みつける脚までの水平距離")
    FBZZ_FIELD_RANGE(float, stompRadius, 3.2f, "Shock Radius", 0.5f, 15.0f)
    FBZZ_FIELD_RANGE_INT(int, stompDamage, 2, "Damage", 0, 100)

    FBZZ_GROUP("Jump Stomp")
    FBZZ_TOOLTIP("企画書 8 章の攻撃表には無い 5 種目。踏みつけとビームの間の距離を埋める")
    FBZZ_FIELD_RANGE(float, jumpMinRange, 6.0f, "Jump From", 1.0f, 40.0f)
    FBZZ_TOOLTIP("この距離以上で跳ぶ。踏みつけの間合いから外へ逃げた相手を追う手段")
    FBZZ_FIELD_RANGE(float, jumpMaxTravel, 16.0f, "Max Travel", 2.0f, 40.0f)
    FBZZ_TOOLTIP("1 回で跳べる水平距離の上限。遠すぎる相手へは届かないまま落ちる")
    FBZZ_FIELD_RANGE(float, jumpTakeoffTime, 1.07f, "Takeoff", 0.0f, 4.0f)
    FBZZ_TOOLTIP("README: JumpUp の f33 で 4 脚が地面を離れる。ここまでは地上に居る")
    FBZZ_FIELD_RANGE(float, jumpAirTime, 1.40f, "Air Time", 0.2f, 8.0f)
    FBZZ_TOOLTIP("滞空秒数。FallIdle はループなので好きなだけ伸ばせる (README)")
    FBZZ_FIELD_RANGE(float, jumpArcHeight, 6.0f, "Arc Height", 0.5f, 30.0f)
    FBZZ_FIELD_RANGE(float, landContactTime, 0.70f, "Land Contact", 0.0f, 3.0f)
    FBZZ_TOOLTIP("README: Land の f22 で 4 脚が接地する。着地の «この秒数前» に Land を流し始める")
    FBZZ_FIELD_RANGE(float, landTotalTime, 2.60f, "Land Total", 0.2f, 8.0f)
    FBZZ_FIELD_RANGE(float, landStaggerFrom, 0.87f, "Land Stagger", 0.0f, 8.0f)
    FBZZ_TOOLTIP("README: f26 が潰れ最下点。そこから立ち直るまでが反撃機会")
    FBZZ_FIELD_RANGE(float, jumpHitRadius, 5.0f, "Shock Radius", 0.5f, 20.0f)
    FBZZ_TOOLTIP("着地の衝撃波。踏みつけより広いのが «大ジャンプ» の意味")
    FBZZ_FIELD_RANGE_INT(int, jumpDamage, 3, "Damage", 0, 100)

    FBZZ_GROUP("Charge")
    FBZZ_FIELD_RANGE(float, chargeWindupTime, 1.50f, "Windup", 0.1f, 6.0f)
    FBZZ_TOOLTIP("README: Charge_Windup は 45F。溜め切りは f38")
    FBZZ_FIELD_RANGE(float, chargeSpeed, 5.0f, "Speed", 1.0f, 20.0f)
    FBZZ_TOOLTIP("8 章の突進速度 5.0 m/s。Charge_Run の再生速度もこれに追随する")
    FBZZ_FIELD_RANGE(float, chargeMaxSeconds, 3.0f, "Max Seconds", 0.2f, 12.0f)
    FBZZ_TOOLTIP("壁に当たらなかった場合の打ち切り。当たらないまま走り続けさせない")
    FBZZ_FIELD_RANGE(float, chargeHitRadius, 3.0f, "Hit Radius", 0.5f, 12.0f)
    FBZZ_FIELD_RANGE_INT(int, chargeDamage, 3, "Damage", 0, 100)
    FBZZ_FIELD_RANGE(float, wallProbe, 4.0f, "Wall Probe", 0.5f, 20.0f)
    FBZZ_TOOLTIP("進行方向へこの距離を見て、塞がっていたら激突する")
    FBZZ_FIELD_RANGE(float, crashStunTime, 5.00f, "Crash Stun", 0.5f, 15.0f)
    FBZZ_TOOLTIP("README: Crash_Stun は 150F = 5 秒。プレイヤー最大の反撃機会")
    FBZZ_FIELD_RANGE_INT(int, crashSelfDamage, 200, "Self Damage", 0, 5000)
    FBZZ_TOOLTIP("8 章「ボス自身が地形に激突して大ダメージ」。雑魚 1 体の激突の何倍かで置く")

    FBZZ_GROUP("Beam")
    FBZZ_FIELD_RANGE(float, beamStartTime, 1.00f, "Start", 0.1f, 5.0f)
    FBZZ_FIELD_RANGE(float, beamSweepTime, 3.50f, "Sweep", 0.2f, 15.0f)
    FBZZ_FIELD_RANGE(float, beamEndTime, 0.80f, "End", 0.1f, 5.0f)
    FBZZ_FIELD_RANGE(float, beamSweepSpeed, 22.0f, "Sweep Speed", 1.0f, 180.0f)
    FBZZ_TOOLTIP("薙ぐ旋回速度 [度/秒]。走って追い越せる速さでないと «移動を強制する» にならない")
    FBZZ_FIELD_RANGE(float, beamLength, 20.0f, "Max Reach", 2.0f, 60.0f)
    FBZZ_TOOLTIP("接地点をボスから離せる上限 [m]。実際の距離はプレイヤーまでの距離に追従する")
    FBZZ_FIELD_RANGE(float, beamNearReach, 5.0f, "Min Reach", 1.0f, 30.0f)
    FBZZ_TOOLTIP("これより手前は焼かない。腹下は踏みつけの間合いなので譲る")
    FBZZ_FIELD_RANGE(float, beamRiseHeight, 3.2f, "Rise Height", 0.0f, 12.0f)
    FBZZ_TOOLTIP("薙ぎ終わりに終端を地面から持ち上げる高さ [m]。0 で地面を焼くだけ")
    // WHY 太さとダメージをここに持たないか: 判定するのは BossBeamComponent で、
    //     線を引いているのもあちら。同じ «線» の太さを 2 か所に置くと、
    //     見えている線と当たる線が黙って食い違う。

    FBZZ_GROUP("Magnetic Pulse")
    FBZZ_FIELD_RANGE(float, pulseHitTime, 0.85f, "Hit Time", 0.0f, 4.0f)
    FBZZ_TOOLTIP("README: パルス発生は f24–29 = 0.80〜0.97 秒")
    FBZZ_FIELD_RANGE(float, pulseTotalTime, 2.00f, "Total", 0.1f, 6.0f)
    FBZZ_FIELD_RANGE(float, pulseRadius, 22.0f, "Radius", 1.0f, 60.0f)
    FBZZ_TOOLTIP("8 章は «全域» なので、アリーナ半径 (実測 20m) を覆う値を既定にする")
    FBZZ_FIELD_RANGE(float, pulseKnockback, 9.0f, "Knockback", 0.0f, 40.0f)
    FBZZ_TOOLTIP("帯電中の雑魚を外向きへ弾く速さ。極性そのものは残す (8 章)")
    FBZZ_FIELD_RANGE_INT(int, pulseDamage, 1, "Damage", 0, 100)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(std::string, debugAct, "Idle", "Act")
    FBZZ_FIELD_READ_ONLY(float, debugDistance, 0.0f, "Distance")
    FBZZ_FIELD(bool, drawDebugRanges, false, "Draw Ranges")

    void OnStart() override;
    void OnFixedUpdate() override;
    void OnDrawGizmos() override;

private:
    /// 今出している行動。Idle 以外は途中で選び直さない。
    enum class Act : int {
        Idle = 0, Stomp, JumpUp, JumpAir, JumpLand,
        ChargeWindup, ChargeRun, CrashStun, Beam, Pulse
    };

    void TickIdle(float dt);
    void TickStomp(float dt);
    void TickJumpUp(float dt);
    void TickJumpAir(float dt);
    void TickJumpLand(float dt);
    void TickChargeWindup(float dt);
    void TickChargeRun(float dt);
    void TickCrashStun(float dt);
    void TickBeam(float dt);
    void TickPulse(float dt);

    void BeginStomp();
    void BeginJump();
    void BeginCharge();
    void BeginBeam();
    void BeginPulse();
    void BeginCrash();
    /// 終端をボスの正面へ置き直す。照射中は毎フレーム呼ぶ。
    /// @param sweep01 薙ぎの進み [0,1]。1 へ近づくほど終端を持ち上げる。
    void AimBeam(float sweep01);
    /// 行動を終えて Idle へ戻す。硬直と消灯も必ずここで解く。
    void EndAct();

    /// 8 章の距離テーブル。出せる攻撃が無ければ false。
    [[nodiscard]] bool SelectAttack();
    /// 踏みつける脚。プレイヤーが前後どちら側・左右どちら側に居るかで選ぶ。
    [[nodiscard]] BossLeg PickStompLeg(const Vector3& toPlayer) const;
    /// 踏みつけの着弾点。ヒットボックスのリグが居れば足ボーンの実座標を使う。
    [[nodiscard]] Vector3 StompPoint(BossLeg leg) const;

    [[nodiscard]] GameObject* Player() const { return m_player.Resolve(scene); }
    void RefreshPlayer();
    [[nodiscard]] bool IsAlive() const;
    [[nodiscard]] Vector3 Forward() const;
    /// 水平方向だけ direction へ向き直る。
    void FaceDirection(const Vector3& direction, float dt, float degreesPerSecond) const;
    /// 水平速度を 0 にする。落下は殺さない。
    void StopHorizontal() const;
    void MoveHorizontal(const Vector3& direction, float speed) const;
    /// プレイヤーへダメージを入れる。経路は CombatManager 1 本に通す。
    bool HitPlayer(int amount) const;
    /// 円内のプレイヤーを殴る。踏みつけ・パルスの衝撃波が共有する。
    bool HitPlayerInSphere(const Vector3& center, float radius, int amount) const;

    [[nodiscard]] BossAnimatorComponent*      Anim() const;
    [[nodiscard]] BossPolarityCoreComponent*  Core() const;
    [[nodiscard]] BossBeamComponent*          Beam() const;

    EntityRef m_player;
    Act       m_act        = Act::Idle;
    float     m_timer      = 0.0f;
    float     m_cooldown   = 0.0f;
    /// 今の行動でダメージ判定を出したか。1 回の振りで 1 回だけ当てる。
    bool      m_dealt      = false;
    /// 踏み込んだ瞬間に固定した突進方向。以後は変えない (8 章)。
    Vector3   m_chargeDir  = Vector3::FORWARD;
    BossLeg   m_stompLeg   = BossLeg::FrontRight;
    /// 大ジャンプの離陸点と着地点。踏み切った瞬間に確定させる。
    Vector3   m_jumpStart  = Vector3::ZERO;
    Vector3   m_jumpTarget = Vector3::ZERO;
    /// Land を流し始めたか。接地の landContactTime 前に 1 度だけ流す。
    bool      m_landCued   = false;
    /// 中距離で跳ぶかビームか。同じ間合いから同じ手しか来ないと読み合いにならない。
    bool      m_preferJump = false;
    /// ビームの段 (0 = 構え / 1 = 照射 / 2 = 終わり)。
    int       m_beamStage  = 0;
    /// 「戦闘が居ない」を 1 度だけ言うためのラッチ。報告は const な当て所からも起きる。
    mutable bool m_warnedNoCombat = false;
};

FBZZ_REFLECT(BossAiComponent)


inline BossAnimatorComponent* BossAiComponent::Anim() const
{
    return scene.GetScript<BossAnimatorComponent>();
}

inline BossPolarityCoreComponent* BossAiComponent::Core() const
{
    return scene.GetScript<BossPolarityCoreComponent>();
}

inline BossBeamComponent* BossAiComponent::Beam() const
{
    return scene.GetScript<BossBeamComponent>();
}

inline void BossAiComponent::OnStart()
{
    m_act      = Act::Idle;
    m_timer    = 0.0f;
    m_cooldown = std::max(attackInterval, 0.0f);
    m_dealt    = false;
    m_landCued = false;
    m_beamStage = 0;
    m_warnedNoCombat = false;
    RefreshPlayer();

    if (!Anim()) {
        debug.LogError("BossAiComponent requires a BossAnimatorComponent on the same object "
                       "(it is the only entry point to the Animator).");
    }

    // 10.6 の P2 は磁力パルスを攻撃表に持つ。8 章はそれを «極を切り替える瞬間» と
    // 決めているので、切替の合図をここで拾う。P1 では出さない。
    if (auto* core = Core()) {
        core->onPolaritySwitch = [this]() {
            if (!IsAlive()) return;
            if (m_act != Act::Idle) return;
            if (Core() && Core()->CurrentPhase() < 2) return;
            BeginPulse();
        };
    } else {
        debug.LogError("BossAiComponent requires a BossPolarityCoreComponent on the same object.");
    }
}

inline void BossAiComponent::RefreshPlayer()
{
    // 毎フレーム取り直す。プレイヤーが作り直される構成 (リスポーン) でも繋がり直る。
    if (m_player.Resolve(scene)) return;
    if (GameObject* player = scene.FindWithTag(playerTag))
        m_player = EntityRef{ player->GetID() };
}

inline bool BossAiComponent::IsAlive() const
{
    const auto* health = scene.GetScript<EnemyHealthComponent>();
    return !health || health->IsAlive();
}

inline Vector3 BossAiComponent::Forward() const
{
    const Vector3 facing = transform.worldRotation * Vector3::FORWARD;
    return Vector3{ facing.x, 0.0f, facing.z }.NormalizedOr(Vector3::FORWARD);
}

inline void BossAiComponent::FaceDirection(const Vector3& direction, float dt,
                                           float degreesPerSecond) const
{
    Vector3 flat{ direction.x, 0.0f, direction.z };
    if (flat.LengthSq() < EPSILON) return;

    auto* rb = scene.GetComponent<RigidBodyComponent>();
    if (!rb || !rb->rigidBody) return;

    // 角速度で回す。指数補間だと «残り角度が小さいほど遅い» になり、ビームを薙ぐ速さが
    // プレイヤーの位置で変わってしまう。8 章は «走って追い越せる» ことを求めている。
    const Quaternion current = rb->rigidBody->GetRotation();
    const Quaternion desired = Quaternion::LookRotation(flat.Normalized());
    const float step = std::max(degreesPerSecond, 0.0f) * dt * DEG2RAD;

    // 残り角度。内積から出した半角を 2 倍したものが 2 つの姿勢の間の角度になる。
    const float dot   = std::clamp(current.x * desired.x + current.y * desired.y +
                                   current.z * desired.z + current.w * desired.w,
                                   -1.0f, 1.0f);
    const float angle = 2.0f * std::acos(std::fabs(dot));
    const float t     = angle > EPSILON ? std::min(step / angle, 1.0f) : 1.0f;

    rb->rigidBody->SetRotation(Quaternion::Slerp(current, desired, t).Normalized());
}

inline void BossAiComponent::StopHorizontal() const
{
    Vector3 velocity = physics.GetVelocity();
    velocity.x = 0.0f;
    velocity.z = 0.0f;
    physics.SetVelocity(velocity);
}

inline void BossAiComponent::MoveHorizontal(const Vector3& direction, float speed) const
{
    Vector3 velocity = physics.GetVelocity();
    velocity.x = direction.x * std::max(speed, 0.0f);
    velocity.z = direction.z * std::max(speed, 0.0f);
    physics.SetVelocity(velocity);
}

inline bool BossAiComponent::HitPlayer(int amount) const
{
    GameObject* player = Player();
    if (!player || amount <= 0) return false;

    auto* combat = CombatManagerComponent::Instance();
    if (!combat) {
        if (!m_warnedNoCombat) {
            m_warnedNoCombat = true;
            debug.LogError("BossAiComponent found no CombatManagerComponent in the scene. "
                           "Boss attacks deal no damage.");
        }
        return false;
    }
    return combat->DamagePlayer(player, amount);
}

inline bool BossAiComponent::HitPlayerInSphere(const Vector3& center, float radius,
                                               int amount) const
{
    GameObject* player = Player();
    if (!player) return false;

    // WHY OverlapSphere を使わないか: 衝撃波が拾いたいのはプレイヤー 1 体だけで、
    //     盤面の全コライダーを集めて絞り込む理由が無い。距離で足りる。
    Vector3 toPlayer = player->transform.worldPosition - center;
    toPlayer.y = 0.0f;
    if (toPlayer.LengthSq() > radius * radius) return false;
    return HitPlayer(amount);
}

inline void BossAiComponent::OnFixedUpdate()
{
    const float dt = time.FixedDeltaTime();
    RefreshPlayer();

    if (!IsAlive()) {
        if (m_act != Act::Idle) EndAct();
        debugAct = "Dead";
        StopHorizontal();
        // WHY ここで倒れさせるか: HP を持っているのは EnemyHealthComponent で、
        //     あちらは «敵が倒れたら消す» までしか知らない (ボスの Animator も
        //     BossAnimatorComponent も見えていない)。倒れた «見え» を出せるのは、
        //     両方を知っているここだけ。毎フレーム押しても Bool なので害は無い。
        if (auto* anim = Anim()) anim->SetDead(true);
        return;
    }

    if (GameObject* player = Player()) {
        Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
        toPlayer.y = 0.0f;
        debugDistance = toPlayer.Length();
    }

    switch (m_act) {
    case Act::Stomp:        TickStomp(dt);        return;
    case Act::JumpUp:       TickJumpUp(dt);       return;
    case Act::JumpAir:      TickJumpAir(dt);      return;
    case Act::JumpLand:     TickJumpLand(dt);     return;
    case Act::ChargeWindup: TickChargeWindup(dt); return;
    case Act::ChargeRun:    TickChargeRun(dt);    return;
    case Act::CrashStun:    TickCrashStun(dt);    return;
    case Act::Beam:         TickBeam(dt);         return;
    case Act::Pulse:        TickPulse(dt);        return;
    case Act::Idle:         break;
    }
    TickIdle(dt);
}

inline void BossAiComponent::TickIdle(float dt)
{
    debugAct = "Idle";
    m_cooldown = std::max(0.0f, m_cooldown - dt);

    GameObject* player = Player();
    if (!player) {
        StopHorizontal();
        return;
    }

    Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
    toPlayer.y = 0.0f;
    const float distance = toPlayer.Length();
    if (distance < EPSILON) {
        StopHorizontal();
        return;
    }

    const Vector3 direction = toPlayer / distance;
    FaceDirection(direction, dt, turnSpeed);

    if (m_cooldown <= 0.0f && SelectAttack()) return;

    // 間合いより遠ければ詰める。近ければ止まって «腹下へ潜る» 余地を残す。
    if (distance > std::max(keepDistance, 0.0f)) MoveHorizontal(direction, patrolSpeed);
    else                                         StopHorizontal();
}

inline bool BossAiComponent::SelectAttack()
{
    const float distance = debugDistance;

    // 8 章の表をそのまま上から当てる。範囲が重ならないよう境界は片側だけを含める。
    if (distance >= chargeMinRange) { BeginCharge(); return true; }

    // 中距離はビームと大ジャンプで交互に出す。
    // WHY 交互にするか: 同じ間合いから必ず同じ手が来ると、1 度覚えた後は «立ち位置を
    //     変えない» が最適解になり、8 章が «距離で役割を分ける» ことで作ろうとした
    //     読み合いが消える。距離の役割は保ったまま、手の中身だけ振る。
    if (distance >= beamMinRange) {
        m_preferJump = !m_preferJump;
        if (m_preferJump) BeginJump();
        else              BeginBeam();
        return true;
    }

    // 踏みつけの間合いより外・ビームの間合いより内。8 章の表が空けている帯なので、
    // 距離を詰める手段でもある大ジャンプを当てる。
    if (distance >= jumpMinRange) { BeginJump();  return true; }
    if (distance <= stompMaxRange) { BeginStomp(); return true; }

    // どれにも当たらない設定 (jumpMinRange > stompMaxRange の隙間) は «詰める» に任せる。
    return false;
}

inline BossLeg BossAiComponent::PickStompLeg(const Vector3& toPlayer) const
{
    const Vector3 forward = Forward();
    const Vector3 right{ forward.z, 0.0f, -forward.x };

    const bool front = Vector3::Dot(toPlayer, forward) >= 0.0f;
    // 8 章「プレイヤーはボスの周囲を回るため、背後へ回り込んでも踏みつけが届く」。
    const bool onRight = Vector3::Dot(toPlayer, right) >= 0.0f;

    if (front) return onRight ? BossLeg::FrontRight : BossLeg::FrontLeft;
    return onRight ? BossLeg::BackRight : BossLeg::BackLeft;
}

inline Vector3 BossAiComponent::StompPoint(BossLeg leg) const
{
    // ヒットボックスのリグが居るなら、足ボーンの «今» の位置がそのまま着弾点になる。
    // アニメーションが振り上げて振り下ろす軌跡をそのまま拾えるので、Reach の推定が要らない。
    if (const auto* rig = scene.GetScript<BossHitboxRigComponent>()) {
        if (GameObject* foot = rig->FootBone(leg)) {
            Vector3 point = foot->transform.worldPosition;
            point.y = transform.worldPosition.y;
            return point;
        }
    }

    // リグが無い構成へのフォールバック。4 本の脚が胴体の四隅にあるという構造だけから出す。
    const Vector3 forward = Forward();
    const Vector3 right{ forward.z, 0.0f, -forward.x };

    const bool front   = leg == BossLeg::FrontRight || leg == BossLeg::FrontLeft;
    const bool onRight = leg == BossLeg::FrontRight || leg == BossLeg::BackRight;

    const float half = std::max(stompReach, 0.0f) * 0.7071f;
    Vector3 point = transform.worldPosition;
    point += forward * (front ? half : -half);
    point += right   * (onRight ? half : -half);
    point.y = transform.worldPosition.y;
    return point;
}

inline void BossAiComponent::BeginJump()
{
    m_act      = Act::JumpUp;
    m_timer    = 0.0f;
    m_dealt    = false;
    m_landCued = false;
    debugAct   = "Jump Up";

    m_jumpStart  = transform.worldPosition;
    m_jumpTarget = m_jumpStart;

    // Jump() は接地も同時に落とす。落とさないと JumpUp が終わった次のフレームに
    // FallIdle → Land が成立し、滞空せずに着地モーションへ落ちる (BossAnimatorComponent)。
    if (auto* anim = Anim()) anim->Jump();
}

inline void BossAiComponent::TickJumpUp(float dt)
{
    debugAct = "Jump Up";
    StopHorizontal();
    m_timer += dt;

    // 踏み切るまでは地上に居る。ここで落下点を狙い定める。
    if (GameObject* player = Player()) {
        Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
        toPlayer.y = 0.0f;
        FaceDirection(toPlayer, dt, turnSpeed);

        const float distance = toPlayer.Length();
        const float travel   = std::min(distance, std::max(jumpMaxTravel, 0.0f));
        m_jumpTarget = transform.worldPosition +
                       toPlayer.NormalizedOr(Forward()) * travel;
    }

    if (m_timer < jumpTakeoffTime) return;

    // README の f33。4 脚が地面を離れるフレームで «跳んだ» ことにする。
    m_jumpStart = transform.worldPosition;
    m_jumpTarget.y = m_jumpStart.y;
    m_act   = Act::JumpAir;
    m_timer = 0.0f;

    // 放物線はここが引く。重力を効かせたままだと二重に落ちる
    // (README: アニメーション側は跳んでいる間も胴体の高さを固定してある)。
    physics.SetGravityScale(0.0f);
}

inline void BossAiComponent::TickJumpAir(float dt)
{
    debugAct = "Jump Air";
    m_timer += dt;

    const float air = std::max(jumpAirTime, 0.05f);
    const float t   = Clamp01(m_timer / air);

    // 水平は等速、垂直は放物線。滞空時間そのものは FallIdle をループさせる長さなので、
    // 「どれだけ見上げさせたいか」で決めてよい (README)。
    Vector3 desired = Vector3::Lerp(m_jumpStart, m_jumpTarget, t);
    desired.y += std::max(jumpArcHeight, 0.0f) * 4.0f * t * (1.0f - t);

    // 位置ではなく速度で運ぶ。Transform 直書きだと衝突解決を飛ばして壁を抜ける。
    const Vector3 delta = desired - transform.worldPosition;
    physics.SetVelocity(dt > 0.0f ? delta / dt : Vector3::ZERO);

    // README: Land の f22 が接地フレーム。着地の landContactTime «前» に流し始めないと、
    // 潰れ込みが接地より後ろへずれて «着いてから沈む» に見える。
    if (!m_landCued && m_timer >= air - std::max(landContactTime, 0.0f)) {
        m_landCued = true;
        if (auto* anim = Anim()) anim->SetGrounded(true);
    }

    if (m_timer < air) return;

    // 接地。ここが Land の f22 に重なる。
    physics.SetGravityScale(1.0f);
    StopHorizontal();
    (void)HitPlayerInSphere(transform.worldPosition, jumpHitRadius, jumpDamage);

    m_act = Act::JumpLand;
    // Land は既に landContactTime ぶん進んでいる。0 から数え直すと硬直が伸びる。
    m_timer = std::max(landContactTime, 0.0f);
}

inline void BossAiComponent::TickJumpLand(float dt)
{
    debugAct = "Jump Land";
    StopHorizontal();
    m_timer += dt;

    // README: f26 が潰れ最下点。そこから立ち直るまでが 8 章の «明確な隙»。
    if (auto* core = Core()) core->SetStaggered(m_timer >= landStaggerFrom);

    if (m_timer >= landTotalTime) EndAct();
}

inline void BossAiComponent::BeginStomp()
{
    GameObject* player = Player();
    Vector3 toPlayer = player ? (player->transform.worldPosition - transform.worldPosition)
                              : Forward();
    toPlayer.y = 0.0f;

    m_stompLeg = PickStompLeg(toPlayer);
    m_act      = Act::Stomp;
    m_timer    = 0.0f;
    m_dealt    = false;
    debugAct   = "Stomp";

    if (auto* anim = Anim()) anim->Stomp(m_stompLeg);
}

inline void BossAiComponent::TickStomp(float dt)
{
    debugAct = "Stomp";
    StopHorizontal();
    m_timer += dt;

    // README: f23 で接地、f25 が潰れ最下点。その間に 1 度だけ衝撃波を出す。
    if (!m_dealt && m_timer >= stompHitTime) {
        m_dealt = true;
        (void)HitPlayerInSphere(StompPoint(m_stompLeg), stompRadius, stompDamage);
    }

    // README: f35–46 は足が刺さったままの硬直。8 章の «反撃を取らせる» 区間。
    if (auto* core = Core()) core->SetStaggered(m_timer >= stompStaggerFrom);

    if (m_timer >= stompTotalTime) EndAct();
}

inline void BossAiComponent::BeginCharge()
{
    m_act    = Act::ChargeWindup;
    m_timer  = 0.0f;
    m_dealt  = false;
    debugAct = "Charge Windup";

    if (auto* anim = Anim()) anim->BeginCharge();
}

inline void BossAiComponent::TickChargeWindup(float dt)
{
    debugAct = "Charge Windup";
    StopHorizontal();
    m_timer += dt;

    // 溜めている間だけ狙いを定める。踏み込んだ後は 8 章の通り方向転換しない。
    if (GameObject* player = Player()) {
        Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
        toPlayer.y = 0.0f;
        FaceDirection(toPlayer, dt, turnSpeed);
        m_chargeDir = toPlayer.NormalizedOr(m_chargeDir);
    }

    if (m_timer < chargeWindupTime) return;

    m_act   = Act::ChargeRun;
    m_timer = 0.0f;
    m_dealt = false;
}

inline void BossAiComponent::TickChargeRun(float dt)
{
    debugAct = "Charge Run";
    m_timer += dt;
    MoveHorizontal(m_chargeDir, chargeSpeed);

    // WHY 距離で轢くか: 5 m/s で走り抜ける 1 フレームぶんの移動は 8 cm 前後あり、
    //     接触解決の順序次第で «すり抜けた» フレームができる。避けたのか判定が
    //     抜けたのかはプレイヤーから区別できない。
    if (!m_dealt && HitPlayerInSphere(transform.worldPosition, chargeHitRadius, chargeDamage))
        m_dealt = true;

    // 8 章「避けて壁へ誘導すると、ボス自身が地形に激突して大ダメージ＋長時間スタン」。
    // 胴体の高さから前方を見る。足元から撃つと床の傾斜を壁と読む。
    Vector3 eye = transform.worldPosition;
    eye.y += 2.0f;
    RaycastHit hit;
    if (physics.Raycast(eye, m_chargeDir, std::max(wallProbe, 0.1f), hit)) {
        // 自分自身とプレイヤーは壁ではない。プレイヤーを壁と読むと、轢いた瞬間に
        // 激突して «避けていないのにボスが自滅する» ことになる。
        GameObject* self = scene.Self();
        const bool isSelf   = hit.gameObject == self;
        const bool isPlayer = hit.gameObject && hit.gameObject->tag == playerTag;
        if (!isSelf && !isPlayer) {
            BeginCrash();
            return;
        }
    }

    if (m_timer >= chargeMaxSeconds) {
        if (auto* anim = Anim()) anim->EndCharge();
        EndAct();
    }
}

inline void BossAiComponent::BeginCrash()
{
    m_act    = Act::CrashStun;
    m_timer  = 0.0f;
    debugAct = "Crash Stun";
    StopHorizontal();

    if (auto* anim = Anim()) anim->Crash();

    // 8 章の «大ダメージ»。15 章「敵を武器として使う」がボス自身にも適用される、
    // 唯一の «銃以外で削れる» 経路なので、盤面の衝突と同じ CombatManager ではなく
    // 自分の HP へ直接入れる (誰かがぶつけたわけではない)。
    if (auto* health = scene.GetScript<EnemyHealthComponent>())
        (void)health->ApplyDamage(std::max(crashSelfDamage, 0));

    // 激突の間だけリングを落とす。8 章「極性リングが消灯し、無防備であることが
    // 見た目で分かる」。この 5 秒がプレイヤーの組み立て時間になる。
    if (auto* core = Core()) {
        core->SetStaggered(true);
        core->SetCoreDark(true);
    }
}

inline void BossAiComponent::TickCrashStun(float dt)
{
    debugAct = "Crash Stun";
    StopHorizontal();
    m_timer += dt;
    if (m_timer >= crashStunTime) EndAct();
}

inline void BossAiComponent::BeginBeam()
{
    m_act       = Act::Beam;
    m_timer     = 0.0f;
    m_beamStage = 0;
    debugAct    = "Beam";

    if (auto* anim = Anim()) anim->BeginBeam();
    // 点火はここから始まる。構えの 1.0 秒をかけて針から本径まで太る (8 章の予兆)。
    if (auto* beam = Beam()) beam->SetFiring(true);
    // 狙いも同時に置く。次の FixedUpdate を待つと、点火の 1 フレーム目だけ
    // 終端が前回の照射のまま残る。
    AimBeam(0.0f);
}

inline void BossAiComponent::TickBeam(float dt)
{
    debugAct = "Beam";
    // 8 章「照射中はボスが停止する」。
    StopHorizontal();
    m_timer += dt;

    if (m_beamStage == 0) {
        // 構えの間はまだ薙がない。ここで狙いを付け切る。
        if (GameObject* player = Player()) {
            Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
            toPlayer.y = 0.0f;
            FaceDirection(toPlayer, dt, turnSpeed);
        }
        // 点火中の細い線も «どこへ向くか» を見せる。予兆はここで読ませる。
        // 構えの間は地面を指したまま (振り上げるのは薙ぎ始めてから)。
        AimBeam(0.0f);
        if (m_timer >= beamStartTime) {
            m_beamStage = 1;
            m_timer     = 0.0f;
        }
        return;
    }

    if (m_beamStage == 1) {
        // 薙ぐ。プレイヤーを追うが、追いつけない速さに保つことで «移動を強制する»。
        if (GameObject* player = Player()) {
            Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
            toPlayer.y = 0.0f;
            FaceDirection(toPlayer, dt, beamSweepSpeed);
        }
        AimBeam(beamSweepTime > 0.0f ? m_timer / beamSweepTime : 1.0f);

        if (m_timer >= beamSweepTime) {
            m_beamStage = 2;
            m_timer     = 0.0f;
            if (auto* anim = Anim())  anim->EndBeam();
            // 消灯もビームの側が時間をかけて処理する。ここは «止めた» とだけ言う。
            if (auto* beam = Beam()) beam->SetFiring(false);
        }
        return;
    }

    if (m_timer >= beamEndTime) EndAct();
}

inline void BossAiComponent::AimBeam(float sweep01)
{
    auto* beam = Beam();
    if (!beam) return;

    // 終端はボスの正面へ置く。旋回がそのまま «薙ぎ» になるので、AI は向きだけを
    // 決めればよい (8 章「ボスが旋回して薙ぐ」)。
    //
    // WHY 距離をプレイヤーへ追従させるか: 固定距離だと、円弧がプレイヤーの立っている
    //     半径を通らない配置ができてしまい、«正面を向いているのに永久に当たらない»
    //     ビームになる。距離が付いてくるなら、避ける手は «横へ動く» に絞られる。
    const float reach = Clamp(debugDistance, std::max(beamNearReach, 0.1f),
                              std::max(beamLength, beamNearReach + 0.1f));

    // 薙ぎ終わりへ向けて終端を持ち上げる。
    //
    // WHY 始めから上げないか: 8 章は «地面へ照射» と書いていて、床を焼いている絵が
    //     «逃げ道が消えていく» の説明になっている。最初から水平だと、ただの
    //     «太い線が横切る» になって床の焦げが意味を失う。地面から始めて振り上げると、
    //     同じ 1 回の中で «焼かれた床» と «胴を薙ぐ高さ» の両方が出る。
    //
    // WHY 立ち上がりを遅らせるか: 線形に上げると掃射の中盤で既に腰の高さになり、
    //     しゃがむ / 距離を取るといった «下をくぐる» 判断の余地が一瞬で消える。
    //     二乗にすると前半は床に留まり、終盤だけ跳ね上がる。
    const float rise = Clamp01(sweep01);
    const float lift = std::max(beamRiseHeight, 0.0f) * rise * rise;

    beam->Aim(transform.worldPosition + Forward() * reach, lift);
}

inline void BossAiComponent::BeginPulse()
{
    m_act    = Act::Pulse;
    m_timer  = 0.0f;
    m_dealt  = false;
    debugAct = "Pulse";

    if (auto* anim = Anim()) anim->Pulse();
}

inline void BossAiComponent::TickPulse(float dt)
{
    debugAct = "Pulse";
    StopHorizontal();
    m_timer += dt;

    if (!m_dealt && m_timer >= pulseHitTime) {
        m_dealt = true;
        const Vector3 center = transform.worldPosition;
        (void)HitPlayerInSphere(center, pulseRadius, pulseDamage);

        // 8 章「帯電中の雑魚が外向きに弾き飛ばされる。極性そのものは残る」。
        // 引力を切ってから弾く。切らないと、次のフレームに盤面が同じリンクを
        // 張り直して速度を上書きし、弾かれたように見えない。
        for (GameObject* object : scene.FindObjectsOfType<PolarityBodyComponent>()) {
            if (!object || !object->activeInHierarchy()) continue;

            const auto* target = scene.GetScript<PolarityTargetComponent>(object);
            if (!target || !target->IsCharged()) continue;

            Vector3 away = object->transform.worldPosition - center;
            away.y = 0.0f;
            if (away.LengthSq() > pulseRadius * pulseRadius) continue;

            if (auto* body = scene.GetScript<PolarityBodyComponent>(object))
                body->CancelPull();

            const Vector3 direction = away.NormalizedOr(Forward());
            physics.SetVelocity(object, direction * std::max(pulseKnockback, 0.0f));
        }
    }

    if (m_timer >= pulseTotalTime) EndAct();
}

inline void BossAiComponent::EndAct()
{
    m_act      = Act::Idle;
    m_timer    = 0.0f;
    m_dealt    = false;
    m_landCued = false;
    m_cooldown = std::max(attackInterval, 0.0f);
    debugAct   = "Idle";

    // 硬直と消灯は行動の終わりで必ず解く。途中で打ち切られた経路 (死亡・激突) も
    // ここを通るので、「倒したのにリングが消えたまま」が残らない。
    if (auto* core = Core()) {
        core->SetStaggered(false);
        core->SetCoreDark(false);
    }
    // 跳んでいる途中で打ち切られると、重力を切ったまま・空中扱いのままになる。
    // どちらも «ボスが浮いたまま動かない» という止まり方をするので、必ず戻す。
    physics.SetGravityScale(1.0f);
    if (auto* anim = Anim()) {
        anim->EndCharge();
        anim->EndBeam();
        anim->SetGrounded(true);
    }
    // 死亡や割り込みで照射の途中から抜けても、線が空に残らないようにする。
    if (auto* beam = Beam()) beam->SetFiring(false);
}

inline void BossAiComponent::OnDrawGizmos()
{
    if (!drawDebugRanges) return;

    const Vector3 origin = transform.worldPosition;
    debug.DrawSphere(origin, chargeMinRange, { 1.0f, 0.3f, 0.2f, 1.0f });
    debug.DrawSphere(origin, beamMinRange,   { 1.0f, 0.8f, 0.2f, 1.0f });
    debug.DrawSphere(origin, stompMaxRange,  { 0.3f, 0.8f, 1.0f, 1.0f });
    debug.DrawRay(origin, Forward() * beamLength, { 1.0f, 0.8f, 0.2f, 1.0f });
}

} // namespace sandbox
