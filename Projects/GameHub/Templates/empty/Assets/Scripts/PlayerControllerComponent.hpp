/// @file    PlayerControllerComponent.hpp
/// @brief   RigidBody ベースの汎用プレイヤーコントローラースクリプト。
/// @author  Hasegawa Jin
/// @date    2026-08-16
#pragma once

#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;
using namespace fbzz::input;
using namespace fbzz::physics;
using fbzz::Time;

namespace sandbox {

class PlayerControllerComponent : public Script {
    FBZZ_SCRIPT(PlayerControllerComponent)

public:
    FBZZ_GROUP("Movement")
    FBZZ_FIELD_RANGE(float, moveSpeed,             4.0f,  "Move Speed",        0.1f, 20.0f)
    FBZZ_FIELD_RANGE(float, sprintMultiplier,       1.8f,  "Sprint Multiplier", 1.0f,  5.0f)
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
    void HandleJump(CharacterControllerComponent* cc, RigidBody* phy);
    void UpdateIK();
    void UpdateSlopeLean(IKSolverComponent& ik, CharacterControllerComponent* cc,
                         RigidBodyComponent* rb);
    Vector3 GetMoveForward() const;
    Vector3 GetMoveRight(const Vector3& forward) const;
    bool m_hasSpineTargetBase = false;
    Vector3 m_spineTargetBase = Vector3::ZERO;
    Vector3 m_smoothedSpineOffset = Vector3::ZERO;
};

// Reflect() をフィールド宣言から自動生成する (旧 .generated.hpp は廃止)。
FBZZ_REFLECT(PlayerControllerComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────
inline void PlayerControllerComponent::OnStart()
{
    // WHY: 接触摩擦トルクによるカプセル傾きで水平ジッターが発生するため全軸フリーズ。
    physics.SetFreezeRotation(true, true, true);
}

inline void PlayerControllerComponent::OnUpdate()
{
    if (!transform) return;
    auto* cc  = scene.GetComponent<CharacterControllerComponent>();
    auto* rb  = scene.GetComponent<RigidBodyComponent>();
    auto* phy = rb && rb->enabled && rb->rigidBody ? rb->rigidBody.get() : nullptr;

    if (cc) {
        cc->Tick(phy, Time::deltaTime);
        animator.SetFloat(paramVerticalSpeed, cc->verticalSpeed);
        animator.SetBool(paramIsGrounded,     cc->isGrounded);
    }
    UpdateIK();
    HandleJump(cc, phy);

    const Vector3 forward = GetMoveForward();
    const Vector3 right   = GetMoveRight(forward);
    Vector3 move = Vector3::ZERO;
    if (input.GetKey(keyForward))  move += forward;
    if (input.GetKey(keyBackward)) move -= forward;
    if (input.GetKey(keyRight))    move += right;
    if (input.GetKey(keyLeft))     move -= right;

    const bool hasInput   = move.LengthSq() > EPSILON;
    const bool isGrounded = cc && cc->isGrounded;
    const Vector3 moveDirection = hasInput ? move.Normalized() : Vector3::ZERO;

    if (hasInput && rotateToMoveDirection) {
        const Quaternion targetRotation =
            (Quaternion::LookRotation(moveDirection) *
             Quaternion::FromAxisAngle(Vector3::UP, ToRad(modelYawOffsetDegrees))).Normalized();
        // WHY: 線形な turnSpeed*dt はフレームレートで応答が変わるため、指数応答で
        //      WASD の急な方向変更を滑らかにしつつ、どの FPS でも同じ旋回感を保つ。
        const float turnResponse = 1.0f - std::exp(
            -std::max(turnSpeed, 0.0f) * std::max(Time::deltaTime, 0.0f));
        transform.rotation = Quaternion::Slerp(
            transform.rotation, targetRotation, turnResponse).Normalized();
    }

    if (phy) {
        // WHY: 水平速度を加速度補間し Y 速度は重力・接触解決に任せる。
        //      着地直後や方向転換でも即 MaxSpeed にならず人間らしい挙動になる。
        Vector3 vel = phy->GetVelocity();
        if (hasInput) {
            const float   speed = input.GetKey(keySprint) ? moveSpeed * sprintMultiplier : moveSpeed;
            const float   accel = isGrounded ? groundAccel : airAccel;
            const float   t     = std::min(1.0f, accel * Time::deltaTime);
            vel.x += (moveDirection.x * speed - vel.x) * t;
            vel.z += (moveDirection.z * speed - vel.z) * t;
        } else {
            const float t = std::min(1.0f, groundDecel * Time::deltaTime);
            vel.x -= vel.x * t;
            vel.z -= vel.z * t;
        }
        phy->SetVelocity(vel);
        animator.SetFloat(paramSpeed, std::sqrtf(vel.x * vel.x + vel.z * vel.z));
    } else {
        if (hasInput) {
            const float   speed = input.GetKey(keySprint) ? moveSpeed * sprintMultiplier : moveSpeed;
            transform.position += moveDirection * (speed * Time::deltaTime);
            animator.SetFloat(paramSpeed, speed);
        } else {
            animator.SetFloat(paramSpeed, 0.0f);
        }
    }
}

inline void PlayerControllerComponent::OnCollisionEnter(const CollisionInfo& info)
{
    if (auto* cc = scene.GetComponent<CharacterControllerComponent>())
        cc->RegisterGroundContact(info);
}

inline void PlayerControllerComponent::OnCollisionStay(const CollisionInfo& info)
{
    OnCollisionEnter(info);
}

inline void PlayerControllerComponent::HandleJump(CharacterControllerComponent* cc, RigidBody* phy)
{
    if (!cc || !cc->isGrounded || !phy) return;
    if (!input.GetKeyDown(keyJump)) return;
    cc->Jump(phy, { 0.0f, jumpForce * phy->GetMass(), 0.0f });
    animator.SetBool(paramIsGrounded, false);
    animator.SetTrigger(paramJumpTrigger);
}

inline void PlayerControllerComponent::UpdateIK()
{
    auto* ik = scene.GetComponent<IKSolverComponent>();
    if (!ik) return;

    // Use Foot IK は足チェーンだけを制御する。Solver 全体を切ると Spine と LookAt まで停止してしまう。
    for (auto& chain : ik->chains) {
        if (chain.type == IKSolverType::FootPlace)
            chain.enabled = useFootIK;
    }

    UpdateSlopeLean(*ik,
                    scene.GetComponent<CharacterControllerComponent>(),
                    scene.GetComponent<RigidBodyComponent>());
}

inline void PlayerControllerComponent::UpdateSlopeLean(
    IKSolverComponent& ik, CharacterControllerComponent* cc, RigidBodyComponent* rb)
{
    IKChain* spine = nullptr;
    for (auto& chain : ik.chains) {
        if (chain.type == IKSolverType::FABRIK) {
            spine = &chain;
            break;
        }
    }
    if (!spine) return;

    auto* target = scene.GetGameObject(spine->targetEntity);
    if (!target) return;
    if (!m_hasSpineTargetBase) {
        m_spineTargetBase = target->transform.position;
        m_hasSpineTargetBase = true;
    }

    Vector3 desiredOffset = Vector3::ZERO;
    if (cc && cc->isGrounded && rb && rb->enabled && rb->rigidBody) {
        Vector3 moveDirection = rb->rigidBody->GetVelocity();
        moveDirection.y = 0.0f;
        if (moveDirection.LengthSq() > 0.01f && cc->groundNormal.y > 0.1f) {
            moveDirection = moveDirection.Normalized();
            const Vector3 normal = cc->groundNormal.Normalized();
            // 地面法線から移動方向の上り勾配 tan(theta) を求め、上り坂だけ上体を進行方向へ倒す。
            const float uphillGrade = std::max(
                0.0f, -Vector3::Dot(normal, moveDirection) / normal.y);
            constexpr float LEAN_PER_GRADE = 0.35f;
            constexpr float MAX_LEAN_OFFSET = 0.20f;
            const float lean = std::min(MAX_LEAN_OFFSET, uphillGrade * LEAN_PER_GRADE);
            const Vector3 localMoveDirection =
                (transform.worldRotation.Inverse() * moveDirection).Normalized();
            desiredOffset = localMoveDirection * lean;
        }
    }

    // 接触法線は物理ステップごとに微動するため、指数応答でターゲットの揺れを抑える。
    constexpr float LEAN_RESPONSE = 8.0f;
    const float response = 1.0f - std::exp(-LEAN_RESPONSE * std::max(Time::deltaTime, 0.0f));
    m_smoothedSpineOffset = Vector3::Lerp(m_smoothedSpineOffset, desiredOffset, response);
    target->transform.position = m_spineTargetBase + m_smoothedSpineOffset;
}

inline Vector3 PlayerControllerComponent::GetMoveForward() const
{
    if (!useCameraForward) return Vector3::FORWARD;
    auto* camGO = scene.GetMainCameraObject();
    if (!camGO) return Vector3::FORWARD;
    Vector3 fwd = camGO->transform.forward;
    fwd.y = 0.0f;
    return fwd.LengthSq() > EPSILON ? fwd.Normalized() : Vector3::FORWARD;
}

inline Vector3 PlayerControllerComponent::GetMoveRight(const Vector3& forward) const
{
    Vector3 right = Vector3::Cross(Vector3::UP, forward);
    return right.LengthSq() > EPSILON ? right.Normalized() : Vector3::RIGHT;
}

} // namespace sandbox
