// FBZZ Engine
// TpsCameraComponent.hpp | sandbox
// Third-person camera follow script
#pragma once

#include <Engine/Input/Input.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <string>

namespace sandbox {

struct TpsCameraComponent : fbzz::scene::Script {
    static constexpr const char* TYPE_NAME = "TpsCameraComponent";

    const char* GetTypeName() const override { return TYPE_NAME; }

    std::string targetTag = "Player";
    float distance = 5.0f;
    float height = 1.6f;
    float yaw = 0.0f;
    float pitch = 15.0f;
    float minPitch = -20.0f;
    float maxPitch = 65.0f;
    float mouseSensitivity = 0.2f;
    bool mouseOrbit = true;

    void Reflect(fbzz::scene::IReflector& reflector) override
    {
        reflector.Field("Target Tag", targetTag);
        reflector.Field("Distance", distance);
        reflector.Field("Height", height);
        reflector.Field("Yaw", yaw);
        reflector.Field("Pitch", pitch);
        reflector.Field("Min Pitch", minPitch);
        reflector.Field("Max Pitch", maxPitch);
        reflector.Field("Mouse Sensitivity", mouseSensitivity);
        reflector.Field("Mouse Orbit", mouseOrbit);
    }

    void OnStart() override
    {
        FindTarget();
    }

    // WHY: PhysicsSystem 後の最新プレイヤー位置を使うことで、カメラ位置と
    //      プレイヤーメッシュ位置の 1 フレームずれによる前後ジッターを防ぐ。
    void OnLateUpdate(float dt) override
    {
        (void)dt;
        if (!m_gameObject) return;
        if (!m_target || !m_target->IsValid()) FindTarget();
        if (!m_target) return;

        if (mouseOrbit && fbzz::input::Input::MouseButton(1)) {
            const fbzz::math::Vector2 delta = fbzz::input::Input::MouseDelta();
            yaw += delta.x * mouseSensitivity;
            pitch = fbzz::math::Clamp(pitch + delta.y * mouseSensitivity, minPitch, maxPitch);
        }

        const fbzz::math::Quaternion yawRotation =
            fbzz::math::Quaternion::FromAxisAngle(fbzz::math::Vector3::UP, fbzz::math::ToRad(yaw));
        const fbzz::math::Quaternion pitchRotation =
            fbzz::math::Quaternion::FromAxisAngle(fbzz::math::Vector3::RIGHT, fbzz::math::ToRad(pitch));
        const fbzz::math::Quaternion rotation = (yawRotation * pitchRotation).Normalized();
        const fbzz::math::Vector3 focus = m_target->transform.position + fbzz::math::Vector3::UP * height;
        const fbzz::math::Vector3 cameraPosition = focus - (rotation * fbzz::math::Vector3::FORWARD) * distance;

        m_gameObject->transform.localPosition = cameraPosition;
        m_gameObject->transform.position = cameraPosition;
        m_gameObject->transform.localRotation = rotation;
        m_gameObject->transform.rotation = rotation;
    }

private:
    void FindTarget()
    {
        m_target = nullptr;
        if (!m_scene) return;

        if (!targetTag.empty()) {
            m_target = m_scene->FindWithTag(targetTag);
        }
    }

    fbzz::scene::GameObject* m_target = nullptr;
};

} // namespace sandbox
