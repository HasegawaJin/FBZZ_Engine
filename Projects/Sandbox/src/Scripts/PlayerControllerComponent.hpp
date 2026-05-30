// FBZZ Engine
// PlayerControllerComponent.hpp | sandbox
// WASD movement script for a player GameObject
#pragma once

#include <Engine/Input/Input.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

namespace sandbox {

struct PlayerControllerComponent : fbzz::scene::Script {
    static constexpr const char* TYPE_NAME = "PlayerControllerComponent";

    const char* GetTypeName() const override { return TYPE_NAME; }

    float moveSpeed = 4.0f;
    float sprintMultiplier = 1.8f;
    float modelYawOffsetDegrees = 180.0f;
    bool useCameraForward = true;
    bool rotateToMoveDirection = true;

    void Reflect(fbzz::scene::IReflector& reflector) override
    {
        reflector.Field("Move Speed", moveSpeed);
        reflector.Field("Sprint Multiplier", sprintMultiplier);
        reflector.Field("Use Camera Forward", useCameraForward);
        reflector.Field("Rotate To Move Direction", rotateToMoveDirection);
    }

    void OnUpdate(float dt) override
    {
        if (!m_gameObject) return;

        const fbzz::math::Vector3 forward = GetMoveForward();
        const fbzz::math::Vector3 right = GetMoveRight(forward);

        fbzz::math::Vector3 move = fbzz::math::Vector3::ZERO;
        if (fbzz::input::Input::KeyHeld(fbzz::input::KeyCode::W)) move += forward;
        if (fbzz::input::Input::KeyHeld(fbzz::input::KeyCode::S)) move -= forward;
        if (fbzz::input::Input::KeyHeld(fbzz::input::KeyCode::D)) move += right;
        if (fbzz::input::Input::KeyHeld(fbzz::input::KeyCode::A)) move -= right;

        auto* rigidBody = m_gameObject->GetComponent<fbzz::scene::RigidBodyComponent>();
        const bool usePhysicsMove = rigidBody && rigidBody->enabled && rigidBody->rigidBody;
        if (move.LengthSq() <= fbzz::math::EPSILON) {
            StopHorizontalPhysicsVelocity(rigidBody);
            SetAnimatorSpeed(0.0f);
            return;
        }

        const float speed = fbzz::input::Input::KeyHeld(fbzz::input::KeyCode::SHIFT)
            ? moveSpeed * sprintMultiplier
            : moveSpeed;
        const fbzz::math::Vector3 direction = move.Normalized();
        SetAnimatorSpeed(speed);

        if (usePhysicsMove) {
            // WHY: カプセルを Transform で直接ワープさせると、坂の接触法線による押し上げを
            // physics::World が速度として解決できない。水平速度だけを入力で上書きし、Y 速度は
            // 重力・接触解決に任せることで、斜面上では通常の衝突解決で登れるようにする。
            fbzz::math::Vector3 velocity = rigidBody->rigidBody->GetVelocity();
            velocity.x = direction.x * speed;
            velocity.z = direction.z * speed;
            rigidBody->rigidBody->SetVelocity(velocity);
        } else {
            // RigidBody を持たないテスト用 GameObject では従来通り Transform 移動にフォールバックする。
            m_gameObject->transform.localPosition += direction * (speed * dt);
            m_gameObject->transform.position = m_gameObject->transform.localPosition;
        }

        if (rotateToMoveDirection) {
            const fbzz::math::Quaternion moveRotation = fbzz::math::Quaternion::LookRotation(direction);
            const fbzz::math::Quaternion modelOffset =
                fbzz::math::Quaternion::FromAxisAngle(fbzz::math::Vector3::UP,
                                                      fbzz::math::ToRad(modelYawOffsetDegrees));
            m_gameObject->transform.localRotation = (moveRotation * modelOffset).Normalized();
            m_gameObject->transform.rotation = m_gameObject->transform.localRotation;
        }
    }

private:
    fbzz::math::Vector3 GetMoveForward() const
    {
        if (!useCameraForward || !m_scene) return fbzz::math::Vector3::FORWARD;

        for (auto& go : m_scene->GameObjects()) {
            auto* camera = go.GetComponent<fbzz::scene::CameraComponent>();
            if (!camera || !camera->enabled || !camera->isMain) continue;

            fbzz::math::Vector3 forward = go.transform.Forward();
            forward.y = 0.0f;
            if (forward.LengthSq() > fbzz::math::EPSILON) return forward.Normalized();
        }

        return fbzz::math::Vector3::FORWARD;
    }

    fbzz::math::Vector3 GetMoveRight(const fbzz::math::Vector3& forward) const
    {
        fbzz::math::Vector3 right = fbzz::math::Vector3::Cross(fbzz::math::Vector3::UP, forward);
        if (right.LengthSq() <= fbzz::math::EPSILON) return fbzz::math::Vector3::RIGHT;
        return right.Normalized();
    }

    void StopHorizontalPhysicsVelocity(fbzz::scene::RigidBodyComponent* rigidBody) const
    {
        if (!rigidBody || !rigidBody->enabled || !rigidBody->rigidBody) return;

        // WHAT: 入力がないフレームでは XZ 速度だけを止め、Y 速度は落下・接地判定に残す。
        fbzz::math::Vector3 velocity = rigidBody->rigidBody->GetVelocity();
        velocity.x = 0.0f;
        velocity.z = 0.0f;
        rigidBody->rigidBody->SetVelocity(velocity);
    }

    void SetAnimatorSpeed(float speed) const
    {
        if (!m_gameObject) return;
        auto* anim = m_gameObject->GetComponent<fbzz::scene::AnimatorComponent>();
        if (anim) anim->SetFloat("Speed", speed);
    }
};

} // namespace sandbox
