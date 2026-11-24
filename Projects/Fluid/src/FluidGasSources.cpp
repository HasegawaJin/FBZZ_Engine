/// @file    FluidGasSources.cpp
/// @brief   発生源の注入、燃焼による膨張、スカラー量の減衰と色キー。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Fluid/FluidSolver.hpp>
#include <Fluid/FluidOperatorEval.hpp>
#include "FluidOperatorGeometry.hpp"
#include <Math/CurlNoise.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::fluid {

namespace {

using operator_geometry_detail::Saturate;

/// @brief 注入を重ねても値が発散しないための上限。絵の上では opacity で飽和するので十分大きい。
constexpr float MAX_DENSITY     = 64.0f;
constexpr float MAX_TEMPERATURE = 64.0f;

/// @brief 鍵 = 色の量 / max(密度, これ)。GPU (FluidGpuCommon の写し) と同じ値にしておかないと焼き分けで色が変わる。
constexpr float COLOR_DENSITY_EPSILON   = 1.0e-6f;

[[nodiscard]] float ColorKeyOf(float mass, float density) { return Saturate(mass / (std::max)(density, COLOR_DENSITY_EPSILON)); }

/// @brief 注入で走査するセルの範囲 (形を外から包む箱)。FluidSourceWeight と同じく minSize まで広げた寸法で包む。
void SourceBounds(const FluidSource& source, const math::Vector3& center, float minimumSize,
                  math::Vector3& outLower, math::Vector3& outUpper)
{
    const float sx = (std::max)(source.size.x, minimumSize);
    const float sy = (std::max)(source.size.y, minimumSize);
    const float sz = (std::max)(source.size.z, minimumSize);
    math::Vector3 extent = { sx, sx, sx };
    switch (source.shape) {
    case FluidSourceShape::Box:
        extent = { sx, sy, sz };
        break;
    case FluidSourceShape::Cone: {
        const math::Vector3& d = source.direction;
        const float lengthSq = d.x * d.x + d.y * d.y + d.z * d.z;
        const math::Vector3 axis = lengthSq < 1.0e-12f ? math::Vector3{ 0.0f, 1.0f, 0.0f }
                                                       : d * (1.0f / std::sqrt(lengthSq));
        /// @note 頂点と底の中心を含む箱を、底の半径 (= 最も太い所) だけ全軸に膨らませる。
        const math::Vector3 tip = center + axis * sy;
        outLower = { (std::min)(center.x, tip.x) - sx, (std::min)(center.y, tip.y) - sx, (std::min)(center.z, tip.z) - sx };
        outUpper = { (std::max)(center.x, tip.x) + sx, (std::max)(center.y, tip.y) + sx, (std::max)(center.z, tip.z) + sx };
        return;
    }
    case FluidSourceShape::Ring:
        extent = { sx + sy, sx + sy, sx + sy };
        break;
    case FluidSourceShape::Texture: {
        /// @note 傾いた板 (箱) を包む軸並行の箱: 各軸へ 3 本の半軸を投影した長さの和。
        math::Vector3 right;
        math::Vector3 up;
        math::Vector3 normal;
        FluidTextureSourceBasis(source, right, up, normal);
        extent = { std::fabs(right.x) * sx + std::fabs(up.x) * sy + std::fabs(normal.x) * sz,
                   std::fabs(right.y) * sx + std::fabs(up.y) * sy + std::fabs(normal.y) * sz,
                   std::fabs(right.z) * sx + std::fabs(up.z) * sy + std::fabs(normal.z) * sz };
        break;
    }
    case FluidSourceShape::Capsule:
    case FluidSourceShape::Cylinder: {
        const math::Vector3& d = source.direction;
        const float lengthSq = d.x * d.x + d.y * d.y + d.z * d.z;
        const math::Vector3 axis = lengthSq < 1.0e-12f ? math::Vector3{ 0.0f, 1.0f, 0.0f }
                                                       : d * (1.0f / std::sqrt(lengthSq));
        /// @note 芯の線分を包む箱を半径だけ全軸に膨らませる (円柱はこのカプセルの中に収まる)。
        extent = { std::fabs(axis.x) * sy + sx, std::fabs(axis.y) * sy + sx, std::fabs(axis.z) * sy + sx };
        break;
    }
    case FluidSourceShape::Sphere:
    default:
        break;
    }
    outLower = center - extent;
    outUpper = center + extent;
}

}

void FluidGasSolver::Inject(float dt)
{
    const bool volumetric = m_cellsZ > 1;
    for (std::size_t sourceIndex = 0; sourceIndex < m_sources.size(); ++sourceIndex) {
        const FluidSource& source = m_sources[sourceIndex];
        if (!FluidSourceEmitting(source, m_time)) continue;
        const FluidOperatorPose pose = PoseFluidSource(source, m_time);

        math::Vector3 lo = pose.center;
        math::Vector3 hi = pose.center;
        SourceBounds(source, pose.center, m_cellWidth, lo, hi);
        const auto range = [&](float low, float high, int count, int& first, int& last) {
            first = std::clamp(static_cast<int>(std::floor(ToGrid(low, count))), 0, count - 1);
            last  = std::clamp(static_cast<int>(std::ceil(ToGrid(high, count))), 0, count - 1);
        };
        int x0 = 0, x1 = 0, y0 = 0, y1 = 0, z0 = 0, z1 = 0;
        range(lo.x, hi.x, m_cellsX, x0, x1);
        range(lo.y, hi.y, m_cellsY, y0, y1);
        if (volumetric) range(lo.z, hi.z, m_cellsZ, z0, z1);

        /// @note 注ぐ «量» だけをエンベロープで揺らす (流速には掛けない — 勢いの変化は velocity と motion の担当)。
        /// @note GPU (PackFluidGpuStep) も同じく «基準の量 × 倍率» を定数へ詰めてから weight・dt を掛ける。
        const float amount = FluidSourceAmount(source, m_time);
        const float density = source.density * amount;
        const float temperature = source.temperature * amount;
        const float fuel = source.fuel * amount;

        /// @note 動く発生源は、動く速さでも周りの煙を引きずる (motion.inheritVelocity)。
        const math::Vector3 target = source.velocity + pose.motionVelocity;
        const bool drivesVelocity = target.x != 0.0f || target.y != 0.0f || target.z != 0.0f;
        const float sourceSeed = static_cast<float>(sourceIndex) * 13.17f;
        for (int z = z0; z <= z1; ++z) {
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const float px = CellCenter(x, m_cellsX);
                    const float py = CellCenter(y, m_cellsY);
                    const float pz = CellCenter(z, m_cellsZ);
                    const math::Vector3 cellPosition = { px, py, pz };
                    float weight = source.shape == FluidSourceShape::Texture
                        ? FluidTextureSourceWeight(source, pose.center, cellPosition, m_cellWidth, volumetric, m_sourceMasks[sourceIndex])
                        : FluidSourceWeight(source, pose.center, cellPosition, m_cellWidth, volumetric);
                    if (weight <= 0.0f) continue;
                    if (source.noise > 0.0f) {
                        const float n = math::ValueNoise3D({ px * 5.0f + m_noiseOffset.x + sourceSeed,
                                                             py * 5.0f + m_noiseOffset.y - m_time * 1.3f,
                                                             pz * 5.0f + m_noiseOffset.z });
                        weight *= (std::max)(0.0f, 1.0f + source.noise * n);
                    }
                    const std::size_t cellIndex = Index(x, y, z);
                    /// @note «注いだ量 × 鍵» を煙と燃料へ入れ、煙を注がない炎でも燃えて生まれる煤に発生源の色を保持する。
                    m_density[cellIndex]     = (std::min)(m_density[cellIndex] + density * weight * dt, MAX_DENSITY);
                    if (m_hasColor)
                        m_colorMass[cellIndex] = (std::min)(m_colorMass[cellIndex] + density * weight * dt * source.colorKey,
                                                    MAX_DENSITY);
                    m_temperature[cellIndex] = (std::min)(m_temperature[cellIndex] + temperature * weight * dt, MAX_TEMPERATURE);
                    m_fuel[cellIndex]        = (std::max)(0.0f, m_fuel[cellIndex] + fuel * weight * dt);
                    if (m_hasColor)
                        m_fuelColorMass[cellIndex] = std::clamp(
                            m_fuelColorMass[cellIndex] + fuel * weight * dt * source.colorKey, 0.0f, MAX_DENSITY);
                    if (drivesVelocity) {
                        const float blend = Saturate(weight * 10.0f * dt);
                        m_vx[cellIndex] += (target.x - m_vx[cellIndex]) * blend;
                        m_vy[cellIndex] += (target.y - m_vy[cellIndex]) * blend;
                        if (volumetric) m_vz[cellIndex] += (target.z - m_vz[cellIndex]) * blend;
                    }
                }
            }
        }
    }
}

void FluidGasSolver::Burn(float dt)
{
    std::fill(m_expansion.begin(), m_expansion.end(), 0.0f);
    const float fraction = Saturate((std::max)(m_settings.burnRate, 0.0f) * dt);
    if (fraction <= 0.0f) return;
    for (std::size_t cellIndex = 0; cellIndex < m_fuel.size(); ++cellIndex) {
        if (m_fuel[cellIndex] <= 1.0e-5f || m_temperature[cellIndex] < m_settings.ignitionTemperature) continue;
        const float burned = m_fuel[cellIndex] * fraction;
        /// @note 燃料を減らす前の鍵で煤を作り、燃えた分だけ燃料の色も連れて行く。
        /// @note 薄い燃料で鍵が跳ぶことを避け、既存の煙へ寄せず量で重み付けして混ぜる。GPU も同じ。
        const float fuelKey = m_hasColor ? ColorKeyOf(m_fuelColorMass[cellIndex], m_fuel[cellIndex]) : 0.0f;
        m_fuel[cellIndex] -= burned;
        if (m_hasColor) m_fuelColorMass[cellIndex] = (std::max)(m_fuelColorMass[cellIndex] - burned * fuelKey, 0.0f);
        m_temperature[cellIndex] = (std::min)(m_temperature[cellIndex] + burned * m_settings.burnHeat, MAX_TEMPERATURE);
        m_density[cellIndex]     = (std::min)(m_density[cellIndex] + burned * m_settings.burnSmoke, MAX_DENSITY);
        if (m_hasColor)
            m_colorMass[cellIndex] = (std::min)(m_colorMass[cellIndex] + burned * m_settings.burnSmoke * fuelKey, MAX_DENSITY);
        /// @note 燃えた分だけガスが膨らむ。圧力解法へ «湧き出し» として渡すと、外向きの爆風になる。
        m_expansion[cellIndex] = burned * m_settings.burnExpansion / dt;
    }
}

void FluidGasSolver::Dissipate(float dt)
{
    /// @note exp(−減衰率 × 秒数) はその時間が過ぎても残る割合。同じ時間を細かく刻んでも、理論上は同じ減衰量になる。
    const float densityKeep     = std::exp(-(std::max)(m_settings.densityDissipation, 0.0f) * dt);
    const float temperatureKeep = std::exp(-(std::max)(m_settings.temperatureDissipation, 0.0f) * dt);
    for (std::size_t cellIndex = 0; cellIndex < m_density.size(); ++cellIndex) {
        m_density[cellIndex] *= densityKeep;
        m_temperature[cellIndex] *= temperatureKeep;
        /// @note 非正規化数へ落ちると演算が桁違いに遅くなる。見えない量は 0 にしてしまう。
        if (m_density[cellIndex] < 1.0e-6f) m_density[cellIndex] = 0.0f;
        if (m_temperature[cellIndex] < 1.0e-6f) m_temperature[cellIndex] = 0.0f;
        if (m_hasColor) {
            m_colorMass[cellIndex] *= densityKeep;
            if (m_colorMass[cellIndex] < 1.0e-6f) m_colorMass[cellIndex] = 0.0f;
        }
    }
}

void FluidGasSolver::UpdateColorKey()
{
    if (!m_hasColor) return;
    for (std::size_t cellIndex = 0; cellIndex < m_density.size(); ++cellIndex)
        m_colorKey[cellIndex] = ColorKeyOf(m_colorMass[cellIndex], m_density[cellIndex]);
}

float FluidGasSolver::SampleColorKey(float x, float y, float z) const
{
    if (!m_hasColor) return 0.0f;
    const float gx = ToGrid(x, m_cellsX);
    const float gy = ToGrid(y, m_cellsY);
    const float gz = m_cellsZ > 1 ? ToGrid(z, m_cellsZ) : 0.0f;
    return ColorKeyOf(SampleGrid(m_colorMass, gx, gy, gz), SampleGrid(m_density, gx, gy, gz));
}

float FluidGasSolver::TotalDensity() const
{
    double total = 0.0;
    for (const float value : m_density) total += value;
    return static_cast<float>(total);
}

}
