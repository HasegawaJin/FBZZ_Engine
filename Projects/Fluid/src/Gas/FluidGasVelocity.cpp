/// @file    FluidGasVelocity.cpp
/// @brief   固体セルの速度境界、外力、渦度による速度更新。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Fluid/Solver/FluidSolver.hpp>
#include <Fluid/Operators/FluidOperatorEval.hpp>
#include <Math/Noise/CurlNoise.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace fbzz::fluid {

void FluidGasSolver::BuildSolids()
{
    m_hasSolid = false;
    if (m_colliders.empty()) return;
    const bool volumetric = m_cellsZ > 1;
    std::array<std::size_t, kMaxFluidColliders> colliderIndices{};
    std::array<FluidOperatorPose, kMaxFluidColliders> colliderPoses{};
    int activeCount = 0;
    for (std::size_t c = 0; c < m_colliders.size() && activeCount < kMaxFluidColliders; ++c) {
        if (!FluidColliderActive(m_colliders[c], m_time)) continue;
        colliderIndices[static_cast<std::size_t>(activeCount)] = c;
        colliderPoses[static_cast<std::size_t>(activeCount)] = PoseFluidCollider(m_colliders[c], m_time);
        ++activeCount;
    }
    if (activeCount == 0) return;

    const std::size_t count = m_density.size();
    m_solid.assign(count, 0);
    m_solidVx.resize(count);
    m_solidVy.resize(count);
    m_solidVz.resize(count);
    for (int z = 0; z < m_cellsZ; ++z) {
        for (int y = 0; y < m_cellsY; ++y) {
            for (int x = 0; x < m_cellsX; ++x) {
                const math::Vector3 cellPosition = { CellCenter(x, m_cellsX), CellCenter(y, m_cellsY), CellCenter(z, m_cellsZ) };
                for (int k = 0; k < activeCount; ++k) {
                    const std::size_t slot = static_cast<std::size_t>(k);
                    /// @note minSize = セル幅: それより細い障害物はどのセル中心も覆えず、素通りになる。
                    if (FluidColliderDistance(m_colliders[colliderIndices[slot]], colliderPoses[slot].center, cellPosition, m_cellWidth, volumetric) >= 0.0f)
                        continue;
                    const std::size_t cellIndex = Index(x, y, z);
                    m_solid[cellIndex] = 1;
                    m_solidVx[cellIndex] = colliderPoses[slot].motionVelocity.x;
                    m_solidVy[cellIndex] = colliderPoses[slot].motionVelocity.y;
                    m_solidVz[cellIndex] = colliderPoses[slot].motionVelocity.z;
                    m_hasSolid = true;
                    break;
                }
            }
        }
    }
}

void FluidGasSolver::EnforceSolids(bool clearScalars)
{
    const bool volumetric = m_cellsZ > 1;
    for (std::size_t cellIndex = 0; cellIndex < m_solid.size(); ++cellIndex) {
        if (m_solid[cellIndex] == 0) continue;
        m_vx[cellIndex] = m_solidVx[cellIndex];
        m_vy[cellIndex] = m_solidVy[cellIndex];
        /// @note 2D は奥行きの速度を持たない (動きのキーに z があっても平面の流れには関係しない)。
        if (volumetric) m_vz[cellIndex] = m_solidVz[cellIndex];
        if (clearScalars) {
            m_density[cellIndex] = 0.0f;
            m_temperature[cellIndex] = 0.0f;
            m_fuel[cellIndex] = 0.0f;
            m_colorMass[cellIndex] = 0.0f;
            m_fuelColorMass[cellIndex] = 0.0f;
        }
    }
}

void FluidGasSolver::ApplyForces(float dt)
{
    const bool volumetric = m_cellsZ > 1;
    const float damping = std::exp(-(std::max)(m_settings.velocityDamping, 0.0f) * dt);
    const float turbulence = m_settings.turbulence;
    const float scale = (std::max)(m_settings.turbulenceScale, 0.01f);

    /// @note 力の中心・効いているか・量の倍率は刻みの始めの時刻で 1 回だけ決める (GPU は刻みごとに CPU で詰める)。
    struct ActiveForce {
        std::size_t   index = 0;
        math::Vector3 center = { 0.0f, 0.0f, 0.0f };
        float         amount = 1.0f;
    };
    std::array<ActiveForce, kMaxFluidForces> active{};
    int activeCount = 0;
    for (std::size_t f = 0; f < m_forces.size() && activeCount < kMaxFluidForces; ++f) {
        if (!FluidForceActive(m_forces[f], m_time)) continue;
        active[static_cast<std::size_t>(activeCount)].index = f;
        active[static_cast<std::size_t>(activeCount)].center = PoseFluidForce(m_forces[f], m_time).center;
        active[static_cast<std::size_t>(activeCount)].amount = FluidForceAmount(m_forces[f], m_time);
        ++activeCount;
    }

    for (int z = 0; z < m_cellsZ; ++z) {
        for (int y = 0; y < m_cellsY; ++y) {
            for (int x = 0; x < m_cellsX; ++x) {
                const std::size_t cellIndex = Index(x, y, z);
                m_vy[cellIndex] += (m_settings.buoyancy * m_temperature[cellIndex] - m_settings.weight * m_density[cellIndex]) * dt;
                m_vx[cellIndex] += m_settings.wind.x * dt;
                m_vy[cellIndex] += m_settings.wind.y * dt;
                if (volumetric) m_vz[cellIndex] += m_settings.wind.z * dt;
                if (turbulence != 0.0f) {
                    /// @note 時間でゆっくりずらして «止まった渦» にしない。
                    const math::Vector3 curl = math::CurlNoise({
                        CellCenter(x, m_cellsX) * scale + m_noiseOffset.x,
                        CellCenter(y, m_cellsY) * scale + m_noiseOffset.y - m_time * 0.35f,
                        CellCenter(z, m_cellsZ) * scale + m_noiseOffset.z + m_time * 0.2f });
                    m_vx[cellIndex] += curl.x * turbulence * dt;
                    m_vy[cellIndex] += curl.y * turbulence * dt;
                    if (volumetric) m_vz[cellIndex] += curl.z * turbulence * dt;
                }
                if (activeCount > 0) {
                    /// @note 前の力を足した後の速度を次の力が見る (Drag の順番の意味を GPU と揃える)。
                    const math::Vector3 cellPosition = { CellCenter(x, m_cellsX), CellCenter(y, m_cellsY), CellCenter(z, m_cellsZ) };
                    math::Vector3 v = { m_vx[cellIndex], m_vy[cellIndex], m_vz[cellIndex] };
                    for (int k = 0; k < activeCount; ++k) {
                        const ActiveForce& force = active[static_cast<std::size_t>(k)];
                        v += FluidForceDelta(m_forces[force.index], force.center, cellPosition, v, m_time, dt, m_noiseOffset,
                                             static_cast<int>(force.index), volumetric, force.amount);
                    }
                    m_vx[cellIndex] = v.x;
                    m_vy[cellIndex] = v.y;
                    if (volumetric) m_vz[cellIndex] = v.z;
                }
                m_vx[cellIndex] *= damping;
                m_vy[cellIndex] *= damping;
                m_vz[cellIndex] *= damping;
            }
        }
    }
    if (m_settings.vorticity > 0.0f) ApplyVorticityConfinement(dt);
}

/// @brief Fedkiw et al. 2001。数値拡散で失われる細かい渦を、渦度の強い方へ押し戻す力で補う。
void FluidGasSolver::ApplyVorticityConfinement(float dt)
{
    for (int z = 0; z < m_cellsZ; ++z) {
        for (int y = 0; y < m_cellsY; ++y) {
            for (int x = 0; x < m_cellsX; ++x) {
                const std::size_t cellIndex = Index(x, y, z);
                const float curlX = Derivative(m_vz, x, y, z, 1) - Derivative(m_vy, x, y, z, 2);
                const float curlY = Derivative(m_vx, x, y, z, 2) - Derivative(m_vz, x, y, z, 0);
                const float curlZ = Derivative(m_vy, x, y, z, 0) - Derivative(m_vx, x, y, z, 1);
                m_curlX[cellIndex] = curlX;
                m_curlY[cellIndex] = curlY;
                m_curlZ[cellIndex] = curlZ;
                m_vorticityMagnitude[cellIndex] = std::sqrt(curlX * curlX + curlY * curlY + curlZ * curlZ);
            }
        }
    }
    const float strength = m_settings.vorticity * m_cellWidth;
    const bool volumetric = m_cellsZ > 1;
    for (int z = 0; z < m_cellsZ; ++z) {
        for (int y = 0; y < m_cellsY; ++y) {
            for (int x = 0; x < m_cellsX; ++x) {
                const float gradientX = Derivative(m_vorticityMagnitude, x, y, z, 0);
                const float gradientY = Derivative(m_vorticityMagnitude, x, y, z, 1);
                const float gradientZ = Derivative(m_vorticityMagnitude, x, y, z, 2);
                const float length = std::sqrt(gradientX * gradientX + gradientY * gradientY + gradientZ * gradientZ);
                if (length < 1.0e-6f) continue;
                const float directionX = gradientX / length;
                const float directionY = gradientY / length;
                const float directionZ = gradientZ / length;
                const std::size_t cellIndex = Index(x, y, z);
                /// @note f = ε h (N × ω)
                m_vx[cellIndex] += (directionY * m_curlZ[cellIndex] - directionZ * m_curlY[cellIndex]) * strength * dt;
                m_vy[cellIndex] += (directionZ * m_curlX[cellIndex] - directionX * m_curlZ[cellIndex]) * strength * dt;
                if (volumetric) m_vz[cellIndex] += (directionX * m_curlY[cellIndex] - directionY * m_curlX[cellIndex]) * strength * dt;
            }
        }
    }
}

/// @brief 軸方向の中心差分。縁では片側差分になり、1 セルしか無い軸 (2D の z) は 0。
float FluidGasSolver::Derivative(const std::vector<float>& field, int x, int y, int z, int axis) const
{
    const int count = axis == 0 ? m_cellsX : (axis == 1 ? m_cellsY : m_cellsZ);
    if (count <= 1) return 0.0f;
    const int center = axis == 0 ? x : (axis == 1 ? y : z);
    const int lo = (std::max)(center - 1, 0);
    const int hi = (std::min)(center + 1, count - 1);
    const float low  = field[Index(axis == 0 ? lo : x, axis == 1 ? lo : y, axis == 2 ? lo : z)];
    const float high = field[Index(axis == 0 ? hi : x, axis == 1 ? hi : y, axis == 2 ? hi : z)];
    return (high - low) / (static_cast<float>(hi - lo) * m_cellWidth);
}

}
