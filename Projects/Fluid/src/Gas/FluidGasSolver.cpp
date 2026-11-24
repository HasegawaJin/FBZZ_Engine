/// @file    FluidGasSolver.cpp
/// @brief   気体ソルバーの格子初期化、部品の差し替え、時間刻み。
/// @author  Hasegawa Jin
/// @date    2026-09-11

#include <Fluid/Solver/FluidSolver.hpp>
#include <Fluid/Operators/FluidOperatorEval.hpp>
#include "../Operators/FluidOperatorGeometry.hpp"
#include <Math/Noise/CurlNoise.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::fluid {

namespace {

using operator_geometry_detail::Saturate;

constexpr int MAX_AXIS_CELLS = 512;

/// @brief このレシピが要求する格子の 1 辺。呼び手が Reset に渡す nx を導いた値で、これが変われば別の格子になる。
[[nodiscard]] int GasGridRequest(const FluidRecipe& recipe, bool volumetric)
{
    return volumetric ? recipe.output.vectorFieldResolution : ResolveGasResolution(recipe);
}

}

/// @brief 部品 (発生源・力・障害物) とその画像だけを取り込む。Reset と ReplaceOperators で共通。
void FluidGasSolver::AdoptOperators(const FluidRecipe& recipe)
{
    m_sources.clear();
    for (const FluidSource& source : recipe.sources)
        if (source.enabled && m_sources.size() < static_cast<std::size_t>(kMaxFluidSources))
            m_sources.push_back(source);
    m_hasColor = false;
    for (FluidSource& source : m_sources) {
        source.colorKey = Saturate(source.colorKey);
        m_hasColor = m_hasColor || source.colorKey > 0.0f;
    }
    m_forces.clear();
    for (const FluidForce& force : recipe.forces)
        if (force.enabled && m_forces.size() < static_cast<std::size_t>(kMaxFluidForces))
            m_forces.push_back(force);
    m_colliders.clear();
    for (const FluidCollider& collider : recipe.colliders)
        if (collider.enabled && m_colliders.size() < static_cast<std::size_t>(kMaxFluidColliders))
            m_colliders.push_back(collider);
    ResolveFluidSourceMasks(m_sources, m_sourceMasks);
}

void FluidGasSolver::Reset(const FluidRecipe& recipe, int requestedCellsX, int requestedCellsY, int requestedCellsZ)
{
    m_settings = recipe.gas;
    AdoptOperators(recipe);
    m_solid.clear();
    m_hasSolid = false;
    m_cellsX = std::clamp(requestedCellsX, 1, MAX_AXIS_CELLS);
    m_cellsY = std::clamp(requestedCellsY, 1, MAX_AXIS_CELLS);
    m_cellsZ = std::clamp(requestedCellsZ, 1, MAX_AXIS_CELLS);
    m_cellWidth  = 2.0f / static_cast<float>((std::max)(m_cellsX, (std::max)(m_cellsY, m_cellsZ)));
    m_time = 0.0f;
    m_seed = recipe.seed;
    m_requestedResolution = GasGridRequest(recipe, m_cellsZ > 1);

    /// @note seed は «ノイズ格子のどこを切り出すか» を決める。値を変えるのではなく座標をずらす。
    const uint32_t hash = math::PcgHash(recipe.seed * 2654435761u + 1u);
    m_noiseOffset = { static_cast<float>(hash % 997u) * 0.731f,
                      static_cast<float>((hash >> 10) % 997u) * 0.517f,
                      static_cast<float>((hash >> 20) % 997u) * 0.379f };

    const std::size_t count = static_cast<std::size_t>(m_cellsX) * m_cellsY * m_cellsZ;
    for (std::vector<float>* field : { &m_density, &m_temperature, &m_fuel,
                                       &m_colorMass, &m_fuelColorMass, &m_colorKey,
                                       &m_vx, &m_vy, &m_vz,
                                       &m_pressure, &m_excessDivergence, &m_expansion,
                                       &m_prevVx, &m_prevVy, &m_prevVz,
                                       &m_scratchA, &m_scratchB, &m_scratchC,
                                       &m_curlX, &m_curlY, &m_curlZ, &m_vorticityMagnitude })
        field->assign(count, 0.0f);

    /// @note 細部の座標は 2D (フリップブック) でだけ使う。3D (速度場 PNG) は速度しか焼かないので持たない。
    for (int layer = 0; layer < 2; ++layer) {
        if (m_cellsZ == 1) {
            m_detailU[layer].assign(count, 0.0f);
            m_detailV[layer].assign(count, 0.0f);
            ResetDetailLayer(layer);
        } else {
            m_detailU[layer].clear();
            m_detailV[layer].clear();
        }
        m_detailEpoch[layer] = 0;
    }
}

/// @brief 場を残せる条件は «今持っている場が、このレシピを頭から解いた途中経過だと言い張れるか»。
/// @note 気体種別・ノイズの seed・要求解像度が同じ場合だけ格子を保持する。
/// @note 刻みごとに効くだけの浮力・散逸・床・圧力反復・燃焼は途中から差し替えてよい。
bool FluidGasSolver::ReplaceOperators(const FluidRecipe& recipe)
{
    if (recipe.kind != FluidKind::Gas) return false;
    if (recipe.seed != m_seed) return false;
    if (GasGridRequest(recipe, m_cellsZ > 1) != m_requestedResolution) return false;

    m_settings = recipe.gas;
    const bool hadColor = m_hasColor;
    AdoptOperators(recipe);
    /// @note 鍵の付いた発生源が 1 つも無くなったら、運んでいた色も捨てる («鍵が無ければ全部 0» を保つ)。
    if (hadColor && !m_hasColor) {
        std::fill(m_colorMass.begin(), m_colorMass.end(), 0.0f);
        std::fill(m_fuelColorMass.begin(), m_fuelColorMass.end(), 0.0f);
        std::fill(m_colorKey.begin(), m_colorKey.end(), 0.0f);
    }
    /// @note 障害物は刻みの始めに組み直す。ここで忘れておかないと、消した障害物が次の 1 刻みだけ残る。
    m_solid.clear();
    m_hasSolid = false;
    return true;
}

void FluidGasSolver::Advance(float dt)
{
    if (dt <= 0.0f) return;
    float maxComponentSpeedSum = 0.0f;
    for (std::size_t cellIndex = 0; cellIndex < m_vx.size(); ++cellIndex)
        maxComponentSpeedSum = (std::max)(maxComponentSpeedSum, std::fabs(m_vx[cellIndex]) + std::fabs(m_vy[cellIndex]) + std::fabs(m_vz[cellIndex]));
    /// @note 1 刻みで 2 セルまでとし、半ラグランジュ移流で細い煙が隙間を飛び越えてちぎれることを防ぐ。
    const float cellsTravelled = maxComponentSpeedSum * dt / m_cellWidth;
    const int substepCount = std::clamp(static_cast<int>(std::ceil(cellsTravelled / 2.0f)), 1, 16);
    const float substepDuration = dt / static_cast<float>(substepCount);
    for (int substep = 0; substep < substepCount; ++substep) Step(substepDuration);
}

void FluidGasSolver::Step(float dt)
{
    if (dt <= 0.0f) return;

    /// @note 障害物と発生源は刻みの始めの時刻で評価する。
    BuildSolids();
    Inject(dt);
    Burn(dt);
    ApplyForces(dt);

    /// @note 障害物の速度を反映してから投影し、燃焼の膨張を含む補正済みの速度場で運ぶ。
    if (m_hasSolid) EnforceSolids(true);
    Project(&m_expansion);
    Advect(dt);
    /// @note 移流で固体セルへ入り込んだスカラー量を消し、固体の速度を戻す。
    if (m_hasSolid) EnforceSolids(true);

    AdvectDetail(dt);
    Dissipate(dt);
    UpdateColorKey();
    m_time += dt;
    UpdateDetailEpochs();
}

}
