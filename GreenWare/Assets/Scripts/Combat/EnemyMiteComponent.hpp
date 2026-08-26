/// @file EnemyMiteComponent.hpp
/// @brief Enemy A「Mite」— 浮遊して詰め寄り、爪で殴るホバードローン (企画書 8)
/// @author Hasegawa Jin
/// @date 2026-08-24
///
/// WHY 重力を切って自前で高さを持つか:
///   8 章は Mite を「接地していないため、引力で最もよく飛ぶ」と定義している。重力を
///   効かせたまま浮かせようとすると、7.3 の飛行中も落下が積み上がって軌道が弧を描き、
///   «最短距離を突っ切る» はずの集束が毎回下向きにずれる。接地しない敵という設定を
///   物理側でそのまま表現し、高さの維持はこのスクリプトが持つ。
///
/// WHY 当たり判定を接触ではなく時間で入れるか:
///   ノーマルスライムは球体が触れれば殴ったことにしてよかったが、こちらは 1.5 秒の
///   爪モーションがある。接触した瞬間に減らすと、振りかぶる前にダメージだけが入り、
///   12.1 が最優先とした「何をされたか分かる」が崩れる。振り始めと当たる瞬間を分ける。
///
/// WHY 殴ったら引くか (張り付かないか):
///   間合いに入ったら止まって振り続ける敵は、プレイヤーから見ると «自分の位置に
///   関係なく一定間隔で殴ってくる置物» になる。避ける・詰める操作が結果を変えないので、
///   3.1 の «敵を武器として使う» ために銃を構える余裕もそこで消える。振り切ったら
///   一度離れ、爪が空くまでは横へ回る。近づいてくる 1 秒が «次が来る» の予告になり、
///   離れている 1 秒がプレイヤーの手番になる。
#pragma once

#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/EnemyAiBase.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class EnemyMiteComponent : public EnemyAiBase {
    FBZZ_SCRIPT_DERIVED(EnemyMiteComponent, EnemyAiBase)

public:
    FBZZ_GROUP("Hover")
    FBZZ_FIELD_RANGE(float, hoverHeight, 1.05f, "Hover Height", 0.0f, 8.0f)
    FBZZ_TOOLTIP("足元から地面までの目標距離。全高 1.16m の機体が浮いて見える高さ")
    FBZZ_FIELD_RANGE(float, hoverStiffness, 5.0f, "Hover Stiffness", 0.5f, 30.0f)
    FBZZ_TOOLTIP("目標高度へ戻る速さ。高いほど固く、低いほどふわつく")
    FBZZ_FIELD_RANGE(float, hoverMaxSpeed, 4.0f, "Hover Max Speed", 0.5f, 30.0f)
    FBZZ_TOOLTIP("高度補正で出してよい垂直速度の上限。落下から戻るときの跳ね上がりを抑える")
    FBZZ_FIELD_RANGE(float, bobAmplitude, 0.12f, "Bob Amplitude", 0.0f, 1.0f)
    FBZZ_TOOLTIP("停止中も生きて見せるための上下動。0 で完全に静止する")
    FBZZ_FIELD_RANGE(float, bobHz, 0.55f, "Bob Hz", 0.0f, 4.0f)
    FBZZ_FIELD_RANGE(float, groundProbe, 12.0f, "Ground Probe", 1.0f, 60.0f)
    FBZZ_TOOLTIP("地面を探す下向きレイの長さ。届かなければ今の高度を保つ")

    FBZZ_GROUP("Claw Attack")
    FBZZ_FIELD_RANGE(float, attackRange, 2.0f, "Attack Range", 0.2f, 12.0f)
    FBZZ_TOOLTIP("この距離まで詰めたら足を止めて爪を振る")
    FBZZ_FIELD_RANGE(float, attackDuration, 1.5f, "Attack Duration", 0.1f, 6.0f)
    FBZZ_TOOLTIP("爪モーションの長さ。Attack.anim の尺 (1.5 秒) に合わせる")
    FBZZ_FIELD_RANGE(float, attackHitTime, 0.55f, "Hit Time", 0.0f, 6.0f)
    FBZZ_TOOLTIP("振り始めから当たり判定が出るまでの秒数")
    FBZZ_FIELD_RANGE(float, attackHitRange, 2.6f, "Hit Range", 0.2f, 12.0f)
    FBZZ_TOOLTIP("当たり判定が出た瞬間にこの距離内なら当たる。Attack Range より少し広く取る")

    FBZZ_GROUP("Weave")
    FBZZ_FIELD_RANGE(float, retreatSeconds, 0.7f, "Retreat", 0.0f, 4.0f)
    FBZZ_TOOLTIP("爪を振り切った後に下がる時間。ここがプレイヤーの手番になる")
    FBZZ_FIELD_RANGE(float, retreatSpeed, 3.6f, "Retreat Speed", 0.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, strafeSpeed, 1.8f, "Strafe Speed", 0.0f, 10.0f)
    FBZZ_TOOLTIP("爪が空くのを待つ間、間合いを保ったまま横へ回る速さ。0 で棒立ちになる")

    // WHY 傾きを速度から作るか (アニメーションに持たせないか):
    //   .anim のクリップは前進しか知らない。旋回・後退・横回りはこのスクリプトが
    //   その場で決めているので、傾ける判断も同じ場所に無いと «真横へ滑る直立した機体»
    //   になる。浮いている敵は、傾きが唯一の «今どちらへ動いているか» の手掛かり。
    FBZZ_GROUP("Lean")
    FBZZ_FIELD_RANGE(float, bankAngle, 22.0f, "Bank Angle", 0.0f, 60.0f)
    FBZZ_TOOLTIP("横へ流れているときに機体を倒す角度 [deg]")
    FBZZ_FIELD_RANGE(float, noseAngle, 12.0f, "Nose Angle", 0.0f, 45.0f)
    FBZZ_TOOLTIP("前へ出るときに機首を下げる角度 [deg]。下がるときは逆に反る")

    void OnFixedUpdate() override;

protected:
    void OnEnemyStart() override;

    [[nodiscard]] const se::Bank* MoveVoiceBank()    const override { return &se::kMiteHoverLoop; }
    [[nodiscard]] const se::Bank* DestroyVoiceBank() const override { return &se::kMiteDestroy; }

private:
    /// 地面からの目標高度へ垂直速度を寄せる。地面が見つからなければ高さを保つ。
    void ApplyHover();
    /// 爪モーションを 1 ステップ進める。当たる瞬間だけダメージを入れる。
    void TickAttack(float dt);
    /// 足元の地面の高さ。見つからなければ false。
    [[nodiscard]] bool FindGroundY(float& outY) const;

    /// 水平速度を書き、facing へ向き直りながら進行方向へ傾ける。上下はホバーのまま。
    void Drive(const Vector3& horizontal, const Vector3& facing, float dt);
    /// facing の向きへ、velocity のぶんだけ傾けて向き直る。
    void FaceWithLean(const Vector3& facing, const Vector3& velocity, float dt);
    /// 回り込む向き。個体ごとに逆回りから始め、爪を振るたびに反転する。
    [[nodiscard]] Vector3 Sideways(const Vector3& facing) const
    { return Vector3::Cross(Vector3::UP, facing) * m_strafeSign; }

    float m_attackTimer  = 0.0f;
    bool  m_attackLanded = false;
    float m_bobSeed      = 0.0f;
    float m_retreatTimer = 0.0f;
    float m_strafeSign   = 1.0f;
    /// 死んだ瞬間に重力を戻したか。撃破された機体はその場に浮かず落ちる。
    bool  m_droppedOnDeath = false;
};

FBZZ_REFLECT(EnemyMiteComponent)


inline void EnemyMiteComponent::OnEnemyStart()
{
    // 向きはこちらが書く。物理の回転に任せると、浮いた球体が接触のたびに回り出す。
    physics.SetFreezeRotation(true, true, true);
    physics.SetGravityScale(0.0f);

    m_attackTimer    = 0.0f;
    m_attackLanded   = false;
    m_retreatTimer   = 0.0f;
    m_droppedOnDeath = false;

    // 個体ごとに上下動の位相をずらす。揃っていると群れが 1 つの塊に見える。
    GameObject* self = scene.Self();
    const auto  index = self ? self->GetID().index : 0u;
    m_bobSeed = static_cast<float>(index) * 2.39f;
    // 回り込む向きも個体で分ける。全員が同じ向きに回ると輪になって囲めてしまう。
    m_strafeSign = (index & 1u) != 0u ? 1.0f : -1.0f;
}

inline void EnemyMiteComponent::OnFixedUpdate()
{
    const float dt = time.FixedDeltaTime();

    if (IsPolarityDriven()) {
        // 引力・溜め・ノックバックの最中。高度補正まで掛けると «ギュンッ» が濁る。
        //
        // 振りかけの爪はここで捨てる。Animator 側は Attack ステートを exitTime で
        // 抜け切っているので、残したまま再開すると «モーションが無いのに当たる» になる。
        debugState     = "Polarity";
        m_attackTimer  = 0.0f;
        m_retreatTimer = 0.0f;
        return;
    }

    if (!IsAlive()) {
        debugState = "Dead";
        // 浮いたまま消えると撃破が «消えた» にしか見えない。重力を戻して落とす。
        if (!m_droppedOnDeath) {
            m_droppedOnDeath = true;
            physics.SetGravityScale(1.0f);
        }
        StopHorizontal();
        return;
    }

    ApplyHover();

    if (m_attackTimer > 0.0f) {
        TickAttack(dt);
        return;
    }

    if (IsMovementLocked()) {
        debugState = "Locked";
        StopHorizontal();
        return;
    }

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

    if (m_retreatTimer > 0.0f) {
        m_retreatTimer -= dt;
        debugState = "Retreat";
        // 下がっている間も正面は外さない。背を向けて逃げると «諦めた» に見えるうえ、
        // 次の爪がどこから来るのかが読めなくなる。
        Drive(direction * -std::max(retreatSpeed, 0.0f), direction, dt);
        return;
    }

    if (distanceSq <= attackRange * attackRange) {
        if (!AttackReady()) {
            debugState = "Circle";
            Drive(Sideways(direction) * std::max(strafeSpeed, 0.0f), direction, dt);
            return;
        }
        StopHorizontal();
        FaceWithLean(direction, Vector3::ZERO, dt);

        debugState     = "Attack";
        m_attackTimer  = std::max(attackDuration, 0.05f);
        m_attackLanded = false;
        // 次に待つときは逆へ回る。同じ側から入り続けると位置取りが読み切られる。
        m_strafeSign   = -m_strafeSign;
        BeginAttackCooldown();
        animator.SetTrigger(enemyanim::kAttack);
        se::Play(audio, se::kMiteAttack);
        return;
    }

    Drive(direction * std::max(moveSpeed, 0.0f), direction, dt);
    debugState = "Chasing";
}

inline void EnemyMiteComponent::TickAttack(float dt)
{
    debugState = "Attack";
    StopHorizontal();

    const float total   = std::max(attackDuration, 0.05f);
    const float elapsed = total - m_attackTimer;
    m_attackTimer -= dt;

    GameObject* player = Player();
    if (player && elapsed < attackHitTime) {
        Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
        toPlayer.y = 0.0f;
        // 振っている間は水平に構える。傾いたまま爪を出すと当たり判定と姿勢がずれて見える。
        FaceWithLean(toPlayer, Vector3::ZERO, dt);
    }

    if (!m_attackLanded && elapsed >= attackHitTime) {
        m_attackLanded = true;
        if (player) {
            Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
            toPlayer.y = 0.0f;
            if (toPlayer.LengthSq() <= attackHitRange * attackHitRange)
                (void)HitPlayer(player);
        }
    }

    if (m_attackTimer <= 0.0f) {
        m_attackTimer  = 0.0f;
        m_retreatTimer = std::max(retreatSeconds, 0.0f);
    }
}

inline void EnemyMiteComponent::Drive(const Vector3& horizontal, const Vector3& facing, float dt)
{
    Vector3 velocity = physics.GetVelocity();
    velocity.x = horizontal.x;
    velocity.z = horizontal.z;
    physics.SetVelocity(velocity);
    FaceWithLean(facing, horizontal, dt);
}

inline void EnemyMiteComponent::FaceWithLean(const Vector3& facing, const Vector3& velocity,
                                             float dt)
{
    Vector3 flat = facing;
    flat.y = 0.0f;
    if (flat.LengthSq() < EPSILON) return;

    auto* rb = scene.GetComponent<RigidBodyComponent>();
    if (!rb || !rb->rigidBody) return;

    flat = flat.Normalized();
    const Vector3 side      = Vector3::Cross(Vector3::UP, flat);
    const float   reference = std::max(moveSpeed, 0.01f);
    const float   lateral   = Clamp(Vector3::Dot(velocity, side) / reference, -1.0f, 1.0f);
    const float   forward   = Clamp(Vector3::Dot(velocity, flat) / reference, -1.0f, 1.0f);

    // 前方軸まわりの正回転は右を上へ持ち上げる (右手系の回転)。右へ流れているときに
    // 右を «下げ» たいので符号を返す。機首の方は右軸まわりの正回転がそのまま下向き。
    const Quaternion lean =
        Quaternion::FromAxisAngle(Vector3::FORWARD, -lateral * ToRad(bankAngle)) *
        Quaternion::FromAxisAngle(Vector3::RIGHT,    forward * ToRad(noseAngle));

    const float res = 1.0f - std::exp(-std::max(turnSpeed, 0.0f) * dt);
    rb->rigidBody->SetRotation(Quaternion::Slerp(
        rb->rigidBody->GetRotation(),
        (Quaternion::LookRotation(flat) * lean).Normalized(), res).Normalized());
}

inline bool EnemyMiteComponent::FindGroundY(float& outY) const
{
    GameObject* self = scene.Self();
    if (!self) return false;

    // 自分のコライダーの中から撃つことになるので、自分に当たった分は捨てる。
    // Raycast (最近傍 1 件) だと、その 1 件が自分だったときに地面が永久に見つからない。
    Vector3 origin = transform.worldPosition;
    origin.y += 0.5f;
    const Vector3 down{ 0.0f, -1.0f, 0.0f };
    for (const RaycastHit& hit : physics.RaycastAll(origin, down, groundProbe)) {
        if (hit.gameObject == self) continue;
        if (hit.gameObject && hit.gameObject->tag == "Enemy") continue;
        outY = hit.point.y;
        return true;
    }
    return false;
}

inline void EnemyMiteComponent::ApplyHover()
{
    float groundY = 0.0f;
    if (!FindGroundY(groundY)) {
        // 地面が無い (穴の上・計測失敗)。落下も上昇もさせず、今の高さで留める。
        Vector3 velocity = physics.GetVelocity();
        velocity.y = 0.0f;
        physics.SetVelocity(velocity);
        return;
    }

    const float bob = bobAmplitude *
        std::sin((Time::time * bobHz) * TWO_PI + m_bobSeed);
    const float targetY = groundY + std::max(hoverHeight, 0.0f) + bob;

    Vector3 velocity = physics.GetVelocity();
    velocity.y = std::clamp((targetY - transform.worldPosition.y) * hoverStiffness,
                            -hoverMaxSpeed, hoverMaxSpeed);
    physics.SetVelocity(velocity);
}

} // namespace sandbox
