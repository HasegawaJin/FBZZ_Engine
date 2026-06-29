// FBZZ Engine
// EnemyControllerComponent.hpp | sandbox
// Player を追跡して近距離で攻撃する Enemy 用 Script
#pragma once

#include <Engine/Scene/Script.hpp>
#include "GameVocab.hpp"
#include "HealthComponent.hpp"

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class EnemyControllerComponent : public Script {
    FBZZ_SCRIPT(EnemyControllerComponent)

public:
    FBZZ_GROUP("Target")
    // WHY: 追跡対象は実行中に出現/再生成されうるため、固定参照ではなくタグで都度探索する。
    FBZZ_FIELD(std::string, targetTag, "Player", "Target Tag")

    FBZZ_GROUP("Movement")
    FBZZ_FIELD_RANGE(float, moveSpeed,             2.8f,   "Move Speed",        0.1f, 12.0f)
    FBZZ_FIELD_RANGE(float, stopDistance,          1.15f,  "Stop Distance",     0.1f,  5.0f)
    FBZZ_FIELD_RANGE(float, rotationSpeed,         10.0f,  "Rotation Speed",    0.1f, 30.0f)
    FBZZ_FIELD_RANGE(float, modelYawOffsetDegrees, 180.0f, "Model Yaw Offset",  0.0f, 360.0f)

    FBZZ_GROUP("Attack")
    FBZZ_FIELD_RANGE(float, attackRange,       1.35f, "Attack Range",        0.1f,  5.0f)
    FBZZ_FIELD_RANGE(float, attackCooldown,    1.20f, "Attack Cooldown",     0.1f, 10.0f)
    FBZZ_FIELD_RANGE(float, attackWindupDelay, 0.85f, "Attack Windup Delay", 0.0f,  3.0f)
    FBZZ_FIELD_RANGE(float, knockbackSpeed,    2.50f, "Knockback Speed",     0.0f, 12.0f)

    FBZZ_GROUP("Defense")
    FBZZ_FIELD_RANGE(float, blockRange,    0.95f, "Block Range",    0.1f, 5.0f)
    FBZZ_FIELD_RANGE(float, blockDuration, 0.28f, "Block Duration", 0.1f, 3.0f)
    FBZZ_FIELD_RANGE(float, blockCooldown, 4.00f, "Block Cooldown", 0.1f, 5.0f)

    FBZZ_GROUP("Animator Params")
    FBZZ_FIELD(std::string, paramSpeed,         "Speed",    "Speed Param")
    FBZZ_FIELD(std::string, paramAttackTrigger, "Attack01", "Attack Trigger Param")
    FBZZ_FIELD(std::string, paramBlock,         "Block",    "Block Param")

    void OnStart() override;
    void OnUpdate() override;

private:
    GameObject* FindTarget();
    void FaceTarget(const Vector3& direction);
    void MoveTowardTarget(const Vector3& direction, float distance);
    void StopHorizontalMotion();
    [[nodiscard]] bool TryBlock(const Vector3& direction, float distance);
    void TryAttack(const Vector3& direction, float distance);
    [[nodiscard]] bool IsAttackState() const;
    [[nodiscard]] bool IsTargetSwinging() const;

    GameObject* m_target = nullptr;
    float m_attackTimer = 0.0f;
    float m_attackRequestGrace = 0.0f;
    float m_attackRangeTimer = 0.0f;
    float m_blockTimer = 0.0f;
    float m_blockCooldownTimer = 0.0f;
};

FBZZ_REFLECT(EnemyControllerComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────
inline void EnemyControllerComponent::OnStart()
{
    // WHY: Player と同じモデルを流用する Enemy でも、接触で転倒すると追跡方向と見た目が崩れるため回転を固定する。
    physics.SetFreezeRotation(true, true, true);
}

inline void EnemyControllerComponent::OnUpdate()
{
    if (!transform) return;

    // 死亡後は追跡・攻撃を止め、その場で Death モーションを再生させる。
    if (auto* health = scene.GetScript<HealthComponent>(); health && health->IsDead()) {
        StopHorizontalMotion();
        animator.SetFloat(paramSpeed, 0.0f);
        return;
    }

    if (m_attackTimer > 0.0f)
        m_attackTimer = std::max(0.0f, m_attackTimer - Time::deltaTime);
    if (m_attackRequestGrace > 0.0f)
        m_attackRequestGrace = std::max(0.0f, m_attackRequestGrace - Time::deltaTime);
    if (m_blockTimer > 0.0f)
        m_blockTimer = std::max(0.0f, m_blockTimer - Time::deltaTime);
    if (m_blockCooldownTimer > 0.0f)
        m_blockCooldownTimer = std::max(0.0f, m_blockCooldownTimer - Time::deltaTime);

    GameObject* target = FindTarget();
    if (!target) {
        StopHorizontalMotion();
        animator.SetFloat(paramSpeed, 0.0f);
        animator.SetBool(paramBlock, false);
        return;
    }

    Vector3 toTarget = target->transform.worldPosition - transform.worldPosition;
    toTarget.y = 0.0f;
    const float distanceSq = toTarget.LengthSq();
    if (distanceSq <= EPSILON) {
        StopHorizontalMotion();
        animator.SetFloat(paramSpeed, 0.0f);
        animator.SetBool(paramBlock, false);
        return;
    }

    const float distance = std::sqrtf(distanceSq);
    const Vector3 direction = toTarget / distance;
    if (TryBlock(direction, distance))
        return;

    animator.SetBool(paramBlock, false);
    MoveTowardTarget(direction, distance);
    TryAttack(direction, distance);
}

inline GameObject* EnemyControllerComponent::FindTarget()
{
    if (m_target && m_target->IsValid() && m_target->activeInHierarchy())
        return m_target;

    m_target = targetTag.empty() ? nullptr : scene.FindWithTag(targetTag);
    return m_target;
}

inline void EnemyControllerComponent::FaceTarget(const Vector3& direction)
{
    const Quaternion targetRotation =
        (Quaternion::LookRotation(direction) *
         Quaternion::FromAxisAngle(Vector3::UP, ToRad(modelYawOffsetDegrees))).Normalized();
    const float turnResponse = 1.0f - std::exp(
        -std::max(rotationSpeed, 0.0f) * std::max(Time::deltaTime, 0.0f));
    transform.rotation = Quaternion::Slerp(transform.rotation, targetRotation, turnResponse).Normalized();
}

inline void EnemyControllerComponent::MoveTowardTarget(const Vector3& direction, float distance)
{
    FaceTarget(direction);

    // WHY: 剣のリーチ内に入った時点で停止しないと、攻撃モーション中に Player へ密着して剣先の距離感が崩れる。
    const float desiredStopDistance = std::max(stopDistance, attackRange);
    const bool isMovementLocked = IsAttackState() || m_attackRequestGrace > 0.0f;
    if (isMovementLocked || distance <= desiredStopDistance) {
        StopHorizontalMotion();
        animator.SetFloat(paramSpeed, 0.0f);
        return;
    }

    if (physics.HasRigidBody()) {
        Vector3 velocity = physics.GetVelocity();
        velocity.x = direction.x * moveSpeed;
        velocity.z = direction.z * moveSpeed;
        physics.SetVelocity(velocity);
    } else {
        transform.position += direction * (moveSpeed * Time::deltaTime);
    }
    animator.SetFloat(paramSpeed, moveSpeed);
}

inline void EnemyControllerComponent::StopHorizontalMotion()
{
    if (!physics.HasRigidBody()) return;

    Vector3 velocity = physics.GetVelocity();
    velocity.x = 0.0f;
    velocity.z = 0.0f;
    physics.SetVelocity(velocity);
}

inline bool EnemyControllerComponent::TryBlock(const Vector3& direction, float distance)
{
    if (IsAttackState() || m_attackRequestGrace > 0.0f) {
        animator.SetBool(paramBlock, false);
        return false;
    }

    if (m_blockTimer <= 0.0f && m_blockCooldownTimer <= 0.0f &&
        distance <= blockRange && IsTargetSwinging()) {
        m_blockTimer = std::max(blockDuration, 0.01f);
        m_blockCooldownTimer = std::max(blockCooldown, m_blockTimer);
    }

    if (m_blockTimer <= 0.0f) {
        animator.SetBool(paramBlock, false);
        return false;
    }

    FaceTarget(direction);
    StopHorizontalMotion();
    animator.SetFloat(paramSpeed, 0.0f);
    animator.SetBool(paramBlock, true);
    return true;
}

inline void EnemyControllerComponent::TryAttack(const Vector3& direction, float distance)
{
    if (animator.IsInState(AnimState::PlayerImpact)) return;
    if (animator.IsInState(AnimState::PlayerHit)) return;
    if (distance > attackRange) {
        m_attackRangeTimer = 0.0f;
        return;
    }
    if (IsTargetSwinging()) {
        m_attackRangeTimer = 0.0f;
        return;
    }

    m_attackRangeTimer += Time::deltaTime;
    if (m_attackTimer > 0.0f || m_attackRangeTimer < attackWindupDelay) return;

    m_attackTimer = std::max(attackCooldown, 0.01f);
    m_attackRangeTimer = 0.0f;
    // WHY: Trigger 発火から AnimatorSystem が Slash_01 へ遷移するまで 1 フレーム遅れるため、その間も移動を止める。
    m_attackRequestGrace = 0.20f;
    animator.SetTrigger(paramAttackTrigger);

    // WHAT: HealthComponent がまだ無いため、攻撃成立時の物理的な反応として Player を少し押し返す。
    if (!m_target || knockbackSpeed <= 0.0f) return;
    if (!physics.HasRigidBody(m_target)) return;

    Vector3 velocity = physics.GetVelocity(m_target);
    velocity.x += direction.x * knockbackSpeed;
    velocity.z += direction.z * knockbackSpeed;
    physics.SetVelocity(m_target, velocity);
}

inline bool EnemyControllerComponent::IsAttackState() const
{
    // WHAT: Enemy は Player と同じ AnimatorController を使うため、通常攻撃 State を攻撃中として扱う。
    return animator.IsInState(AnimState::Slash01) ||
           animator.IsInState(AnimState::Slash02) ||
           animator.IsInState(AnimState::Slash03) ||
           animator.IsInState(AnimState::CrouchSlash) ||
           animator.IsInState(AnimState::PlayerImpact) ||
           animator.IsInState(AnimState::PlayerHit);
}

inline bool EnemyControllerComponent::IsTargetSwinging() const
{
    // ターゲット (Player) が攻撃の振り区間に入っているか。共有語彙ヘルパーに集約。
    return m_target && IsAttackSwing(animator, m_target, 0.18f, 0.78f);
}

} // namespace sandbox
