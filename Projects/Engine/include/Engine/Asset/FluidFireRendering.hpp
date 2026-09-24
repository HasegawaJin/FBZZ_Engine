/// @file    FluidFireRendering.hpp
/// @brief   Fluid Fire の色 LUT、放射量、ソフトニーを共有する。
/// @author  Hasegawa Jin
/// @date    2026-09-24
#pragma once

#include <Engine/Scene/Components/ParticleColorSpace.hpp>
#include <Math/Vector3.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace fbzz::asset {

inline constexpr std::size_t kFluidFireColorLutSamples = 256;
inline constexpr float kFluidFireReferencePathLength = 2.0f;

/// @brief ParticleBlackbodyChroma と同じ色を 2D と HLSL で補間する LUT。
class FluidFireColorLut {
public:
    FluidFireColorLut() = default;

    explicit FluidFireColorLut(float maxKelvin)
    {
        Reset(maxKelvin);
    }

    void Reset(float maxKelvin)
    {
        m_maxKelvin = (std::max)(maxKelvin, 1.0f);
        for (std::size_t i = 0; i < m_chroma.size(); ++i) {
            const float kelvin = m_maxKelvin * static_cast<float>(i)
                / static_cast<float>(m_chroma.size() - 1);
            m_chroma[i] = scene::ParticleBlackbodyChroma(kelvin);
        }
    }

    [[nodiscard]] math::Vector3 Chroma(float kelvin) const
    {
        const float position = std::clamp(kelvin / m_maxKelvin, 0.0f, 1.0f)
            * static_cast<float>(m_chroma.size() - 1);
        const std::size_t index = (std::min)(static_cast<std::size_t>(position), m_chroma.size() - 2);
        const float t = position - static_cast<float>(index);
        const math::Vector3& a = m_chroma[index];
        const math::Vector3& b = m_chroma[index + 1];
        return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
    }

    [[nodiscard]] const std::array<math::Vector3, kFluidFireColorLutSamples>& Samples() const noexcept
    {
        return m_chroma;
    }

    [[nodiscard]] float MaxKelvin() const noexcept
    {
        return m_maxKelvin;
    }

private:
    float m_maxKelvin = 1.0f;
    std::array<math::Vector3, kFluidFireColorLutSamples> m_chroma{};
};

/// @brief 2D Fire と VolumeRaymarch.hlsl が使う黒体放射密度 q(T)。
/// @see https://developer.nvidia.com/gpugems/gpugems3/part-v-physics-simulation/chapter-30-real-time-simulation-and-rendering-3d-fluids GPU Gems 3, §30.3.1 Fire.
[[nodiscard]] inline math::Vector3 FluidFireBlackbodyRadiance(float temperature, float intensity,
                                                              const math::Vector3& chroma)
{
    const float t = (std::max)(temperature, 0.0f);
    const float radiance = t * t * t * t * (std::max)(intensity, 0.0f);
    return chroma * radiance;
}

/// @brief Ramp Fire は Ramp が色と放射量を決めるため温度の 4 乗を掛けない。
[[nodiscard]] inline math::Vector3 FluidFireRampRadiance(const math::Vector3& rampColor, float intensity)
{
    return rampColor * (std::max)(intensity, 0.0f);
}

/// @brief 一定消散区間を通る平均透過率を解析的に積分する。
[[nodiscard]] inline float FluidFireMeanTransmittance(float transmittance, float extinction, float segmentLength)
{
    const float length = (std::max)(segmentLength, 0.0f);
    const float opticalDepth = (std::max)(extinction, 0.0f) * length;
    const float meanAttenuation = opticalDepth > 1.0e-5f
        ? (1.0f - std::exp(-opticalDepth)) / opticalDepth
        : 1.0f - 0.5f * opticalDepth;
    return std::clamp(transmittance, 0.0f, 1.0f) * meanAttenuation;
}

/// @brief 体積の放射密度を遮蔽率と経路長で積分する。長さ 2 の一様場は 2D の q(T) と一致する。
[[nodiscard]] inline math::Vector3 FluidFireSegmentContribution(const math::Vector3& radiance,
                                                                float transmittance, float extinction,
                                                                float segmentLength)
{
    const float weight = FluidFireMeanTransmittance(transmittance, extinction, segmentLength)
        * (std::max)(segmentLength, 0.0f) / kFluidFireReferencePathLength;
    return radiance * weight;
}

/// @brief 2D Fire と 3D の積分後に同じ 1 − exp(−q) を適用する。
[[nodiscard]] inline math::Vector3 FluidFireSoftKnee(const math::Vector3& radiance)
{
    return { 1.0f - std::exp(-(std::max)(radiance.x, 0.0f)),
             1.0f - std::exp(-(std::max)(radiance.y, 0.0f)),
             1.0f - std::exp(-(std::max)(radiance.z, 0.0f)) };
}

}
