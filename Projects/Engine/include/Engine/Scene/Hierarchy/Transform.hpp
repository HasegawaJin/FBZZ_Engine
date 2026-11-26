/// @file    Transform.hpp
/// @brief   全 GameObject が持つ位置・回転・スケール。
/// @author  Hasegawa Jin
/// @date    2026-05-21

/// @note local 値はユーザー操作用、world 値は TransformSystem が毎フレーム更新する。
/// @note 親子階層の所有は GameObject が持ち、Transform は姿勢計算に集中する。
#pragma once
#include <Engine/Api.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Math/LinearAlgebra/Quaternion.hpp>
#include <Math/LinearAlgebra/Matrix4.hpp>

namespace fbzz::scene {

struct Transform {
    /// @name ローカル空間
    /// @note スクリプト・エディタから直接変更する。
    /// @{
    math::Vector3 position = math::Vector3::ZERO; ///< 親の原点を基準とした位置 [m]。ルートではワールド位置。
    math::Quaternion rotation = math::Quaternion::Identity(); ///< 親の向きを基準とした回転。ルートではワールド回転。
    math::Vector3 scale = math::Vector3::ONE; ///< 親の各ローカル軸を基準とした倍率。
    /// @}

    /// @name ワールド空間
    /// @note TransformSystem/PhysicsSystem が毎フレーム更新する。直接変更しない。
    /// @{
    math::Vector3 worldPosition = math::Vector3::ZERO; ///< 階層のローカル変換を合成したワールド位置 [m]。
    math::Quaternion worldRotation = math::Quaternion::Identity(); ///< 階層のローカル回転を合成したワールド回転。
    math::Vector3 worldScale = math::Vector3::ONE; ///< 階層の各軸のスケールを合成した倍率。
    /// @}

    /// @name worldRotation から算出
    /// @note `__declspec(property)` により `go.transform.forward` のように直接アクセスできる。
    /// @{
    FBZZ_ENGINE_API math::Vector3 Forward() const;
    FBZZ_ENGINE_API math::Vector3 Up()      const;
    FBZZ_ENGINE_API math::Vector3 Right()   const;
    /// @}

    __declspec(property(get=Forward)) math::Vector3 forward; ///< ローカル前方をワールド回転で写した方向。
    __declspec(property(get=Up)) math::Vector3 up; ///< ローカル上方をワールド回転で写した方向。
    __declspec(property(get=Right)) math::Vector3 right; ///< ローカル右方をワールド回転で写した方向。

    FBZZ_ENGINE_API void Translate(const math::Vector3& delta, bool worldSpace = false);
    FBZZ_ENGINE_API void Rotate(const math::Vector3& eulerDegrees, bool worldSpace = false);
    FBZZ_ENGINE_API void LookAt(const math::Vector3& worldTarget);

    /// @brief 確定したワールド位置・回転・スケールから TRS 行列を作る。
    FBZZ_ENGINE_API math::Matrix4 GetWorldMatrix() const;

};

} // namespace fbzz::scene
