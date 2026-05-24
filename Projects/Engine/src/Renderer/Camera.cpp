// FBZZ Engine
// Camera.cpp | fbzz::renderer
// ビュー・プロジェクション行列の計算とカメラ姿勢管理
#include <Engine/Renderer/Camera.hpp>
#include <Math/MathUtils.hpp>

namespace fbzz::renderer {

math::Matrix4 Camera::GetViewMatrix() const {
    math::Vector3 forward = GetForward();
    return math::Matrix4::LookAt(m_position, m_position + forward, GetUp());
}

math::Matrix4 Camera::GetProjectionMatrix() const {
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
    math::Vector3 forward = (target - m_position).Normalized();
    m_rotation = math::Quaternion::LookRotation(forward);
}

} // namespace fbzz::renderer
