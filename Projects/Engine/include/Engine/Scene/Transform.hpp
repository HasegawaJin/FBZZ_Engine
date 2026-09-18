/// @file    Transform.hpp
/// @brief   全 GameObject が持つ位置・回転・スケール。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// local 値はユーザー操作用、world 値は TransformSystem が毎フレーム更新する。
/// 親子階層の所有は GameObject が持ち、Transform は姿勢計算に集中する。
#pragma once
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Matrix4.hpp>

namespace fbzz::scene {

struct Transform {
    /// @name ローカル空間
    /// @note スクリプト・エディタから直接変更する。
    ///@{
    math::Vector3    position = math::Vector3::ZERO;           ///< ローカル座標 (旧 localPosition)
    math::Quaternion rotation = math::Quaternion::Identity();  ///< ローカル回転 (旧 localRotation)
    math::Vector3    scale    = math::Vector3::ONE;            ///< ローカルスケール (旧 localScale)
    ///@}

    /// @name ワールド空間
    /// @note TransformSystem/PhysicsSystem が毎フレーム更新する。直接変更しない。
    ///@{
    math::Vector3    worldPosition = math::Vector3::ZERO;          ///< ワールド座標 (旧 position)
    math::Quaternion worldRotation = math::Quaternion::Identity(); ///< ワールド回転 (旧 rotation)
    math::Vector3    worldScale    = math::Vector3::ONE;
    ///@}

    /// @name worldRotation から算出
    /// @note `__declspec(property)` により `go.transform.forward` のように直接アクセスできる。
    ///@{
    math::Vector3 Forward() const;
    math::Vector3 Up()      const;
    math::Vector3 Right()   const;
    ///@}

    __declspec(property(get=Forward)) math::Vector3 forward;
    __declspec(property(get=Up))      math::Vector3 up;
    __declspec(property(get=Right))   math::Vector3 right;

    void Translate(const math::Vector3& delta, bool worldSpace = false);
    void Rotate(const math::Vector3& eulerDegrees, bool worldSpace = false);
    void LookAt(const math::Vector3& worldTarget);

    /// @brief worldPosition + worldRotation + scale から TRS 行列を生成する。
    math::Matrix4 GetWorldMatrix() const;

};

} // namespace fbzz::scene
