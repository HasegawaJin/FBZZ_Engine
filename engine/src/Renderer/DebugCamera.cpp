// FBZZ Engine
// DebugCamera.cpp | fbzz::renderer
// Unity Scene View 風デバッグカメラの入力処理
#include <engine/Renderer/DebugCamera.hpp>
#include <engine/Input/Input.hpp>
#include <math/MathUtils.hpp>
#include <cmath>

namespace fbzz::renderer {

void DebugCamera::LookAt(const math::Vector3& target) {
    math::Vector3 forward = (target - camera.m_position).Normalized();
    m_pitch         = math::ToDeg(std::asin(-forward.y));
    m_yaw           = math::ToDeg(std::atan2(forward.x, forward.z));
    m_focusDistance = (target - camera.m_position).Length();
    m_pivot         = target;
    m_dollyVelocity = 0.0f;
    ApplyRotation();
}

void DebugCamera::ApplyRotation() {
    const math::Vector3 worldUp = { 0.0f, 1.0f, 0.0f };
    auto yawQ   = math::Quaternion::FromAxisAngle(worldUp, math::ToRad(m_yaw));
    auto pitchQ = math::Quaternion::FromAxisAngle({ 1.0f, 0.0f, 0.0f }, math::ToRad(m_pitch));
    camera.m_rotation = yawQ * pitchQ;
}

void DebugCamera::ApplyDolly(float dt) {
    if (std::abs(m_dollyVelocity) < 0.001f) {
        m_dollyVelocity = 0.0f;
        return;
    }
    const float dolly = m_dollyVelocity * dt;
    m_focusDistance   = math::Max(0.1f, m_focusDistance - dolly);
    camera.m_position = m_pivot - camera.GetForward() * m_focusDistance;
    m_dollyVelocity  *= std::exp(-scrollDamping * dt);
}

void DebugCamera::Update(float dt) {
    using namespace fbzz::input;

    const math::Vector2 mouseDelta = Input::MouseDelta();
    const float         scroll     = Input::MouseScrollDelta();
    const bool          altHeld    = Input::KeyHeld(KeyCode::ALT);
    const bool          shiftHeld  = Input::KeyHeld(KeyCode::SHIFT);
    const bool          lmb        = Input::MouseButton(0);
    const bool          mmb        = Input::MouseButton(2);
    const bool          rmb        = Input::MouseButton(1);

    const math::Vector3 worldUp = { 0.0f, 1.0f, 0.0f };

    m_dollyVelocity += scroll * scrollSpeed * m_focusDistance;

    // --- 中ドラッグ : オービット回転 / Shift+中 : パン ---
    if (mmb) {
        if (shiftHeld) {
            const float panScale = m_focusDistance * panSensitivity;
            camera.m_position -= camera.GetRight() * (mouseDelta.x * panScale);
            camera.m_position += worldUp            * (mouseDelta.y * panScale);
            m_pivot = camera.m_position + camera.GetForward() * m_focusDistance;
        } else {
            m_yaw   += mouseDelta.x * mouseSens;
            m_pitch += mouseDelta.y * mouseSens;
            m_pitch  = math::Clamp(m_pitch, -89.0f, 89.0f);
            ApplyRotation();
            camera.m_position = m_pivot - camera.GetForward() * m_focusDistance;
        }
        ApplyDolly(dt);
        return;
    }

    // --- Alt + 左ドラッグ : オービット回転 (Unity 互換) ---
    if (altHeld && lmb) {
        m_yaw   += mouseDelta.x * mouseSens;
        m_pitch += mouseDelta.y * mouseSens;
        m_pitch  = math::Clamp(m_pitch, -89.0f, 89.0f);
        ApplyRotation();
        camera.m_position = m_pivot - camera.GetForward() * m_focusDistance;
        ApplyDolly(dt);
        return;
    }

    // --- 右ドラッグ : FPS ルック + フライ ---
    if (rmb) {
        m_yaw   += mouseDelta.x * mouseSens;
        m_pitch += mouseDelta.y * mouseSens;
        m_pitch  = math::Clamp(m_pitch, -89.0f, 89.0f);
        ApplyRotation();

        float speed = moveSpeed;
        if (shiftHeld) speed *= fastMultiplier;

        math::Vector3 move    = math::Vector3::ZERO;
        math::Vector3 forward = camera.GetForward();
        math::Vector3 right   = camera.GetRight();

        if (Input::KeyHeld(KeyCode::W)) move += forward;
        if (Input::KeyHeld(KeyCode::S)) move -= forward;
        if (Input::KeyHeld(KeyCode::D)) move += right;
        if (Input::KeyHeld(KeyCode::A)) move -= right;
        if (Input::KeyHeld(KeyCode::E)) move += worldUp;
        if (Input::KeyHeld(KeyCode::Q)) move -= worldUp;

        if (move.LengthSq() > math::EPSILON)
            camera.m_position += move.Normalized() * (speed * dt);

        m_pivot = camera.m_position + camera.GetForward() * m_focusDistance;
        ApplyDolly(dt);
        return;
    }

    // --- フリー : ドリーのみ ---
    m_pivot = camera.m_position + camera.GetForward() * m_focusDistance;
    ApplyDolly(dt);
}

} // namespace fbzz::renderer
