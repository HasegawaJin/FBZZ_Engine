// FBZZ Engine
// Transform.hpp | fbzz::scene
// 全 GameObject が持つ位置・回転・スケール。TransformSystem が毎フレーム world 空間を更新する
#pragma once
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Matrix4.hpp>

namespace fbzz::scene {

struct Transform {
    // ローカル空間 (ゲームコードから直接変更する)
    math::Vector3    localPosition = math::Vector3::ZERO;
    math::Quaternion localRotation = math::Quaternion::Identity();
    math::Vector3    localScale    = math::Vector3::ONE;

    // ワールド空間 (TransformSystem / PhysicsSystem が毎フレーム更新。直接変更しない)
    math::Vector3    position   = math::Vector3::ZERO;
    math::Quaternion rotation   = math::Quaternion::Identity();
    math::Vector3    worldScale = math::Vector3::ONE;

    // Unity: transform.forward / up / right
    math::Vector3 Forward() const;
    math::Vector3 Up()      const;
    math::Vector3 Right()   const;

    // Unity: transform.Translate / Rotate / LookAt
    void Translate(const math::Vector3& delta, bool worldSpace = false);
    void Rotate(const math::Vector3& eulerDegrees, bool worldSpace = false);
    void LookAt(const math::Vector3& worldTarget);

    // position + rotation + localScale から TRS 行列を生成
    math::Matrix4 GetWorldMatrix() const;
};

} // namespace fbzz::scene
