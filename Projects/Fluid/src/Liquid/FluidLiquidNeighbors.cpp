/// @file    FluidLiquidNeighbors.cpp
/// @brief   予測位置を格子へ配置し、密度反復で使う近傍索引を生成する。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Fluid/Solver/FluidSolver.hpp>
#include "FluidLiquidDomain.hpp"
#include <algorithm>

namespace fbzz::fluid {

namespace {

using liquid_domain::MIN_X;
using liquid_domain::MIN_Y;
using liquid_domain::MIN_Z;

}

int FluidLiquidSolver::CellOf(float x, float y, float z) const
{
    const int cellX = std::clamp(static_cast<int>((x - MIN_X) / m_kernelRadius), 0, m_cellsX - 1);
    const int cellY = std::clamp(static_cast<int>((y - MIN_Y) / m_kernelRadius), 0, m_cellsY - 1);
    const int cellZ = m_volumetric ? std::clamp(static_cast<int>((z - MIN_Z) / m_kernelRadius), 0, m_cellsZ - 1) : 0;
    return (cellZ * m_cellsY + cellY) * m_cellsX + cellX;
}

void FluidLiquidSolver::BuildNeighbors()
{
    const std::size_t particleCount = m_particles.size();
    const int cellCount = m_cellsX * m_cellsY * m_cellsZ;
    m_cellStart.assign(static_cast<std::size_t>(cellCount) + 1, 0);
    m_cellOfParticle.resize(particleCount);
    for (std::size_t particleIndex = 0; particleIndex < particleCount; ++particleIndex) {
        const int cell = CellOf(m_predictedX[particleIndex], m_predictedY[particleIndex], m_predictedZ[particleIndex]);
        m_cellOfParticle[particleIndex] = cell;
        ++m_cellStart[static_cast<std::size_t>(cell) + 1];
    }
    // 粒子数の累積和を取ると、各セルに属する粒子を書き込む区間の先頭になる。
    for (int cell = 0; cell < cellCount; ++cell) {
        m_cellStart[static_cast<std::size_t>(cell) + 1] += m_cellStart[static_cast<std::size_t>(cell)];
    }

    std::vector<int> nextCellSlot(m_cellStart.begin(), m_cellStart.end() - 1);
    m_cellParticles.resize(particleCount);
    for (std::size_t particleIndex = 0; particleIndex < particleCount; ++particleIndex) {
        const std::size_t cell = static_cast<std::size_t>(m_cellOfParticle[particleIndex]);
        const int particleSlot = nextCellSlot[cell]++;
        m_cellParticles[static_cast<std::size_t>(particleSlot)] = static_cast<int>(particleIndex);
    }

    // セル幅はカーネル半径 h なので、同じセルと隣接セルだけ調べれば予測位置から h 未満の粒子を列挙できる。
    // 各密度反復では近傍の索引を再利用し、補正後の位置から距離とカーネルの重みを計算し直す。
    /// @see https://mmacklin.com/pbf_sig_preprint.pdf Macklin & Müller, Position Based Fluids (2013), §6.
    const int reachZ = m_volumetric ? 1 : 0;
    const int cellsPerLayer = m_cellsX * m_cellsY;
    m_neighborStart.resize(particleCount + 1);
    m_neighbors.clear();
    for (std::size_t particleIndex = 0; particleIndex < particleCount; ++particleIndex) {
        m_neighborStart[particleIndex] = static_cast<int>(m_neighbors.size());
        const int cellZ = m_cellOfParticle[particleIndex] / cellsPerLayer;
        const int cellY = (m_cellOfParticle[particleIndex] % cellsPerLayer) / m_cellsX;
        const int cellX = m_cellOfParticle[particleIndex] % m_cellsX;
        for (int offsetZ = -reachZ; offsetZ <= reachZ; ++offsetZ) {
            const int neighborZ = cellZ + offsetZ;
            if (neighborZ < 0 || neighborZ >= m_cellsZ) {
                continue;
            }
            for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                const int neighborY = cellY + offsetY;
                if (neighborY < 0 || neighborY >= m_cellsY) {
                    continue;
                }
                for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                    const int neighborX = cellX + offsetX;
                    if (neighborX < 0 || neighborX >= m_cellsX) {
                        continue;
                    }
                    const std::size_t cell = static_cast<std::size_t>((neighborZ * m_cellsY + neighborY) * m_cellsX + neighborX);
                    for (int k = m_cellStart[cell]; k < m_cellStart[cell + 1]; ++k) {
                        const int neighborParticle = m_cellParticles[static_cast<std::size_t>(k)];
                        if (static_cast<std::size_t>(neighborParticle) == particleIndex) {
                            continue;
                        }
                        const float dx = m_predictedX[particleIndex] - m_predictedX[static_cast<std::size_t>(neighborParticle)];
                        const float dy = m_predictedY[particleIndex] - m_predictedY[static_cast<std::size_t>(neighborParticle)];
                        const float dz = m_predictedZ[particleIndex] - m_predictedZ[static_cast<std::size_t>(neighborParticle)];
                        if (dx * dx + dy * dy + dz * dz < m_kernelRadiusSquared) {
                            m_neighbors.push_back(neighborParticle);
                        }
                    }
                }
            }
        }
    }
    m_neighborStart[particleCount] = static_cast<int>(m_neighbors.size());
}

}
