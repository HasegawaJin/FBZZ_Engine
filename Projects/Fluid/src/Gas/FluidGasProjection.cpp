/// @file    FluidGasProjection.cpp
/// @brief   ポアソン方程式による圧力計算と速度場の補正。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Fluid/Solver/FluidSolver.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::fluid {

/// @note 速度は全成分をセル中心に置き、フリップブックで差が見えない投影精度より 2D / 3D の共通処理を優先する。

float FluidGasSolver::Divergence(int x, int y, int z) const
{
    const float inverseTwoH = 0.5f / m_cellWidth;
    /// @note セルの両側の速度差を、その間隔（セル幅の 2 倍）で割る。各軸の値を足すと、その場所から流れが広がる速さになる。
    float divergence = 0.0f;
    if (m_cellsX > 1) {
        const float right = m_vx[Index((std::min)(x + 1, m_cellsX - 1), y, z)];
        const float left  = m_vx[Index((std::max)(x - 1, 0), y, z)];
        divergence += (right - left) * inverseTwoH;
    }
    if (m_cellsY > 1) {
        const float up = m_vy[Index(x, (std::min)(y + 1, m_cellsY - 1), z)];
        /// @note 床は «通り抜けられない壁»。下のゴーストは鏡映 (-vy) で、床へ向かう流れを発散として拾う。
        const float down = y > 0 ? m_vy[Index(x, y - 1, z)]
                                 : (m_settings.floor ? -m_vy[Index(x, 0, z)] : m_vy[Index(x, 0, z)]);
        divergence += (up - down) * inverseTwoH;
    }
    if (m_cellsZ > 1) {
        const float front = m_vz[Index(x, y, (std::min)(z + 1, m_cellsZ - 1))];
        const float back  = m_vz[Index(x, y, (std::max)(z - 1, 0))];
        divergence += (front - back) * inverseTwoH;
    }
    return divergence;
}

float FluidGasSolver::MaxDivergence() const
{
    float maxDivergence = 0.0f;
    for (int z = 0; z < m_cellsZ; ++z)
        for (int y = 0; y < m_cellsY; ++y)
            for (int x = 0; x < m_cellsX; ++x) {
                /// @note 障害物の中は流体ではない (投影もそこを解かない)。縁の固体セルは外の流れとの差を拾うだけ。
                if (m_hasSolid && m_solid[Index(x, y, z)] != 0) continue;
                maxDivergence = (std::max)(maxDivergence, std::fabs(Divergence(x, y, z)));
            }
    return maxDivergence;
}

/// @note ∇²p = ∇·u − expansion を解き、u から ∇p を引く。発散のうち目標とする膨張分は残す。
/// @see https://graphics.stanford.edu/courses/cs448-01-spring/papers/stam.pdf Jos Stam, Stable Fluids (1999), §2.1.
void FluidGasSolver::Project(const std::vector<float>* expansion)
{
    BuildPressureRhs(expansion);
    SolvePressure();
    SubtractPressureGradient();

    if (m_settings.floor) {
        for (int z = 0; z < m_cellsZ; ++z) {
            for (int x = 0; x < m_cellsX; ++x) {
                float& floorVelocity = m_vy[Index(x, 0, z)];
                floorVelocity = (std::max)(floorVelocity, 0.0f);
            }
        }
    }
    /// @note 固体セルの速度は障害物が決める。床の制限と重なる場合も最後に障害物の速度を戻す。
    if (m_hasSolid) EnforceSolids(false);
}

void FluidGasSolver::BuildPressureRhs(const std::vector<float>* expansion)
{
    for (int z = 0; z < m_cellsZ; ++z) {
        for (int y = 0; y < m_cellsY; ++y) {
            for (int x = 0; x < m_cellsX; ++x) {
                const std::size_t cell = Index(x, y, z);
                /// @note 実際の広がりから燃焼などで意図した膨張を引く。圧力で取り除くのは、この余分な広がりだけ。
                m_excessDivergence[cell] = Divergence(x, y, z)
                    - (expansion != nullptr ? (*expansion)[cell] : 0.0f);
            }
        }
    }
}

void FluidGasSolver::SolvePressure()
{
    /// @note 前の刻みの圧力を初期値にし、赤黒順の SOR で更新済みの隣接圧力を次の色が読む。
    const bool hasSolids = m_hasSolid;
    const float cellWidthSquared = m_cellWidth * m_cellWidth;
    constexpr float OVER_RELAXATION = 1.7f;
    const int iterations = std::clamp(m_settings.pressureIterations, 1, 400);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        for (int color = 0; color < 2; ++color) {
            for (int z = 0; z < m_cellsZ; ++z) {
                for (int y = 0; y < m_cellsY; ++y) {
                    for (int x = ((y + z + color) & 1); x < m_cellsX; x += 2) {
                        const std::size_t cell = Index(x, y, z);
                        if (hasSolids && m_solid[cell] != 0) continue;

                        float neighborPressureSum = 0.0f;
                        int stencilSize = 0;
                        /// @note 床・固体は隣を自セルと同圧にするため差分が消える。開放端は p = 0 のまま分母に数える。
                        const auto accumulateNeighbor = [&](int nx, int ny, int nz, bool closedOutside) {
                            if (nx >= 0 && nx < m_cellsX && ny >= 0 && ny < m_cellsY && nz >= 0 && nz < m_cellsZ) {
                                const std::size_t neighbor = Index(nx, ny, nz);
                                if (hasSolids && m_solid[neighbor] != 0) return;
                                neighborPressureSum += m_pressure[neighbor];
                                ++stencilSize;
                            } else if (!closedOutside) {
                                ++stencilSize;
                            }
                        };
                        if (m_cellsX > 1) {
                            accumulateNeighbor(x - 1, y, z, false);
                            accumulateNeighbor(x + 1, y, z, false);
                        }
                        if (m_cellsY > 1) {
                            accumulateNeighbor(x, y - 1, z, m_settings.floor);
                            accumulateNeighbor(x, y + 1, z, false);
                        }
                        if (m_cellsZ > 1) {
                            accumulateNeighbor(x, y, z - 1, false);
                            accumulateNeighbor(x, y, z + 1, false);
                        }
                        if (stencilSize == 0) continue;

                        const float targetPressure = (neighborPressureSum - cellWidthSquared * m_excessDivergence[cell])
                            / static_cast<float>(stencilSize);
                        m_pressure[cell] += OVER_RELAXATION * (targetPressure - m_pressure[cell]);
                    }
                }
            }
        }
    }
}

void FluidGasSolver::SubtractPressureGradient()
{
    const bool hasSolids = m_hasSolid;
    const float inverseTwoH = 0.5f / m_cellWidth;
    /// @note 圧力解法と同じ境界条件で、中心差分に使う隣接圧力を選ぶ。
    const auto pressureAt = [&](int x, int y, int z, float centerPressure, bool closedOutside) {
        if (x >= 0 && x < m_cellsX && y >= 0 && y < m_cellsY && z >= 0 && z < m_cellsZ) {
            const std::size_t cell = Index(x, y, z);
            return hasSolids && m_solid[cell] != 0 ? centerPressure : m_pressure[cell];
        }
        return closedOutside ? centerPressure : 0.0f;
    };
    for (int z = 0; z < m_cellsZ; ++z) {
        for (int y = 0; y < m_cellsY; ++y) {
            for (int x = 0; x < m_cellsX; ++x) {
                const std::size_t cell = Index(x, y, z);
                if (hasSolids && m_solid[cell] != 0) continue;
                const float centerPressure = m_pressure[cell];
                if (m_cellsX > 1)
                    m_vx[cell] -= (pressureAt(x + 1, y, z, centerPressure, false)
                        - pressureAt(x - 1, y, z, centerPressure, false)) * inverseTwoH;
                if (m_cellsY > 1)
                    m_vy[cell] -= (pressureAt(x, y + 1, z, centerPressure, false)
                        - pressureAt(x, y - 1, z, centerPressure, m_settings.floor)) * inverseTwoH;
                if (m_cellsZ > 1)
                    m_vz[cell] -= (pressureAt(x, y, z + 1, centerPressure, false)
                        - pressureAt(x, y, z - 1, centerPressure, false)) * inverseTwoH;
            }
        }
    }
}

}
