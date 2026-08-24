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

    void OnFixedUpdate() override;

protected:
    void OnEnemyStart() override;

private:
    /// 地面からの目標高度へ垂直速度を寄せる。地面が見つからなければ高さを保つ。
    void ApplyHover();
    /// 爪モーションを 1 ステップ進める。当たる瞬間だけダメージを入れる。
    void TickAttack(float dt);
    /// 足元の地面の高さ。見つからなければ false。
    [[nodiscard]] bool FindGroundY(float& outY) const;

    float m_attackTimer  = 0.0f;
    bool  m_attackLanded = false;
    float m_bobSeed      = 0.0f;
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
    m_droppedOnDeath = false;

    // 個体ごとに上下動の位相をずらす。揃っていると群れが 1 つの塊に見える。
    GameObject* self = scene.Self();
    m_bobSeed = static_cast<float>(self ? self->GetID().index : 0u) * 2.39f;
}

inline void EnemyMiteComponent::OnFixedUpdate()
{
    const float dt = time.FixedDeltaTime();

    if (IsPolarityDriven()) {
        // 引力・溜め・ノックバックの最中。高度補正まで掛けると «ギュンッ» が濁る。
        //
        // 振りかけの爪はここで捨てる。Animator 側は Attack ステートを exitTime で
        // 抜け切っているので、残したまま再開すると «モーションが無いのに当たる» になる。
        debugState    = "Polarity";
        m_attackTimer = 0.0f;
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
    FaceDirection(direction, dt);

    if (distanceSq <= attackRange * attackRange) {
        StopHorizontal();
        if (!AttackReady()) {
            debugState = "Cooldown";
            return;
        }
        debugState     = "Attack";
        m_attackTimer  = std::max(attackDuration, 0.05f);
        m_attackLanded = false;
        BeginAttackCooldown();
        animator.SetTrigger(enemyanim::kAttack);
        return;
    }

    Vector3 velocity = physics.GetVelocity();
    velocity.x = direction.x * std::max(moveSpeed, 0.0f);
    velocity.z = direction.z * std::max(moveSpeed, 0.0f);
    physics.SetVelocity(velocity);
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
        FaceDirection(toPlayer, dt);
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

    if (m_attackTimer <= 0.0f) m_attackTimer = 0.0f;
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
