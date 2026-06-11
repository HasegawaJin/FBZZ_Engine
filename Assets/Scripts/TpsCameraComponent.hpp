// FBZZ Engine
// TpsCameraComponent.hpp | sandbox
#pragma once
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using namespace fbzz::input;
using fbzz::Time;

namespace sandbox {

class TpsCameraComponent : public Script {
    FBZZ_SCRIPT(TpsCameraComponent)

public:
    FBZZ_FIELD(std::string, targetTag,        "Player", "Target Tag")
    FBZZ_FIELD_RANGE(float, distance,          5.0f, "Distance",         0.5f, 30.0f)
    FBZZ_FIELD_RANGE(float, height,            1.6f, "Height",          -5.0f, 10.0f)
    FBZZ_FIELD(float, yaw,                     0.0f, "Yaw")
    FBZZ_FIELD(float, pitch,                  15.0f, "Pitch")
    FBZZ_FIELD_RANGE(float, minPitch,         -20.0f, "Min Pitch",       -90.0f,  0.0f)
    FBZZ_FIELD_RANGE(float, maxPitch,          65.0f, "Max Pitch",         0.0f, 90.0f)
    FBZZ_FIELD_RANGE(float, mouseSensitivity,   0.2f, "Mouse Sensitivity", 0.01f, 5.0f)
    FBZZ_FIELD(bool,  mouseOrbit,              true,  "Mouse Orbit")
    FBZZ_FIELD_RANGE(float, followSpeed,       10.0f, "Follow Speed",      0.0f, 50.0f)

    void OnStart() override;
    void OnLateUpdate() override;

private:
    void FindTarget();
    GameObject* m_target            = nullptr;
    bool        m_hasCameraPosition = false;
};

} // namespace sandbox

#include "TpsCameraComponent.generated.hpp"

// ── 実装 ────────────────────────────────────────────────────────────────────
#ifndef TPS_CAMERA_IMPL
#define TPS_CAMERA_IMPL

namespace sandbox {

inline void TpsCameraComponent::OnStart()
{
    FindTarget();
    m_hasCameraPosition = false;
}

inline void TpsCameraComponent::OnLateUpdate()
{
    if (!transform) return;
    if (!m_target || !m_target->IsValid()) FindTarget();
    if (!m_target) return;

    if (mouseOrbit && input.MouseButton(1)) {
        const Vector2 delta = input.GetMouseDelta();
        yaw   += delta.x * mouseSensitivity;
        pitch  = Clamp(pitch + delta.y * mouseSensitivity, minPitch, maxPitch);
    }

    const Quaternion yawRot   = Quaternion::FromAxisAngle(Vector3::UP,    ToRad(yaw));
    const Quaternion pitchRot = Quaternion::FromAxisAngle(Vector3::RIGHT, ToRad(pitch));
    const Quaternion rotation = (yawRot * pitchRot).Normalized();
    const Vector3    focus    = m_target->transform.worldPosition + Vector3::UP * height;
    const Vector3    targetCamPos = focus - (rotation * Vector3::FORWARD) * distance;

    // WHY: 指数補間で dt に依存した補間率を計算。初回のみスナップして位置ずれを防ぐ。
    const float safeDt          = Max(Time::deltaTime, 0.0f);
    const float safeFollowSpeed = Max(followSpeed, 0.0f);
    const float followT = safeFollowSpeed <= EPSILON
        ? 1.0f
        : Clamp01(1.0f - Pow(0.001f, safeDt * safeFollowSpeed));
    const Vector3 camPos = m_hasCameraPosition
        ? Vector3::Lerp(transform.worldPosition, targetCamPos, followT)
        : targetCamPos;

    transform.position      = camPos;
    transform.rotation      = rotation;
    m_hasCameraPosition     = true;
}

inline void TpsCameraComponent::FindTarget()
{
    m_target = targetTag.empty() ? nullptr : scene.FindWithTag(targetTag);
}

} // namespace sandbox
#endif
