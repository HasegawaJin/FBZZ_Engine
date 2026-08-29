/// @file    Camera.cpp
/// @brief   Camera の行列計算と LookAt 実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// Transform 由来の姿勢から View / Projection / ViewProjection を生成する。
/// 入力制御は DebugCamera や Scene 側に分離する。
#include <Engine/Renderer/Camera.hpp>
#include <Math/MathUtils.hpp>

namespace fbzz::renderer {

math::Matrix4 Camera::GetViewMatrix() const {
    math::Vector3 forward = GetForward();
    return math::Matrix4::LookAt(m_position, m_position + forward, GetUp());
}

math::Matrix4 Camera::GetProjectionMatrix() const {
    if (m_projection == ProjectionMode::Orthographic) {
        const float halfH = (m_orthoHeight > 0.0f ? m_orthoHeight : 1.0f) * 0.5f;
        const float halfW = halfH * m_aspect;
        return math::Matrix4::Orthographic(-halfW, halfW, -halfH, halfH, m_near, m_far);
    }
    float fovRad = math::ToRad(m_fovY);
    return math::Matrix4::Perspective(fovRad, m_aspect, m_near, m_far);
}

math::Matrix4 Camera::GetViewProjection() const {
    return GetProjectionMatrix() * GetViewMatrix();
}

math::Vector3 Camera::GetForward() const {
    return m_rotation * math::Vector3::FORWARD;
}

math::Vector3 Camera::GetRight() const {
    return m_rotation * math::Vector3::RIGHT;
}

math::Vector3 Camera::GetUp() const {
    return m_rotation * math::Vector3::UP;
}

void Camera::LookAt(const math::Vector3& target) {
    math::Vector3 forward = target - m_position;
    if (forward.LengthSq() < 1e-6f) return;
    m_rotation = math::Quaternion::LookRotation(forward.Normalized());
}

} // namespace fbzz::renderer
