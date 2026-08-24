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
#pragma once

#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/EnemyAiBase.hpp>
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

    void OnFixedUpdate() override;

protected:
    void OnEnemyStart() override;

private:
    /// 突進の 3 段階。Chase 以外は途中で中断しない。
    enum class Phase : int { Chase = 0, Telegraph, Charge, Recover };

    void TickTelegraph(float dt);
    void TickCharge(float dt);
    void TickRecover(float dt);
    void BeginTelegraph();
    void Chase(float dt);

    Phase   m_phase      = Phase::Chase;
    float   m_timer      = 0.0f;
    /// 踏み込んだ瞬間に固定した突進方向。以後は変えない。
    Vector3 m_chargeDir  = Vector3::ZERO;
    bool    m_chargeHit  = false;
};

FBZZ_REFLECT(EnemyRollerComponent)


inline void EnemyRollerComponent::OnEnemyStart()
{
    // 車輪が回って見えるのは Move / Attack クリップの Wheel ボーンで、剛体の回転ではない。
    // 物理に転がされると進行方向と車輪の向きが食い違う。
    physics.SetFreezeRotation(true, true, true);

    m_phase     = Phase::Chase;
    m_timer     = 0.0f;
    m_chargeDir = Vector3::ZERO;
    m_chargeHit = false;
}

inline void EnemyRollerComponent::OnFixedUpdate()
{
    const float dt = time.FixedDeltaTime();

    if (!IsAlive()) {
        debugState = "Dead";
        m_phase    = Phase::Chase;
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

    m_phase = Phase::Charge;
    m_timer = std::max(chargeSeconds, 0.05f);
}

inline void EnemyRollerComponent::TickCharge(float dt)
{
    debugState = "Charge";

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

inline void EnemyRollerComponent::TickRecover(float dt)
{
    debugState = "Recover";
    StopHorizontal();

    m_timer -= dt;
    if (m_timer > 0.0f) return;

    m_phase = Phase::Chase;
    m_timer = 0.0f;
}

} // namespace sandbox
