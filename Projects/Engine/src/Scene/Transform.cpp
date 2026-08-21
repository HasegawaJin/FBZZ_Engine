// FBZZ Engine
// Transform.cpp | fbzz::scene
// Transform メソッドの実装
// ローカル移動・回転・LookAt とワールド行列生成を提供する。
// 親子階層の再計算は TransformSystem が担当する。
#include "Engine/Scene/Transform.hpp"
#include <Math/MathUtils.hpp>
#include <cmath>

namespace fbzz::scene {

math::Vector3 Transform::Forward() const { return worldRotation * math::Vector3::FORWARD; }
math::Vector3 Transform::Up()      const { return worldRotation * math::Vector3::UP;      }
math::Vector3 Transform::Right()   const { return worldRotation * math::Vector3::RIGHT;   }

void Transform::Translate(const math::Vector3& delta, bool worldSpace) {
    if (worldSpace) {
        math::Quaternion parentRot = worldRotation * rotation.Inverse();
        position += parentRot.Inverse() * delta;
    } else {
        position += rotation * delta;
    }
}

void Transform::Rotate(const math::Vector3& eulerDegrees, bool worldSpace) {
    math::Vector3 rad = eulerDegrees * math::DEG2RAD;
    auto q = math::Quaternion::FromEuler(rad);
    if (worldSpace) {
        math::Quaternion parentRot = worldRotation * rotation.Inverse();
        rotation = (parentRot.Inverse() * q * parentRot) * rotation;
    } else {
        rotation = rotation * q;
    }
    rotation = rotation.Normalized();
}

void Transform::LookAt(const math::Vector3& worldTarget) {
    math::Vector3 dir = (worldTarget - worldPosition).Normalized();
    if (dir.LengthSq() < 1e-6f) return;
    math::Quaternion worldLook = math::Quaternion::LookRotation(dir);
    math::Quaternion parentRot = worldRotation * rotation.Inverse();
    rotation = parentRot.Inverse() * worldLook;
}

math::Matrix4 Transform::GetWorldMatrix() const {
    return math::Matrix4::TRS(worldPosition, worldRotation, worldScale);
}

} // namespace fbzz::scene
