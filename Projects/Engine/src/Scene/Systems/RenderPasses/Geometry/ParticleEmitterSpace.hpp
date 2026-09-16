/// @file    ParticleEmitterSpace.hpp
/// @brief   エミッターのローカル空間とワールド空間の往復。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// WHY 独立させるか: 粒子の «どの空間の値か» はシミュレーション・スポーン・力場・描画の
/// すべてに現れる。TU をまたいで同じ変換が必要になったので、実体を 1 つに保つ。
/// scale の扱いが位置と速度で違う (速度に平行移動を含めない) ため、書き下すたびに
/// 片方で scale を掛け忘れるのが典型的な事故で、そこも 1 か所に閉じる。
#pragma once
#include "Engine/Scene/Transform.hpp"
#include <Math/Quaternion.hpp>
#include <cmath>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

inline math::Vector3 TransformEmitterPoint(const Transform& transform, const math::Vector3& localPoint)
{
    // Transform::position は親基準のローカル座標なので、子 GO に置くと親の移動が乗らない。
    // Particle はワールド空間で保持するので world* から発生点を解決する。
    const math::Vector3 scaledLocal = {
        localPoint.x * transform.worldScale.x,
        localPoint.y * transform.worldScale.y,
        localPoint.z * transform.worldScale.z
    };
    return transform.worldPosition + transform.worldRotation * scaledLocal;
}

inline math::Vector3 TransformEmitterVector(const Transform& transform, const math::Vector3& localVector)
{
    // WHAT: 速度は位置ではないため平行移動を含めず、Emitter のワールド回転だけを適用する。
    return transform.worldRotation * localVector;
}

inline math::Vector3 InverseTransformEmitterPoint(const Transform& transform, const math::Vector3& worldPoint)
{
    const math::Vector3 rotated = transform.worldRotation.Inverse() * (worldPoint - transform.worldPosition);
    return {
        std::fabs(transform.worldScale.x) > 1.0e-6f ? rotated.x / transform.worldScale.x : 0.0f,
        std::fabs(transform.worldScale.y) > 1.0e-6f ? rotated.y / transform.worldScale.y : 0.0f,
        std::fabs(transform.worldScale.z) > 1.0e-6f ? rotated.z / transform.worldScale.z : 0.0f
    };
}

inline math::Vector3 InverseTransformEmitterVector(const Transform& transform, const math::Vector3& worldVector)
{
    return transform.worldRotation.Inverse() * worldVector;
}

} // namespace fbzz::scene
