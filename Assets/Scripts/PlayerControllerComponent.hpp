// FBZZ Engine
// PlayerControllerComponent.hpp | sandbox
// RigidBody ベースの汎用プレイヤーコントローラースクリプト
#pragma once

#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>

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
    FBZZ_FIELD(bool, useCameraForward,      true, "Use Camera Forward")
    FBZZ_FIELD(bool, rotateToMoveDirection, true, "Rotate To Move Dir")

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
    FBZZ_FIELD(std::string, paramLandTrigger,   "Land",         "Land Trigger Param")

    void OnStart() override;
    void OnUpdate() override;
    void OnCollisionEnter(const CollisionInfo& info) override;
    void OnCollisionStay(const CollisionInfo& info) override;

private:
    void HandleJump(CharacterControllerComponent* cc, RigidBody* phy);
    void UpdateIK();
    Vector3 GetMoveForward() const;
    Vector3 GetMoveRight(const Vector3& forward) const;
    bool m_wasGrounded = true;
};

} // namespace sandbox

#include "PlayerControllerComponent.generated.hpp"

// ── 実装 ────────────────────────────────────────────────────────────────────
#ifndef PLAYER_CONTROLLER_IMPL
#define PLAYER_CONTROLLER_IMPL

namespace sandbox {

inline void PlayerControllerComponent::OnStart()
{
    // WHY: 接触摩擦トルクによるカプセル傾きで水平ジッターが発生するため全軸フリーズ。
    physics.SetFreezeRotation(true, true, true);
    if (auto* cc = scene.GetComponent<CharacterControllerComponent>())
        m_wasGrounded = cc->isGrounded;
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
        if (!m_wasGrounded && cc->isGrounded)
            animator.SetTrigger(paramLandTrigger);
        m_wasGrounded = cc->isGrounded;
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

    if (move.LengthSq() <= EPSILON) {
        if (phy) {
            Vector3 vel = phy->GetVelocity();
            vel.x = vel.z = 0.0f;
            phy->SetVelocity(vel);
        }
        animator.SetFloat(paramSpeed, 0.0f);
        return;
    }

    const float speed = input.GetKey(keySprint) ? moveSpeed * sprintMultiplier : moveSpeed;
    const Vector3 direction = move.Normalized();
    animator.SetFloat(paramSpeed, speed);

    if (phy) {
        // WHY: 水平速度だけを上書きし Y 速度は重力・接触解決に任せる。
        Vector3 vel = phy->GetVelocity();
        vel.x = direction.x * speed;
        vel.z = direction.z * speed;
        phy->SetVelocity(vel);
    } else {
        transform.position += direction * (speed * Time::deltaTime);
    }

    if (rotateToMoveDirection) {
        const Quaternion rot = (Quaternion::LookRotation(direction) *
            Quaternion::FromAxisAngle(Vector3::UP, ToRad(modelYawOffsetDegrees))).Normalized();
        transform.rotation = rot;
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
    animator.SetFloat(paramVerticalSpeed, jumpForce);
    animator.SetTrigger(paramJumpTrigger);
}

inline void PlayerControllerComponent::UpdateIK()
{
    // AnimatorSystem が State / BlendTree Motion の IK Weight を連続補間する。
    // WHY: Component を状態ごとに強制 ON/OFF すると Run 境界で足が急に吸着するため。
    if (auto* ik = scene.GetComponent<IKSolverComponent>())
        ik->enabled = true;
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
#endif
