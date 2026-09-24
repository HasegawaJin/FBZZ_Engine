/// @file    FluidRenderMath.hpp
/// @brief   2D と 3D の Fluid Bake で共有する細部と歪みの計算。
/// @author  Hasegawa Jin
/// @date    2026-09-24
#pragma once

#include <Math/CurlNoise.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector4.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::asset {

inline constexpr float kFluidDistortionScale = 0.42f;
inline constexpr float kFluidDetailLayerOffset = 17.31f;
inline constexpr int kFluidDetailOctaves = 4;
inline constexpr float kFluidDetailAmplitude = 0.9375f;

/// @brief VolumeRaymarch.hlsl の DetailFactor と同じ 4 オクターブの値ノイズを [-1,1] に正規化する。
[[nodiscard]] inline float FluidDetailNoise(const math::Vector3& position)
{
    float sum = 0.0f;
    float amplitude = 0.5f;
    float frequency = 1.0f;
    for (int octave = 0; octave < kFluidDetailOctaves; ++octave) {
        sum += math::ValueNoise3D(position * frequency) * amplitude;
        amplitude *= 0.5f;
        frequency *= 2.03f;
    }
    return sum / kFluidDetailAmplitude;
}

/// @brief 2 層の重みで縮む分を補い、密度と温度に同じ倍率を掛ける。
[[nodiscard]] inline float FluidDetailFactor(float strength, float weight0, float weight1,
                                             float noise0, float noise1)
{
    const float normalization = 1.0f / std::sqrt((std::max)(weight0 * weight0 + weight1 * weight1, 1.0e-4f));
    return (std::max)(0.0f, 1.0f + strength * (weight0 * noise0 + weight1 * noise1) * normalization);
}

/// @brief 速さ 1 で頭打ちにし、上向き速度を画像の下向き V へ反転する。
[[nodiscard]] inline math::Vector4 EncodeFluidDistortion(math::Vector2 screenVelocity, float coverage, float scale)
{
    const float speed = std::sqrt(screenVelocity.x * screenVelocity.x + screenVelocity.y * screenVelocity.y);
    const float gain = speed > 1.0e-5f ? (std::min)(speed, 1.0f) / speed * scale : 0.0f;
    float dx = screenVelocity.x * gain;
    float dy = -screenVelocity.y * gain;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length > 0.5f) {
        dx *= 0.5f / length;
        dy *= 0.5f / length;
    }
    return { std::clamp(0.5f + dx, 0.0f, 1.0f), std::clamp(0.5f + dy, 0.0f, 1.0f), 0.5f,
             std::clamp(coverage, 0.0f, 1.0f) };
}

}
