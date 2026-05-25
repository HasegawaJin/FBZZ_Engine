// FBZZ Engine
// Transform.cpp | fbzz::scene
// Transform メソッドの実装
// ローカル移動・回転・LookAt とワールド行列生成を提供する。
// 親子階層の再計算は TransformSystem が担当する。
#include "Engine/Scene/Transform.hpp"
#include <Math/MathUtils.hpp>
#include <cmath>

namespace fbzz::scene {

math::Vector3 Transform::Forward() const { return rotation * math::Vector3::FORWARD; }
math::Vector3 Transform::Up()      const { return rotation * math::Vector3::UP;      }
math::Vector3 Transform::Right()   const { return rotation * math::Vector3::RIGHT;   }

void Transform::Translate(const math::Vector3& delta, bool worldSpace) {
    if (worldSpace) {
        math::Quaternion parentRot = rotation * localRotation.Inverse();
        localPosition += parentRot.Inverse() * delta;
    } else {
        localPosition += localRotation * delta;
    }
}

void Transform::Rotate(const math::Vector3& eulerDegrees, bool worldSpace) {
    constexpr float DEG2RAD = 3.14159265f / 180.0f;
    math::Vector3 rad = eulerDegrees * DEG2RAD;
    auto q = math::Quaternion::FromEuler(rad);
    if (worldSpace) {
        math::Quaternion parentRot = rotation * localRotation.Inverse();
        localRotation = (parentRot.Inverse() * q * parentRot) * localRotation;
    } else {
        localRotation = localRotation * q;
    }
    localRotation = localRotation.Normalized();
}

void Transform::LookAt(const math::Vector3& worldTarget) {
    math::Vector3 dir = (worldTarget - position).Normalized();
    if (dir.LengthSq() < 1e-6f) return;
    math::Quaternion worldLook = math::Quaternion::LookRotation(dir);
    math::Quaternion parentRot = rotation * localRotation.Inverse();
    localRotation = parentRot.Inverse() * worldLook;
}

math::Matrix4 Transform::GetWorldMatrix() const {
    return math::Matrix4::TRS(position, rotation, worldScale);
}

} // namespace fbzz::scene
