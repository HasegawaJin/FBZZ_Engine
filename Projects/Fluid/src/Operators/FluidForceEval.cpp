/// @file    FluidForceEval.cpp
/// @brief   距離による減衰と外力による速度の変化。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Fluid/Operators/FluidOperatorEval.hpp>
#include "FluidOperatorGeometry.hpp"
#include <Math/Noise/CurlNoise.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::fluid {

namespace {

using operator_geometry_detail::NormalizeOr;
using operator_geometry_detail::ScalarDot;

} /// @note namespace

math::Vector3 FluidForceDelta(const FluidForce& force, const math::Vector3& center,
                              const math::Vector3& position, const math::Vector3& velocity, float time,
                              float deltaTime, const math::Vector3& noiseOffset, int forceIndex,
                              bool isVolumetric, float strengthScale)
{
    const math::Vector3 zero = { 0.0f, 0.0f, 0.0f };
    /// @note 作用範囲と減衰 → 種類ごとの Δv → 2D 平面への投影。
    /// @note 倍率は influence より先に強さへ掛ける (GPU は定数へ詰める段で掛けてある)。
    const float strength = force.strength * strengthScale;
    if (strength == 0.0f) return zero;
    math::Vector3 fromCenter = position - center;
    if (!isVolumetric) fromCenter.z = 0.0f;
    const float distance = std::sqrt(ScalarDot(fromCenter, fromCenter));
    float influence = 1.0f;
    if (force.radius > 0.0f) {
        if (distance >= force.radius) return zero;
        influence = std::pow(1.0f - distance / force.radius, force.falloffPower);
    }
    const math::Vector3 direction = NormalizeOr(force.direction, math::Vector3{ 1.0f, 0.0f, 0.0f });
    const float effectiveStrength = strength * influence;

    math::Vector3 velocityDelta = zero;
    /// @note HLSL 側の共通式は Assets/Shaders/Bake/Fluid/FluidForceMath.hlsli にある。
    switch (force.type) {
    case FluidForceType::Wind:
        velocityDelta = direction * effectiveStrength * deltaTime;
        break;
    case FluidForceType::Attract:
        if (distance >= 1.0e-5f) velocityDelta = -(fromCenter / distance) * effectiveStrength * deltaTime;
        break;
    case FluidForceType::Repulse:
        if (distance >= 1.0e-5f) velocityDelta = (fromCenter / distance) * effectiveStrength * deltaTime;
        break;
    case FluidForceType::Vortex: {
        const math::Vector3 axis = isVolumetric ? direction : math::Vector3{ 0.0f, 0.0f, 1.0f };
        const math::Vector3 tangent = math::Vector3::Cross(axis, fromCenter);
        const float length = std::sqrt(ScalarDot(tangent, tangent));
        if (length >= 1.0e-5f) velocityDelta = (tangent / length) * effectiveStrength * deltaTime;
        break;
    }
    case FluidForceType::Noise: {
        const float index = static_cast<float>(forceIndex);
        const math::Vector3 noisePosition = position * force.noiseFrequency + noiseOffset
            + math::Vector3{ index * 17.31f, -time * force.noiseSpeed, index * 5.73f };
        velocityDelta = math::CurlNoise(noisePosition) * effectiveStrength * deltaTime;
        break;
    }
    case FluidForceType::Drag:
        /// @note 指数減衰で速度を 0 へ近づける。負の強さを使うと速度が増幅するため、Drag では 0 に丸める。
        velocityDelta = -velocity * (1.0f - std::exp(-(std::max)(effectiveStrength, 0.0f) * deltaTime));
        break;
    }
    if (!isVolumetric) velocityDelta.z = 0.0f;
    return velocityDelta;
}

} /// @note namespace fbzz::fluid
