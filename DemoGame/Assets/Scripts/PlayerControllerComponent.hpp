// FBZZ Engine
// PlayerControllerComponent.hpp | sandbox
// PhysicsProxy ベースの汎用プレイヤーコントローラースクリプト
#pragma once

#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;
using namespace fbzz::input;
using fbzz::Time;

namespace sandbox {

class PlayerControllerComponent : public Script {
    FBZZ_SCRIPT(PlayerControllerComponent)

public:
    FBZZ_GROUP("Movement")
    FBZZ_FIELD_RANGE(float, moveSpeed,             4.0f,  "Move Speed",        0.1f, 20.0f)
    FBZZ_FIELD_RANGE(float, sprintMultiplier,       1.8f,  "Sprint Multiplier", 1.0f,  5.0f)
    FBZZ_FIELD_RANGE(float, strafeSpeedMultiplier,  0.5f,  "Strafe Speed Mult", 0.1f,  1.0f)
    FBZZ_FIELD_RANGE(float, jumpForce,              5.0f,  "Jump Force",        0.1f, 30.0f)
    FBZZ_FIELD_RANGE(float, modelYawOffsetDegrees, 180.0f, "Model Yaw Offset",  0.0f, 360.0f)
    FBZZ_FIELD_RANGE(float, groundAccel,            15.0f, "Ground Accel",       1.0f, 100.0f)
    FBZZ_FIELD_RANGE(float, groundDecel,            20.0f, "Ground Decel",       1.0f, 100.0f)
    FBZZ_FIELD_RANGE(float, airAccel,                3.0f, "Air Accel",          0.0f,  50.0f)
    FBZZ_FIELD_RANGE(float, turnSpeed,              12.0f, "Turn Speed",         0.1f,  30.0f)
    FBZZ_FIELD(bool, useCameraForward,      true, "Use Camera Forward")
    FBZZ_FIELD(bool, rotateToMoveDirection, true, "Rotate To Move Dir")
    FBZZ_FIELD(bool, useFootIK,             true, "Use Foot IK")
    FBZZ_GROUP("Key Bindings")
    FBZZ_FIELD(KeyCode, keyForward,  KeyCode::W,     "Forward")
    FBZZ_FIELD(KeyCode, keyBackward, KeyCode::S,     "Backward")
    FBZZ_FIELD(KeyCode, keyLeft,     KeyCode::A,     "Left")
    FBZZ_FIELD(KeyCode, keyRight,    KeyCode::D,     "Right")
    FBZZ_FIELD(KeyCode, keyJump,     KeyCode::SPACE, "Jump")
    FBZZ_FIELD(KeyCode, keySprint,   KeyCode::SHIFT, "Sprint")

    FBZZ_GROUP("Animator Params")
    FBZZ_FIELD(std::string, paramSpeed,         "Speed",        "Speed Param")
    FBZZ_FIELD(std::string, paramVerticalSpeed, "VerticalSpeed","Vertical Speed Param")
    FBZZ_FIELD(std::string, paramIsGrounded,    "IsGrounded",   "IsGrounded Param")
    FBZZ_FIELD(std::string, paramJumpTrigger,   "Jump",         "Jump Trigger Param")

    void OnStart() override;
    void OnUpdate() override;
    void OnCollisionEnter(const CollisionInfo& info) override;
    void OnCollisionStay(const CollisionInfo& info) override;

private:
    void HandleJump(bool isGrounded);
    void HandleCombat(bool isGrounded);
    [[nodiscard]] bool IsComboAttackState() const;
    void StartComboAttack();
    Vector3 GetMoveForward() const;
    Vector3 GetMoveRight(const Vector3& forward) const;
    int m_nextComboIndex = 0;
    float m_comboWindowRemaining = 0.0f;
    bool m_wasComboAttacking = false;
    bool m_comboAttackRequested = false;
    bool m_comboAttackQueued = false;
    int m_requestedComboIndex = -1;
};

} // namespace sandbox

#include "PlayerControllerComponent.generated.hpp"

// ── 実装 ────────────────────────────────────────────────────────────────────
#ifndef PlayerControllerComponent_IMPL
#define PlayerControllerComponent_IMPL

namespace sandbox {

void PlayerControllerComponent::OnStart()
{
    // WHY: 接触摩擦トルクによるカプセル傾きで水平ジッターが発生するため全軸フリーズ。
    physics.SetFreezeRotation(true, true, true);
}

void PlayerControllerComponent::OnUpdate()
{
    if (!transform) return;
    character.Tick(Time::deltaTime);
    const bool isGrounded = character.IsGrounded();
    animator.SetFloat(paramVerticalSpeed, character.GetVerticalSpeed());
    animator.SetBool(paramIsGrounded, isGrounded);

    HandleCombat(isGrounded);
    HandleJump(isGrounded);

    const Vector3 forward = GetMoveForward();
    const Vector3 right   = GetMoveRight(forward);
    Vector3 move = Vector3::ZERO;
    const bool isStrafing = animator.IsInState("Block") || animator.GetBool("Block");
    const bool isMovementLocked =
        IsComboAttackState() || animator.IsInState("CrouchSlash") ||
        animator.IsInState("Land") || animator.IsInState("PlayerImpact") ||
        animator.IsInState("PlayerHit") ||
        m_comboAttackRequested;
    // CrouchIdle / CrouchSlash に移動クリップがないため、C 押下中は水平移動を停止する。
    // Slash / Land 中も入力と慣性移動を止め、モーションの足運びと物理位置を一致させる。
    if (!input.GetKey(KeyCode::C) && !isMovementLocked) {
        if (input.GetKey(keyForward))  move += forward;
        if (input.GetKey(keyBackward)) move -= forward;
        if (input.GetKey(keyRight))    move += right;
        if (input.GetKey(keyLeft))     move -= right;
    }

    const bool hasInput   = move.LengthSq() > EPSILON;
    const Vector3 moveDirection = hasInput ? move.Normalized() : Vector3::ZERO;

    if (rotateToMoveDirection && (hasInput || isStrafing)) {
        // WHY: 右クリック中 (isStrafing) のみカメラ前方を向く。
        //      A/D 単体では通常の moveDirection へ向き、カメラ前方固定にはしない。
        const Vector3 faceDirection = isStrafing ? GetMoveForward() : moveDirection;
        const Quaternion targetRotation =
            (Quaternion::LookRotation(faceDirection) *
             Quaternion::FromAxisAngle(Vector3::UP, ToRad(modelYawOffsetDegrees))).Normalized();
        // WHY: 線形な turnSpeed*dt はフレームレートで応答が変わるため、指数応答で
        //      WASD の急な方向変更を滑らかにしつつ、どの FPS でも同じ旋回感を保つ。
        const float turnResponse = 1.0f - std::exp(
            -std::max(turnSpeed, 0.0f) * std::max(Time::deltaTime, 0.0f));
        transform.rotation = Quaternion::Slerp(
            transform.rotation, targetRotation, turnResponse).Normalized();
    }

    if (physics.HasRigidBody()) {
        // WHY: 水平速度を加速度補間し Y 速度は重力・接触解決に任せる。
        //      着地直後や方向転換でも即 MaxSpeed にならず人間らしい挙動になる。
        Vector3 vel = physics.GetVelocity();
        if (isMovementLocked) {
            // WHY: 入力だけ無効にすると直前の速度で滑るため、攻撃・着地中は水平速度も即時停止する。
            vel.x = 0.0f;
            vel.z = 0.0f;
        } else if (hasInput) {
            const float   baseSpeed = (isStrafing ? moveSpeed * strafeSpeedMultiplier : moveSpeed);
            const float   speed = input.GetKey(keySprint) ? baseSpeed * sprintMultiplier : baseSpeed;
            const float   accel = isGrounded ? groundAccel : airAccel;
            const float   t     = std::min(1.0f, accel * Time::deltaTime);
            vel.x += (moveDirection.x * speed - vel.x) * t;
            vel.z += (moveDirection.z * speed - vel.z) * t;
        } else {
            const float t = std::min(1.0f, groundDecel * Time::deltaTime);
            vel.x -= vel.x * t;
            vel.z -= vel.z * t;
        }
        physics.SetVelocity(vel);
        animator.SetFloat(paramSpeed, std::sqrtf(vel.x * vel.x + vel.z * vel.z));
    } else {
        if (hasInput) {
            const float   baseSpeed = (isStrafing ? moveSpeed * strafeSpeedMultiplier : moveSpeed);
            const float   speed = input.GetKey(keySprint) ? baseSpeed * sprintMultiplier : baseSpeed;
            transform.position += moveDirection * (speed * Time::deltaTime);
            animator.SetFloat(paramSpeed, speed);
        } else {
            animator.SetFloat(paramSpeed, 0.0f);
        }
    }
}

void PlayerControllerComponent::OnCollisionEnter(const CollisionInfo& info)
{
    character.RegisterGroundContact(info);
}

void PlayerControllerComponent::OnCollisionStay(const CollisionInfo& info)
{
    OnCollisionEnter(info);
}

void PlayerControllerComponent::HandleJump(bool isGrounded)
{
    if (!isGrounded || !physics.HasRigidBody()) return;
    // WHY: しゃがみ姿勢のまま JumpUp へ遷移すると下半身が急伸するため、C 押下中は跳ばない。
    if (input.GetKey(KeyCode::C)) return;
    if (!input.GetKeyDown(keyJump)) return;
    character.Jump({ 0.0f, jumpForce * physics.GetMass(), 0.0f });
    animator.SetBool(paramIsGrounded, false);
    animator.SetTrigger(paramJumpTrigger);
}

void PlayerControllerComponent::HandleCombat(bool isGrounded)
{
    // 各 Slash クリップの実際の振り区間だけを正規化時間で追跡する。
    // WHY: 固定秒数ではクリップ前半だけで停止し、剣先が振り切った位置まで残像が届かない。
    const bool isComboAttacking = IsComboAttackState();
    const bool isCrouchAttacking = animator.IsInState("CrouchSlash");
    const bool isImpacting = animator.IsInState("PlayerImpact");
    const bool isHitReacting = animator.IsInState("PlayerHit");
    const bool isAttacking = isComboAttacking || isCrouchAttacking;
    const bool isCombatLocked = isAttacking || isImpacting || isHitReacting;
    const float attackTime = isAttacking ? animator.GetNormalizedTime() : 0.0f;
    // 空中で防御姿勢へ急遷移するとJump/Fallを中断するため、地上時だけ戦闘入力を許可する。
    const bool canUseCombat = isGrounded;
    const bool isCrouching = canUseCombat && input.GetKey(KeyCode::C);
    // WHY: Slash 中の Block 遷移は攻撃を途中で切り、コンボと剣筋を不自然に中断するため禁止する。
    const bool isBlocking =
        canUseCombat && !isCrouching && !isCombatLocked && input.MouseButton(MouseBtn::Right);
    animator.SetBool("Crouch", isCrouching);
    animator.SetBool("Block", isBlocking);

    // AnimatorSystem の状態反映はスクリプト更新より後なので、Trigger 発火から State 進入までを
    // requested で保持し、待機中にコンボ番号が誤ってリセットされることを防ぐ。
    if (isComboAttacking) {
        const bool enteredRequestedState =
            (m_requestedComboIndex == 0 && animator.IsInState("Slash_01")) ||
            (m_requestedComboIndex == 1 && animator.IsInState("Slash_02")) ||
            (m_requestedComboIndex == 2 && animator.IsInState("Slash_03"));
        if (enteredRequestedState) {
            m_comboAttackRequested = false;
            m_requestedComboIndex = -1;
        }

        const bool canQueueNextSlash =
            !m_comboAttackRequested && !animator.IsInState("Slash_03");
        if (canQueueNextSlash && input.MouseButtonDown(MouseBtn::Left))
            m_comboAttackQueued = true;

        // WHAT: 後半までに受けた入力を保持し、終了前から次段へクロスフェードする。
        constexpr float COMBO_BLEND_START_NORMALIZED_TIME = 0.62f;
        if (m_comboAttackQueued && attackTime >= COMBO_BLEND_START_NORMALIZED_TIME) {
            m_comboAttackQueued = false;
            StartComboAttack();
        }
    } else if (m_wasComboAttacking) {
        constexpr float COMBO_CONTINUATION_SECONDS = 0.45f;
        m_comboWindowRemaining = COMBO_CONTINUATION_SECONDS;
    } else if (!m_comboAttackRequested && m_comboWindowRemaining > 0.0f) {
        m_comboWindowRemaining = std::max(0.0f, m_comboWindowRemaining - Time::deltaTime);
        if (m_comboWindowRemaining <= 0.0f) {
            m_nextComboIndex = 0;
            m_comboAttackQueued = false;
        }
    }

    // しゃがみ攻撃を優先し、通常コンボは再生中に予約されなかった場合だけ終了後の猶予で継続する。
    if (isCrouching && animator.IsInState("CrouchIdle") && !isCombatLocked &&
        input.MouseButtonDown(MouseBtn::Left)) {
        // WHAT: しゃがみ攻撃は通常3段コンボと独立させ、C解除後のコンボ段数へ影響させない。
        animator.SetTrigger("CrouchAttack");
    } else if (canUseCombat && !isCrouching && !isBlocking && !isCombatLocked &&
        !m_comboAttackRequested &&
        input.MouseButtonDown(MouseBtn::Left)) {
        if (m_comboWindowRemaining <= 0.0f) m_nextComboIndex = 0;
        StartComboAttack();
    }
    m_wasComboAttacking = isComboAttacking;
}

bool PlayerControllerComponent::IsComboAttackState() const
{
    // WHAT: 3種類の Slash State をひとつの攻撃中判定として扱う。
    return animator.IsInState("Slash_01") ||
           animator.IsInState("Slash_02") ||
           animator.IsInState("Slash_03");
}

void PlayerControllerComponent::StartComboAttack()
{
    // WHAT: 現在のコンボ番号に対応する Trigger を発火し、次の受付段を循環させる。
    m_requestedComboIndex = m_nextComboIndex;
    switch (m_nextComboIndex) {
    case 1:  animator.SetTrigger("Attack02"); break;
    case 2:  animator.SetTrigger("Attack03"); break;
    default: animator.SetTrigger("Attack01"); break;
    }
    m_nextComboIndex = (m_nextComboIndex + 1) % 3;
    m_comboWindowRemaining = 0.0f;
    m_comboAttackRequested = true;
}

Vector3 PlayerControllerComponent::GetMoveForward() const
{
    if (!useCameraForward) return Vector3::FORWARD;
    auto* camGO = scene.GetMainCameraObject();
    if (!camGO) return Vector3::FORWARD;
    Vector3 fwd = camGO->transform.forward;
    fwd.y = 0.0f;
    return fwd.LengthSq() > EPSILON ? fwd.Normalized() : Vector3::FORWARD;
}

Vector3 PlayerControllerComponent::GetMoveRight(const Vector3& forward) const
{
    Vector3 right = Vector3::Cross(Vector3::UP, forward);
    return right.LengthSq() > EPSILON ? right.Normalized() : Vector3::RIGHT;
}

} // namespace sandbox
#endif
