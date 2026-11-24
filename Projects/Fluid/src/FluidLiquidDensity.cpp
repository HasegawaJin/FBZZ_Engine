/// @file    FluidLiquidDensity.cpp
/// @brief   密度カーネル、Jacobi 位置補正、XSPH 粘性。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Fluid/FluidSolver.hpp>
#include <Fluid/FluidOperatorEval.hpp>
#include "FluidOperatorGeometry.hpp"
#include <algorithm>
#include <cmath>

namespace fbzz::fluid {

namespace {

using operator_geometry_detail::Saturate;

}

float FluidLiquidSolver::Poly6(float distanceSquared) const
{
    if (distanceSquared >= m_kernelRadiusSquared) return 0.0f;
    const float d = m_kernelRadiusSquared - distanceSquared;
    return m_densityKernelScale * d * d * d;
}

/// @note ∇W = scale × (p_i − p_j)。scale は負 (離れるほど W が減る)。
float FluidLiquidSolver::SpikyGradientScale(float distance) const
{
    if (distance <= 1.0e-7f || distance >= m_kernelRadius) return 0.0f;
    const float d = m_kernelRadius - distance;
    return m_gradientKernelScale * d * d / distance;
}

float FluidLiquidSolver::MeasuredDensity(std::size_t index) const
{
    return index < m_density.size() ? m_density[index] : 0.0f;
}

void FluidLiquidSolver::SolveDensity()
{
    const float inverseRestDensity = 1.0f / m_restDensity;
    const float cohesion = Saturate(m_settings.cohesion);
    /// @note Jacobi 反復では全粒子の λ と位置補正を計算し終えてから予測位置を更新する。
    ComputeDensityMultipliers(inverseRestDensity, cohesion);
    ComputeDensityCorrections(inverseRestDensity);
    ApplyDensityCorrections();
}

/// @note Cᵢ = ρᵢ / ρ₀ − 1。λᵢ = −Cᵢ / (Σₖ |∇ₖCᵢ|² + ε) は密度誤差を打ち消す位置補正の重み。
/// @see https://mmacklin.com/pbf_sig_preprint.pdf Macklin & Müller, Position Based Fluids (2013), §3.
void FluidLiquidSolver::ComputeDensityMultipliers(float inverseRestDensity, float cohesion)
{
    const std::size_t particleCount = m_particles.size();
    for (std::size_t particleIndex = 0; particleIndex < particleCount; ++particleIndex) {
        /// @note Poly6 による密度の総和には自粒子も含める。近傍リストには自粒子が入らない。
        float density = Poly6(0.0f);
        float selfGradientX = 0.0f;
        float selfGradientY = 0.0f;
        float selfGradientZ = 0.0f;
        float neighborGradientSquares = 0.0f;
        for (int neighborSlot = m_neighborStart[particleIndex]; neighborSlot < m_neighborStart[particleIndex + 1]; ++neighborSlot) {
            const std::size_t neighborIndex = static_cast<std::size_t>(m_neighbors[static_cast<std::size_t>(neighborSlot)]);
            const float dx = m_predictedX[particleIndex] - m_predictedX[neighborIndex];
            const float dy = m_predictedY[particleIndex] - m_predictedY[neighborIndex];
            const float dz = m_predictedZ[particleIndex] - m_predictedZ[neighborIndex];
            const float r2 = dx * dx + dy * dy + dz * dz;
            density += Poly6(r2);
            const float scale = SpikyGradientScale(std::sqrt(r2)) * inverseRestDensity;
            selfGradientX += scale * dx;
            selfGradientY += scale * dy;
            selfGradientZ += scale * dz;
            neighborGradientSquares += (scale * dx) * (scale * dx) + (scale * dy) * (scale * dy)
                                     + (scale * dz) * (scale * dz);
        }
        m_density[particleIndex] = density;
        /// @note 密度を静止時の値で割ると 1 が基準になる。そこから 1 を引くと、粒子が詰まり過ぎた度合いを正の値で表せる。
        float constraint = density * inverseRestDensity - 1.0f;
        /// @note 静止密度を下回る場合だけ cohesion を掛け、疎な粒子を引き寄せる強さを調整する。
        if (constraint < 0.0f) constraint *= cohesion;
        /// @note 自粒子の勾配は寄与を足してから二乗し、近傍粒子の勾配は個別に二乗してから足す。
        const float denominator = selfGradientX * selfGradientX + selfGradientY * selfGradientY
            + selfGradientZ * selfGradientZ + neighborGradientSquares + m_densityRegularization;
        m_densityMultiplier[particleIndex] = -constraint / denominator;
    }
}

/// @note 各粒子対の λᵢ + λⱼ に人工圧力 s_corr を加え、引張不安定による粒子の凝集を抑える。
/// @see https://mmacklin.com/pbf_sig_preprint.pdf Macklin & Müller, Position Based Fluids (2013), 式 (13)–(14).
void FluidLiquidSolver::ComputeDensityCorrections(float inverseRestDensity)
{
    const std::size_t particleCount = m_particles.size();
    /// @note 発生直後に粒子が重なっても、一反復の補正距離を粒子半径の半分までに抑える。
    const float maxCorrection = m_particleRadius * 0.5f;
    for (std::size_t particleIndex = 0; particleIndex < particleCount; ++particleIndex) {
        float sumX = 0.0f;
        float sumY = 0.0f;
        float sumZ = 0.0f;
        for (int neighborSlot = m_neighborStart[particleIndex]; neighborSlot < m_neighborStart[particleIndex + 1]; ++neighborSlot) {
            const std::size_t neighborIndex = static_cast<std::size_t>(m_neighbors[static_cast<std::size_t>(neighborSlot)]);
            const float dx = m_predictedX[particleIndex] - m_predictedX[neighborIndex];
            const float dy = m_predictedY[particleIndex] - m_predictedY[neighborIndex];
            const float dz = m_predictedZ[particleIndex] - m_predictedZ[neighborIndex];
            const float r2 = dx * dx + dy * dy + dz * dz;
            const float scale = SpikyGradientScale(std::sqrt(r2));
            if (scale == 0.0f) continue;
            const float kernelRatio = Poly6(r2) / m_tensileReference;
            const float tensileCorrection = -m_tensileScale * kernelRatio * kernelRatio * kernelRatio * kernelRatio;
            const float coefficient = (m_densityMultiplier[particleIndex] + m_densityMultiplier[neighborIndex] + tensileCorrection) * scale * inverseRestDensity;
            sumX += coefficient * dx;
            sumY += coefficient * dy;
            sumZ += coefficient * dz;
        }
        const float length = std::sqrt(sumX * sumX + sumY * sumY + sumZ * sumZ);
        if (length > maxCorrection) {
            const float shrink = maxCorrection / length;
            sumX *= shrink;
            sumY *= shrink;
            sumZ *= shrink;
        }
        m_deltaX[particleIndex] = sumX;
        m_deltaY[particleIndex] = sumY;
        m_deltaZ[particleIndex] = sumZ;
    }
}

void FluidLiquidSolver::ApplyDensityCorrections()
{
    const std::size_t particleCount = m_particles.size();
    const float floorLimit = m_settings.floorHeight + m_particleRadius;
    for (std::size_t particleIndex = 0; particleIndex < particleCount; ++particleIndex) {
        m_predictedX[particleIndex] += m_deltaX[particleIndex];
        m_predictedY[particleIndex] += m_deltaY[particleIndex];
        m_predictedZ[particleIndex] += m_deltaZ[particleIndex];
        if (m_settings.floor && m_predictedY[particleIndex] < floorLimit) m_predictedY[particleIndex] = floorLimit;
        /// @note 各反復で床、次いで登録順の障害物から半径ぶん離す。次の障害物は直前の補正を反映した位置を読む。
        for (const ActiveCollider& active : m_activeColliders) {
            const FluidCollider& collider = m_colliders[active.index];
            const math::Vector3 p = { m_predictedX[particleIndex], m_predictedY[particleIndex], m_predictedZ[particleIndex] };
            const float distance = FluidColliderDistance(collider, active.center, p, 0.0f, m_volumetric);
            if (distance >= m_particleRadius) continue;
            const math::Vector3 n = FluidColliderNormal(collider, active.center, p, 0.0f, m_volumetric);
            const float push = m_particleRadius - distance;
            m_predictedX[particleIndex] += n.x * push;
            m_predictedY[particleIndex] += n.y * push;
            m_predictedZ[particleIndex] += n.z * push;
        }
    }
}

/// @note XSPH は近傍との速度差を W / ρ₀ で重み付けして足し、速度のばらつきを抑える。
/// @see https://mmacklin.com/pbf_sig_preprint.pdf Macklin & Müller, Position Based Fluids (2013), §5, Eq. (17).
void FluidLiquidSolver::ApplyViscosity()
{
    const float viscosity = Saturate(m_settings.viscosity);
    if (viscosity <= 0.0f) return;
    const std::size_t particleCount = m_particles.size();
    const float inverseRestDensity = 1.0f / m_restDensity;
    for (std::size_t particleIndex = 0; particleIndex < particleCount; ++particleIndex) {
        float sumX = 0.0f;
        float sumY = 0.0f;
        float sumZ = 0.0f;
        for (int neighborSlot = m_neighborStart[particleIndex]; neighborSlot < m_neighborStart[particleIndex + 1]; ++neighborSlot) {
            const std::size_t neighborIndex = static_cast<std::size_t>(m_neighbors[static_cast<std::size_t>(neighborSlot)]);
            const float dx = m_particles[particleIndex].x - m_particles[neighborIndex].x;
            const float dy = m_particles[particleIndex].y - m_particles[neighborIndex].y;
            const float dz = m_particles[particleIndex].z - m_particles[neighborIndex].z;
            const float weight = Poly6(dx * dx + dy * dy + dz * dz) * inverseRestDensity;
            sumX += (m_particles[neighborIndex].vx - m_particles[particleIndex].vx) * weight;
            sumY += (m_particles[neighborIndex].vy - m_particles[particleIndex].vy) * weight;
            sumZ += (m_particles[neighborIndex].vz - m_particles[particleIndex].vz) * weight;
        }
        m_deltaX[particleIndex] = sumX * viscosity;
        m_deltaY[particleIndex] = sumY * viscosity;
        m_deltaZ[particleIndex] = sumZ * viscosity;
    }
    /// @note 全粒子の速度差は適用前の速度から求める。更新した速度が同じパスの近傍計算へ混ざるのを防ぐ。
    for (std::size_t particleIndex = 0; particleIndex < particleCount; ++particleIndex) {
        m_particles[particleIndex].vx += m_deltaX[particleIndex];
        m_particles[particleIndex].vy += m_deltaY[particleIndex];
        m_particles[particleIndex].vz += m_deltaZ[particleIndex];
    }
}

}
