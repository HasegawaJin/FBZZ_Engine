/// @file    FluidLiquidSolver.cpp
/// @brief   液体ソルバーの初期化、静止密度の校正、部品の差し替え。
/// @author  Hasegawa Jin
/// @date    2026-09-11

#include <Fluid/FluidSolver.hpp>
#include <Fluid/FluidOperatorEval.hpp>
#include "FluidOperatorGeometry.hpp"
#include "FluidLiquidDomain.hpp"
#include <Math/CurlNoise.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::fluid {

namespace {

using operator_geometry_detail::FLUID_PI;

using liquid_domain::MIN_X;
using liquid_domain::MAX_X;
using liquid_domain::MIN_Y;
using liquid_domain::MAX_Y;
using liquid_domain::MIN_Z;
using liquid_domain::MAX_Z;

/// @note 引張不安定の補正 (Macklin & Müller の s_corr)。粒子が 2 つずつ団子になるのを防ぐ。
/// @note この単位系の λ は約 1e-4 のため、論文の k = 0.1 を直接使わず静止時の勾配和で補正単位を揃える。
constexpr float TENSILE_STRENGTH = 0.05f;

}

/// @brief 発生源・力・障害物とマスクを Reset / ReplaceOperators 共通で取り込む。
/// @pre m_volumetric と m_particleRadius が確定していること。
void FluidLiquidSolver::AdoptOperators(const FluidRecipe& recipe)
{
    m_sources.clear();
    for (const FluidSource& source : recipe.sources)
        if (source.enabled && m_sources.size() < static_cast<std::size_t>(kMaxFluidSources))
            m_sources.push_back(source);
    /// @note 粒子半径まで寸法を広げ、体積 0 によって FluidLiquidEmitScale が全粒を 1 点へ出すことを防ぐ。
    for (FluidSource& source : m_sources) {
        source.size.x = (std::max)(source.size.x, m_particleRadius);
        source.size.y = (std::max)(source.size.y, m_particleRadius);
        source.size.z = (std::max)(source.size.z, m_particleRadius);
    }
    m_forces.clear();
    for (const FluidForce& force : recipe.forces)
        if (force.enabled && m_forces.size() < static_cast<std::size_t>(kMaxFluidForces))
            m_forces.push_back(force);
    m_colliders.clear();
    for (const FluidCollider& collider : recipe.colliders) {
        if (!collider.enabled || m_colliders.size() >= static_cast<std::size_t>(kMaxFluidColliders)) continue;
        m_colliders.push_back(collider);
        /// @note 2D では箱の奥行きを広げ、d.z = 0 の距離と法線が z 軸で決まって粒子を平面外へ押し出すことを防ぐ。
        if (!m_volumetric && collider.shape == FluidColliderShape::Box) m_colliders.back().size.z = 1.0e6f;
    }
    m_activeColliders.clear();
    ResolveFluidSourceMasks(m_sources, m_sourceMasks);
    /// @note 撃ち出し済みの数は «有効な発生源の中での添字» で引き継ぐ。増えた分は 0 から、減った分は捨てる。
    m_emittedPerSource.resize(m_sources.size(), 0);
}

void FluidLiquidSolver::Reset(const FluidRecipe& recipe, bool volumetric)
{
    m_settings = recipe.liquid;
    m_volumetric = volumetric;
    m_particleRadius = std::clamp(m_settings.particleRadius, 0.002f, 0.1f);
    m_emittedPerSource.clear();
    AdoptOperators(recipe);
    m_particles.clear();
    m_time = 0.0f;
    m_rngState = math::PcgHash(recipe.seed * 747796405u + 2891336453u) | 1u;

    m_kernelRadius  = m_particleRadius * 4.0f;
    m_kernelRadiusSquared = m_kernelRadius * m_kernelRadius;
    /// @note Poly6 / Spiky の係数は次元で違う。流用すると静止密度が狂い、液面が縮むか膨らむ。
    if (m_volumetric) {
        m_densityKernelScale         = 315.0f / (64.0f * FLUID_PI * std::pow(m_kernelRadius, 9.0f));
        m_gradientKernelScale = -45.0f / (FLUID_PI * std::pow(m_kernelRadius, 6.0f));
    } else {
        m_densityKernelScale         = 4.0f / (FLUID_PI * std::pow(m_kernelRadius, 8.0f));
        m_gradientKernelScale = -30.0f / (FLUID_PI * std::pow(m_kernelRadius, 5.0f));
    }

    /// @note 粒子を直径間隔で並べて静止密度を測り、解析値と格子の丸めとの差による初期圧縮・膨張を防ぐ。
    const float particleSpacing = 2.0f * m_particleRadius;
    const int kernelReach = static_cast<int>(std::ceil(m_kernelRadius / particleSpacing));
    const int kernelReachZ = m_volumetric ? kernelReach : 0;
    float density = 0.0f;
    float gradientSquaredSum = 0.0f;
    for (int k = -kernelReachZ; k <= kernelReachZ; ++k) {
        for (int j = -kernelReach; j <= kernelReach; ++j) {
            for (int i = -kernelReach; i <= kernelReach; ++i) {
                const float dx = static_cast<float>(i) * particleSpacing;
                const float dy = static_cast<float>(j) * particleSpacing;
                const float dz = static_cast<float>(k) * particleSpacing;
                const float r2 = dx * dx + dy * dy + dz * dz;
                density += Poly6(r2);
                if (i == 0 && j == 0 && k == 0) continue;
                const float scale = SpikyGradientScale(std::sqrt(r2));
                gradientSquaredSum += (scale * dx) * (scale * dx) + (scale * dy) * (scale * dy)
                                 + (scale * dz) * (scale * dz);
            }
        }
    }
    m_restDensity = (std::max)(density, 1.0e-6f);
    const float restGradientSum = (std::max)(gradientSquaredSum / (m_restDensity * m_restDensity), 1.0e-12f);
    /// @note 拘束をわずかに柔らかくする (CFM)。0 だと近傍が 1 つしか無い飛沫で分母が 0 に近づき跳ねる。
    m_densityRegularization = 0.01f * restGradientSum;
    m_tensileReference = (std::max)(Poly6((0.2f * m_kernelRadius) * (0.2f * m_kernelRadius)), 1.0e-12f);
    m_tensileScale = TENSILE_STRENGTH / restGradientSum;

    m_cellsX = (std::max)(1, static_cast<int>(std::ceil((MAX_X - MIN_X) / m_kernelRadius)));
    m_cellsY = (std::max)(1, static_cast<int>(std::ceil((MAX_Y - MIN_Y) / m_kernelRadius)));
    m_cellsZ = m_volumetric ? (std::max)(1, static_cast<int>(std::ceil((MAX_Z - MIN_Z) / m_kernelRadius))) : 1;
}

/// @note 液体種別と粒子半径が同じ場合だけ粒子を保持する。
/// @note 半径が変わると近傍格子・カーネル係数・静止密度も変わり、既存の粒子配置を引き継げない。
/// @note 重力・粘性・まとまり・反復数・寿命は刻みごとに効くだけなので、途中から差し替えてよい。
bool FluidLiquidSolver::ReplaceOperators(const FluidRecipe& recipe)
{
    if (recipe.kind != FluidKind::Liquid) return false;
    if (std::clamp(recipe.liquid.particleRadius, 0.002f, 0.1f) != m_particleRadius) return false;
    m_settings = recipe.liquid;
    AdoptOperators(recipe);
    return true;
}

}
