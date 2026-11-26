/// @file    FluidGasTransport.cpp
/// @brief   気体の速度とスカラー量の移流、格子の補間、ディテール座標の更新。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Fluid/Solver/FluidSolver.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::fluid {

namespace {

[[nodiscard]] float Lerp(float a, float b, float t) { return a + (b - a) * t; }

}

void FluidGasSolver::ResetDetailLayer(int layer)
{
    for (int y = 0; y < m_cellsY; ++y) {
        for (int x = 0; x < m_cellsX; ++x) {
            const std::size_t cellIndex = Index(x, y, 0);
            m_detailU[layer][cellIndex] = CellCenter(x, m_cellsX);
            m_detailV[layer][cellIndex] = CellCenter(y, m_cellsY);
        }
    }
}

void FluidGasSolver::AdvectDetail(float dt)
{
    if (!HasDetail()) {
        return;
    }
    // 細部の座標も Advect が退避した投影済み速度で運び、煙の移流と同じ流れを読む。
    const float cellsPerUnit = dt / m_cellWidth;
    for (int layer = 0; layer < 2; ++layer) {
        for (int y = 0; y < m_cellsY; ++y) {
            for (int x = 0; x < m_cellsX; ++x) {
                const std::size_t cellIndex = Index(x, y, 0);
                const float gridX = static_cast<float>(x) - m_prevVx[cellIndex] * cellsPerUnit;
                const float gridY = static_cast<float>(y) - m_prevVy[cellIndex] * cellsPerUnit;
                m_scratchA[cellIndex] = SampleGrid(m_detailU[layer], gridX, gridY, 0.0f);
                m_scratchB[cellIndex] = SampleGrid(m_detailV[layer], gridX, gridY, 0.0f);
            }
        }
        m_detailU[layer].swap(m_scratchA);
        m_detailV[layer].swap(m_scratchB);
    }
}

/// @brief 層 0 は位相 0、層 1 は位相 0.5 で初期位置へ戻す。どちらもその瞬間の重みが 0 なので継ぎ目は見えない。
void FluidGasSolver::UpdateDetailEpochs()
{
    if (!HasDetail()) {
        return;
    }
    const float period = (std::max)(m_settings.detailPeriod, 0.05f);
    const int epoch0 = static_cast<int>(std::floor(m_time / period));
    const int epoch1 = static_cast<int>(std::floor(m_time / period + 0.5f));
    if (epoch0 != m_detailEpoch[0]) {
        m_detailEpoch[0] = epoch0;
        ResetDetailLayer(0);
    }
    if (epoch1 != m_detailEpoch[1]) {
        m_detailEpoch[1] = epoch1;
        ResetDetailLayer(1);
    }
}

void FluidGasSolver::DetailWeights(float& outLayer0, float& outLayer1) const
{
    const float period = (std::max)(m_settings.detailPeriod, 0.05f);
    const float phase = m_time / period - std::floor(m_time / period);
    outLayer0 = 1.0f - std::fabs(2.0f * phase - 1.0f);
    outLayer1 = 1.0f - outLayer0;
}

void FluidGasSolver::SampleDetailCoordinate(int layer, float x, float y, float& outU, float& outV) const
{
    if (!HasDetail()) {
        outU = x;
        outV = y;
        return;
    }
    layer = std::clamp(layer, 0, 1);
    const float gridX = ToGrid(x, m_cellsX);
    const float gridY = ToGrid(y, m_cellsY);
    outU = SampleGrid(m_detailU[layer], gridX, gridY, 0.0f);
    outV = SampleGrid(m_detailV[layer], gridX, gridY, 0.0f);
}

float FluidGasSolver::CellCenter(int index, int count) const
{
    return (static_cast<float>(index) + 0.5f - static_cast<float>(count) * 0.5f) * m_cellWidth;
}

float FluidGasSolver::ToGrid(float position, int count) const
{
    return position / m_cellWidth + static_cast<float>(count) * 0.5f - 0.5f;
}

float FluidGasSolver::SampleGrid(const std::vector<float>& field, float gridX, float gridY, float gridZ) const
{
    gridX = std::clamp(gridX, 0.0f, static_cast<float>(m_cellsX - 1));
    gridY = std::clamp(gridY, 0.0f, static_cast<float>(m_cellsY - 1));
    gridZ = std::clamp(gridZ, 0.0f, static_cast<float>(m_cellsZ - 1));
    const int x0 = static_cast<int>(gridX);
    const int y0 = static_cast<int>(gridY);
    const int z0 = static_cast<int>(gridZ);
    const int x1 = (std::min)(x0 + 1, m_cellsX - 1);
    const int y1 = (std::min)(y0 + 1, m_cellsY - 1);
    const int z1 = (std::min)(z0 + 1, m_cellsZ - 1);
    const float tx = gridX - static_cast<float>(x0);
    const float ty = gridY - static_cast<float>(y0);
    const float tz = gridZ - static_cast<float>(z0);
    const float c00 = Lerp(field[Index(x0, y0, z0)], field[Index(x1, y0, z0)], tx);
    const float c10 = Lerp(field[Index(x0, y1, z0)], field[Index(x1, y1, z0)], tx);
    const float c01 = Lerp(field[Index(x0, y0, z1)], field[Index(x1, y0, z1)], tx);
    const float c11 = Lerp(field[Index(x0, y1, z1)], field[Index(x1, y1, z1)], tx);
    return Lerp(Lerp(c00, c10, ty), Lerp(c01, c11, ty), tz);
}

void FluidGasSolver::MinMaxAround(const std::vector<float>& field, float gridX, float gridY, float gridZ,
                                  float& outMin, float& outMax) const
{
    gridX = std::clamp(gridX, 0.0f, static_cast<float>(m_cellsX - 1));
    gridY = std::clamp(gridY, 0.0f, static_cast<float>(m_cellsY - 1));
    gridZ = std::clamp(gridZ, 0.0f, static_cast<float>(m_cellsZ - 1));
    const int x0 = static_cast<int>(gridX);
    const int y0 = static_cast<int>(gridY);
    const int z0 = static_cast<int>(gridZ);
    const int x1 = (std::min)(x0 + 1, m_cellsX - 1);
    const int y1 = (std::min)(y0 + 1, m_cellsY - 1);
    const int z1 = (std::min)(z0 + 1, m_cellsZ - 1);
    outMin = outMax = field[Index(x0, y0, z0)];
    for (const int z : { z0, z1 }) {
        for (const int y : { y0, y1 }) {
            for (const int x : { x0, x1 }) {
                const float value = field[Index(x, y, z)];
                outMin = (std::min)(outMin, value);
                outMax = (std::max)(outMax, value);
            }
        }
    }
}

float FluidGasSolver::SampleDensity(float x, float y, float z) const
{
    return SampleGrid(m_density, ToGrid(x, m_cellsX), ToGrid(y, m_cellsY), m_cellsZ > 1 ? ToGrid(z, m_cellsZ) : 0.0f);
}

float FluidGasSolver::SampleTemperature(float x, float y, float z) const
{
    return SampleGrid(m_temperature, ToGrid(x, m_cellsX), ToGrid(y, m_cellsY), m_cellsZ > 1 ? ToGrid(z, m_cellsZ) : 0.0f);
}

void FluidGasSolver::SampleVelocity(float x, float y, float z, float& outX, float& outY, float& outZ) const
{
    const float gridX = ToGrid(x, m_cellsX);
    const float gridY = ToGrid(y, m_cellsY);
    const float gridZ = m_cellsZ > 1 ? ToGrid(z, m_cellsZ) : 0.0f;
    outX = SampleGrid(m_vx, gridX, gridY, gridZ);
    outY = SampleGrid(m_vy, gridX, gridY, gridZ);
    outZ = m_cellsZ > 1 ? SampleGrid(m_vz, gridX, gridY, gridZ) : 0.0f;
}

void FluidGasSolver::Advect(float dt)
{
    // スカラー量と速度の自己移流は、同じ投影済み速度を読み、セルを更新する順序の影響を受けない。
    std::copy(m_vx.begin(), m_vx.end(), m_prevVx.begin());
    std::copy(m_vy.begin(), m_vy.end(), m_prevVy.begin());
    std::copy(m_vz.begin(), m_vz.end(), m_prevVz.begin());

    AdvectScalar(m_density, dt);
    AdvectScalar(m_temperature, dt);
    AdvectScalar(m_fuel, dt);
    if (m_hasColor) {
        AdvectScalar(m_colorMass, dt);
        AdvectScalar(m_fuelColorMass, dt);
    }

    // 速度の自己移流は渦の芯での振動を避けて半ラグランジュとし、形の鋭さはスカラー側の MacCormack で出す。
    const float cellsPerUnit = dt / m_cellWidth;
    for (int z = 0; z < m_cellsZ; ++z) {
        for (int y = 0; y < m_cellsY; ++y) {
            for (int x = 0; x < m_cellsX; ++x) {
                const std::size_t cellIndex = Index(x, y, z);
                const float gridX = static_cast<float>(x) - m_prevVx[cellIndex] * cellsPerUnit;
                const float gridY = static_cast<float>(y) - m_prevVy[cellIndex] * cellsPerUnit;
                const float gridZ = m_cellsZ > 1 ? static_cast<float>(z) - m_prevVz[cellIndex] * cellsPerUnit : 0.0f;
                m_vx[cellIndex] = SampleGrid(m_prevVx, gridX, gridY, gridZ);
                m_vy[cellIndex] = SampleGrid(m_prevVy, gridX, gridY, gridZ);
                if (m_cellsZ > 1) {
                    m_vz[cellIndex] = SampleGrid(m_prevVz, gridX, gridY, gridZ);
                }
            }
        }
    }
}

/// @see https://www.andyselle.com/papers/7/maccormack.pdf Selle et al., An Unconditionally Stable MacCormack Method (2008), error correction and clamping limiter.
void FluidGasSolver::AdvectScalar(std::vector<float>& field, float dt)
{
    const float velocityToCells = dt / m_cellWidth;
    auto& forwardField = m_scratchA;
    auto& roundTripField = m_scratchB;
    auto& limitedField = m_scratchC;

    const auto traceCell = [&](std::size_t cell, int x, int y, int z, float timeDirection) {
        return math::Vector3{
            static_cast<float>(x) - timeDirection * m_prevVx[cell] * velocityToCells,
            static_cast<float>(y) - timeDirection * m_prevVy[cell] * velocityToCells,
            m_cellsZ > 1 ? static_cast<float>(z) - timeDirection * m_prevVz[cell] * velocityToCells : 0.0f
        };
    };

    for (int z = 0; z < m_cellsZ; ++z) {
        for (int y = 0; y < m_cellsY; ++y) {
            for (int x = 0; x < m_cellsX; ++x) {
                const std::size_t cell = Index(x, y, z);
                const math::Vector3 departure = traceCell(cell, x, y, z, 1.0f);
                forwardField[cell] = SampleGrid(field, departure.x, departure.y, departure.z);
            }
        }
    }
    if (!m_settings.sharpAdvection) {
        field.swap(forwardField);
        return;
    }

    // MacCormack の逆向き移流では、移流後の場を同じ速度で時刻だけ逆に進め、元の場の推定を作る。
    for (int z = 0; z < m_cellsZ; ++z) {
        for (int y = 0; y < m_cellsY; ++y) {
            for (int x = 0; x < m_cellsX; ++x) {
                const std::size_t cell = Index(x, y, z);
                const math::Vector3 arrival = traceCell(cell, x, y, z, -1.0f);
                roundTripField[cell] = SampleGrid(forwardField, arrival.x, arrival.y, arrival.z);
            }
        }
    }

    // 往復誤差の半分で数値拡散を補正し、出発点の補間に使った値の範囲へ収めて新しい極値を抑える。
    for (int z = 0; z < m_cellsZ; ++z) {
        for (int y = 0; y < m_cellsY; ++y) {
            for (int x = 0; x < m_cellsX; ++x) {
                const std::size_t cell = Index(x, y, z);
                const float corrected = forwardField[cell] + 0.5f * (field[cell] - roundTripField[cell]);
                const math::Vector3 departure = traceCell(cell, x, y, z, 1.0f);
                float donorMin = 0.0f;
                float donorMax = 0.0f;
                MinMaxAround(field, departure.x, departure.y, departure.z, donorMin, donorMax);
                limitedField[cell] = std::clamp(corrected, donorMin, donorMax);
            }
        }
    }
    field.swap(limitedField);
}

}
