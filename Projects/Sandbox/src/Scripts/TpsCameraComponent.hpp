// FBZZ Engine
// TpsCameraComponent.hpp | sandbox
// Third-person camera follow script
#pragma once

// WHY: Sandbox スクリプトは engine 層からインクルードされない末端ヘッダのため、
//      using namespace を許可する。詳細は AGENTS.md を参照。
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using namespace fbzz::input;

namespace sandbox {

class TpsCameraComponent : public Script {
public:
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

    void Reflect(IReflector& reflector) override
    {
        reflector.Field("Target Tag",        targetTag);
        reflector.Field("Distance",          distance);
        reflector.Field("Height",            height);
        reflector.Field("Yaw",               yaw);
        reflector.Field("Pitch",             pitch);
        reflector.Field("Min Pitch",         minPitch);
        reflector.Field("Max Pitch",         maxPitch);
        reflector.Field("Mouse Sensitivity", mouseSensitivity);
        reflector.Field("Mouse Orbit",       mouseOrbit);
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

        if (mouseOrbit && Input::MouseButton(1)) {
            const Vector2 delta = Input::MouseDelta();
            yaw   += delta.x * mouseSensitivity;
            pitch  = Clamp(pitch + delta.y * mouseSensitivity, minPitch, maxPitch);
        }

        const Quaternion yawRot   = Quaternion::FromAxisAngle(Vector3::UP,    ToRad(yaw));
        const Quaternion pitchRot = Quaternion::FromAxisAngle(Vector3::RIGHT, ToRad(pitch));
        const Quaternion rotation = (yawRot * pitchRot).Normalized();
        const Vector3    focus    = m_target->transform.position + Vector3::UP * height;
        const Vector3    camPos   = focus - (rotation * Vector3::FORWARD) * distance;

        transform->localPosition = camPos;
        transform->position      = camPos;
        transform->localRotation = rotation;
        transform->rotation      = rotation;
    }

private:
    void FindTarget()
    {
        m_target = targetTag.empty() ? nullptr : FindWithTag(targetTag);
    }

    GameObject* m_target = nullptr;
};

} // namespace sandbox
