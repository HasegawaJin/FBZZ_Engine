// FBZZ Engine
// PlayerControllerComponent.hpp | sandbox
// PhysicsProxy ベースのプレイヤーコントローラースクリプト
//
// MyPlayer (4 クリップ: Idle / Walk / Run / Idle_Combat) 用の最小構成。
//   - WASD + Shift : Locomotion (Speed パラメータで Idle/Walk/Run をブレンド)
//   - 右クリック保持: 戦闘構え (Combat=true → Idle_Combat)。構え中はカメラ前方を
//     向いたままストレイフ移動し、移動速度を strafeSpeedMultiplier で落とす。
// 攻撃・ジャンプ・被弾などは対応モーションが無いため未実装。
// モーション追加時は GameVocab に語彙を足し、ここから拡張する。
#pragma once

#include <Engine/Scene/Script.hpp>
#include "GameVocab.hpp"
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
    FBZZ_FIELD_RANGE(float, modelYawOffsetDegrees, 180.0f, "Model Yaw Offset",  0.0f, 360.0f)
    FBZZ_FIELD_RANGE(float, groundAccel,            15.0f, "Ground Accel",       1.0f, 100.0f)
    FBZZ_FIELD_RANGE(float, groundDecel,            20.0f, "Ground Decel",       1.0f, 100.0f)
    FBZZ_FIELD_RANGE(float, turnSpeed,              12.0f, "Turn Speed",         0.1f,  30.0f)
    FBZZ_FIELD(bool, useCameraForward,      true, "Use Camera Forward")
    FBZZ_FIELD(bool, rotateToMoveDirection, true, "Rotate To Move Dir")
    FBZZ_FIELD(bool, useFootIK,             true, "Use Foot IK")

    FBZZ_GROUP("Key Bindings")
    FBZZ_FIELD(KeyCode, keyForward,  KeyCode::W,     "Forward")
    FBZZ_FIELD(KeyCode, keyBackward, KeyCode::S,     "Backward")
    FBZZ_FIELD(KeyCode, keyLeft,     KeyCode::A,     "Left")
    FBZZ_FIELD(KeyCode, keyRight,    KeyCode::D,     "Right")
    FBZZ_FIELD(KeyCode, keySprint,   KeyCode::SHIFT, "Sprint")

    FBZZ_GROUP("Animator Params")
    FBZZ_FIELD(std::string, paramSpeed,  "Speed",  "Speed Param")
    FBZZ_FIELD(std::string, paramCombat, "Combat", "Combat Param")

    void OnStart() override;
    void OnUpdate() override;
    void OnCollisionEnter(const CollisionInfo& info) override;
    void OnCollisionStay(const CollisionInfo& info) override;

private:
    Vector3 GetMoveForward() const;
    Vector3 GetMoveRight(const Vector3& forward) const;
};

FBZZ_REFLECT(PlayerControllerComponent)

inline void PlayerControllerComponent::OnStart()
{
    // WHY: 接触摩擦トルクによるカプセル傾きで水平ジッターが発生するため全軸フリーズ。
    physics.SetFreezeRotation(true, true, true);
}

inline void PlayerControllerComponent::OnUpdate()
{
    if (!transform) return;
    // 接地状態の更新 (重力・地面追従は CharacterProxy に任せる)。
    character.Tick(Time::deltaTime);

    // 戦闘構え: 右クリック保持で Idle_Combat へ。構え解除は Animator 側の遷移条件に任せる。
    const bool isCombat = input.MouseButton(MouseBtn::Right);
    animator.SetBool(paramCombat, isCombat);

    const Vector3 forward = GetMoveForward();
    const Vector3 right   = GetMoveRight(forward);
    Vector3 move = Vector3::ZERO;
    if (input.GetKey(keyForward))  move += forward;
    if (input.GetKey(keyBackward)) move -= forward;
    if (input.GetKey(keyRight))    move += right;
    if (input.GetKey(keyLeft))     move -= right;

    const bool hasInput = move.LengthSq() > EPSILON;
    const Vector3 moveDirection = hasInput ? move.Normalized() : Vector3::ZERO;

    if (rotateToMoveDirection && (hasInput || isCombat)) {
        // WHY: 構え中 (isCombat) はカメラ前方を向いたままストレイフする。
        //      通常移動では進行方向へ向き、カメラ前方固定にはしない。
        const Vector3 faceDirection = isCombat ? GetMoveForward() : moveDirection;
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

    // 構え中は移動アニメが無い (Idle_Combat は静止クリップ) ため、滑り量を抑える。
    const float baseSpeed = isCombat ? moveSpeed * strafeSpeedMultiplier : moveSpeed;

    if (physics.HasRigidBody()) {
        // WHY: 水平速度を加速度補間し Y 速度は重力・接触解決に任せる。
        //      方向転換でも即 MaxSpeed にならず人間らしい挙動になる。
        Vector3 vel = physics.GetVelocity();
        if (hasInput) {
            const float speed = input.GetKey(keySprint) && !isCombat
                ? baseSpeed * sprintMultiplier : baseSpeed;
            const float t = std::min(1.0f, groundAccel * Time::deltaTime);
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
            const float speed = input.GetKey(keySprint) && !isCombat
                ? baseSpeed * sprintMultiplier : baseSpeed;
            transform.position += moveDirection * (speed * Time::deltaTime);
            animator.SetFloat(paramSpeed, speed);
        } else {
            animator.SetFloat(paramSpeed, 0.0f);
        }
    }
}

inline void PlayerControllerComponent::OnCollisionEnter(const CollisionInfo& info)
{
    character.RegisterGroundContact(info);
}

inline void PlayerControllerComponent::OnCollisionStay(const CollisionInfo& info)
{
    OnCollisionEnter(info);
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
