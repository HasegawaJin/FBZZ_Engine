// FBZZ Engine
// Camera.hpp | fbzz::renderer
// ビュー・プロジェクション行列の計算とカメラ姿勢管理
#pragma once

#include <math/Matrix4.hpp>
#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>

namespace fbzz::renderer {

class Camera {
public:
    math::Matrix4 GetViewMatrix()       const;
    math::Matrix4 GetProjectionMatrix() const;
    math::Matrix4 GetViewProjection()   const;

    math::Vector3 GetForward() const;
    math::Vector3 GetRight()   const;
    math::Vector3 GetUp()      const;

    void LookAt(const math::Vector3& target);

    math::Vector3    m_position = { 0.0f, 0.0f, -10.0f };
    math::Quaternion m_rotation;

    float m_fovY   = 60.0f;
    float m_aspect = 16.0f / 9.0f;
    float m_near   = 0.1f;
    float m_far    = 1000.0f;
};

} // namespace fbzz::renderer
