/// @file    FluidLiquidEmission.cpp
/// @brief   発生源の粒子数・位置・発射速度の決定。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Fluid/Solver/FluidSolver.hpp>
#include <Fluid/Operators/FluidOperatorEval.hpp>
#include "../Operators/FluidOperatorGeometry.hpp"
#include <algorithm>
#include <cmath>

namespace fbzz::fluid {

namespace {

using operator_geometry_detail::Saturate;

using operator_geometry_detail::FLUID_PI;

constexpr int   PARTICLE_HARD_LIMIT = 20000;

/// @note Texture の点をマスクで選り分けるときの引き直しの上限。これで外れ続けるのは «ほぼ真っ黒な画像» だけ。
constexpr int TEXTURE_EMIT_ATTEMPTS = 16;

}

float FluidLiquidSolver::NextRandom()
{
    m_rngState = m_rngState * 1664525u + 1013904223u;
    return static_cast<float>(m_rngState >> 8) * (1.0f / 16777216.0f);
}

void FluidLiquidSolver::Emit()
{
    const std::size_t limit = static_cast<std::size_t>(
        std::clamp(m_settings.maxParticles, 1, PARTICLE_HARD_LIMIT));
    for (std::size_t sourceIndex = 0; sourceIndex < m_sources.size(); ++sourceIndex) {
        const FluidSource& source = m_sources[sourceIndex];
        const int total = (std::max)(source.count, 0);
        int target = 0;
        if (source.duration <= 0.0f)
            target = m_time >= source.startTime ? total : 0;
        else
            target = static_cast<int>(static_cast<float>(total)
                                      * Saturate((m_time - source.startTime) / source.duration));
        target = std::clamp(target, 0, total);
        if (m_emittedPerSource[sourceIndex] >= target || m_particles.size() >= limit) continue;

        const FluidOperatorPose pose = PoseFluidSource(source, m_time);
        math::Vector3 launchVelocity = source.velocity + pose.motionVelocity;
        /// @note 2D は奥行きの速度を見ない (レシピに z が書いてあっても平面の絵には関係しない)。
        if (!m_volumetric) launchVelocity.z = 0.0f;
        const float speed = launchVelocity.Length();
        /// @note 詰まりすぎを避けて広げる倍率は FluidLiquidEmitScale が正本 (Inspector も同じ値を出す)。
        /// @note 撃ち出す速度に動きの速さを含めて測るため、その速度を持った写しを渡す。
        FluidSource fitted = source;
        fitted.velocity = launchVelocity;
        const float scale = FluidLiquidEmitScale(fitted, m_settings, m_volumetric);
        const float jitterScale = Saturate(source.spread) * speed;
        while (m_emittedPerSource[sourceIndex] < target && m_particles.size() < limit) {
            Particle particle;
            particle.colorKey = Saturate(source.colorKey);
            math::Vector3 point = pose.center;
            bool hasSpawnPosition = false;
            if (source.shape == FluidSourceShape::Texture) {
                for (int attempt = 0; attempt < TEXTURE_EMIT_ATTEMPTS && !hasSpawnPosition; ++attempt) {
                    const float u0 = NextRandom();
                    const float u1 = NextRandom();
                    const float u2 = NextRandom();
                    const float accept = NextRandom();
                    hasSpawnPosition = SampleFluidTextureSourcePoint(source, pose.center, m_sourceMasks[sourceIndex], u0, u1, u2, accept,
                                                           m_volumetric, point);
                }
            }
            if (!hasSpawnPosition) {
                const float u0 = NextRandom();
                const float u1 = NextRandom();
                const float u2 = NextRandom();
                point = SampleFluidSourcePoint(source, pose.center, u0, u1, u2, m_volumetric);
            }
            particle.x = pose.center.x + (point.x - pose.center.x) * scale;
            particle.y = pose.center.y + (point.y - pose.center.y) * scale;
            if (m_volumetric) {
                particle.z = pose.center.z + (point.z - pose.center.z) * scale;
                /// @note ばらつきの向きは球面上で一様にする (円柱座標で z を一様に取ると球面一様になる)。
                const float cz = NextRandom() * 2.0f - 1.0f;
                const float angle = NextRandom() * 2.0f * FLUID_PI;
                const float ring = std::sqrt((std::max)(0.0f, 1.0f - cz * cz));
                const float jitter = NextRandom() * jitterScale;
                particle.vx = launchVelocity.x + ring * std::cos(angle) * jitter;
                particle.vy = launchVelocity.y + ring * std::sin(angle) * jitter;
                particle.vz = launchVelocity.z + cz * jitter;
            } else {
                const float angle  = NextRandom() * 2.0f * FLUID_PI;
                const float jitter = NextRandom() * jitterScale;
                particle.vx = launchVelocity.x + std::cos(angle) * jitter;
                particle.vy = launchVelocity.y + std::sin(angle) * jitter;
            }
            m_particles.push_back(particle);
            ++m_emittedPerSource[sourceIndex];
        }
    }
}

}
