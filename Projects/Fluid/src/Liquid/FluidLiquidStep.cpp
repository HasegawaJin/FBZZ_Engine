/// @file    FluidLiquidStep.cpp
/// @brief   液体の時間刻み、外力による予測位置、位置補正後の速度と境界処理。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Fluid/Solver/FluidSolver.hpp>
#include <Fluid/Operators/FluidOperatorEval.hpp>
#include "../Operators/FluidOperatorGeometry.hpp"
#include "FluidLiquidDomain.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace fbzz::fluid {

/// @note 2D のときは z と vz が常に 0 のまま進む (3D の項は 0 を足すだけ)。

namespace {

using operator_geometry_detail::Saturate;

using liquid_domain::MIN_X;
using liquid_domain::MAX_X;
using liquid_domain::MIN_Y;
using liquid_domain::MAX_Y;
using liquid_domain::MIN_Z;
using liquid_domain::MAX_Z;

/// @note 密度拘束の押し出しが 1 刻みで速度へ持ち越せる上限 [領域単位/秒]。
constexpr float MAX_CORRECTION_SPEED = 2.0f;

/// @note 刻みの始めの時刻で効いている力と、その時刻の中心・量の倍率。
struct ActiveForce {
    std::size_t   index = 0; ///< ソルバーが保持する外力設定配列の添字。
    math::Vector3 center = { 0.0f, 0.0f, 0.0f }; ///< この刻みで評価した外力の局所中心位置。
    float         amount = 1.0f; ///< この刻みで外力の基準強度へ掛ける倍率。
};

/// @note 障害物に «触れている» とみなす距離 (粒子半径の倍率)。床の接触と同じ幅。
constexpr float COLLIDER_CONTACT_SCALE = 1.05f;

}

void FluidLiquidSolver::Advance(float dt)
{
    Advance(dt, {});
}

void FluidLiquidSolver::Advance(float dt, const FluidLiquidBoundary& boundary)
{
    if (dt <= 0.0f) {
        return;
    }

    // 現在の粒子とこのフレームに放出する粒子の両方から、刻みを決める速度の上限を見積もる。
    float maxSpeed = 0.0f;
    for (const Particle& particle : m_particles) {
        maxSpeed = (std::max)(maxSpeed, std::sqrt(particle.vx * particle.vx + particle.vy * particle.vy
                                                  + particle.vz * particle.vz));
    }
    for (const FluidSource& source : m_sources) {
        math::Vector3 launchVelocity = source.velocity + PoseFluidSource(source, m_time).motionVelocity;
        if (!m_volumetric) {
            launchVelocity.z = 0.0f;
        }
        maxSpeed = (std::max)(maxSpeed, launchVelocity.Length() * (1.0f + Saturate(source.spread)));
    }
    maxSpeed += (std::max)(m_settings.gravity, 0.0f) * dt;
    // 力で速くなる分も 1 刻みの移動に入れる。Drag は遅くするだけなので数えない。
    // 倍率を掛けた実際の強さで見積もる (倍率が 1 を超える間だけ刻みが足りなくなるのを防ぐ)。
    for (const FluidForce& force : m_forces) {
        if (force.type != FluidForceType::Drag && FluidForceActive(force, m_time)) {
            maxSpeed += std::fabs(force.strength * FluidForceAmount(force, m_time)) * dt;
        }
    }
    // 1 刻みで粒子半径まで。これを超えると近傍の取りこぼしで粒子がすり抜ける。
    const int substepCount = std::clamp(static_cast<int>(std::ceil(maxSpeed * dt / m_particleRadius)), 1, 48);
    const float substepDuration = dt / static_cast<float>(substepCount);
    for (int substep = 0; substep < substepCount; ++substep) {
        Step(substepDuration, boundary);
    }
}

void FluidLiquidSolver::Step(float dt)
{
    Step(dt, {});
}

void FluidLiquidSolver::Step(float dt, const FluidLiquidBoundary& boundary)
{
    if (dt <= 0.0f) {
        return;
    }

    // 粒子の追加と寿命切れの除去を済ませ、今回の粒子配列と作業配列の添字をそろえる。
    Emit();
    if (m_settings.particleLifetime > 0.0f) {
        const float lifetime = m_settings.particleLifetime;
        std::erase_if(m_particles, [lifetime](const Particle& p) { return p.age >= lifetime; });
    }

    const std::size_t particleCount = m_particles.size();
    m_predictedX.resize(particleCount);
    m_predictedY.resize(particleCount);
    m_predictedZ.resize(particleCount);
    m_densityMultiplier.resize(particleCount);
    m_density.resize(particleCount);
    m_deltaX.resize(particleCount);
    m_deltaY.resize(particleCount);
    m_deltaZ.resize(particleCount);

    // 力と障害物の位置や強さをこの刻みの先頭で評価し、全粒子で同じ値を使う。
    std::array<ActiveForce, kMaxFluidForces> activeForces{};
    int activeForceCount = 0;
    for (std::size_t forceIndex = 0; forceIndex < m_forces.size() && activeForceCount < kMaxFluidForces; ++forceIndex) {
        if (!FluidForceActive(m_forces[forceIndex], m_time)) {
            continue;
        }
        activeForces[static_cast<std::size_t>(activeForceCount)].index = forceIndex;
        activeForces[static_cast<std::size_t>(activeForceCount)].center = PoseFluidForce(m_forces[forceIndex], m_time).center;
        activeForces[static_cast<std::size_t>(activeForceCount)].amount = FluidForceAmount(m_forces[forceIndex], m_time);
        ++activeForceCount;
    }
    const math::Vector3 noNoiseOffset = { 0.0f, 0.0f, 0.0f };

    m_activeColliders.clear();
    for (std::size_t colliderIndex = 0; colliderIndex < m_colliders.size(); ++colliderIndex) {
        if (!FluidColliderActive(m_colliders[colliderIndex], m_time)) {
            continue;
        }
        const FluidOperatorPose pose = PoseFluidCollider(m_colliders[colliderIndex], m_time);
        ActiveCollider entry;
        entry.index = colliderIndex;
        entry.center = pose.center;
        entry.velocity = pose.motionVelocity;
        if (!m_volumetric) {
            entry.velocity.z = 0.0f;
        }
        m_activeColliders.push_back(entry);
    }

    // 先に外力を速度へ積分し、その速度から近傍探索に使う予測位置を作る。
    for (std::size_t particleIndex = 0; particleIndex < particleCount; ++particleIndex) {
        Particle& particle = m_particles[particleIndex];
        particle.vy -= m_settings.gravity * dt;
        if (activeForceCount > 0) {
            const math::Vector3 position = { particle.x, particle.y, particle.z };
            math::Vector3 velocity = { particle.vx, particle.vy, particle.vz };
            for (int activeForceIndex = 0; activeForceIndex < activeForceCount; ++activeForceIndex) {
                const ActiveForce& force = activeForces[static_cast<std::size_t>(activeForceIndex)];
                velocity += FluidForceDelta(m_forces[force.index], force.center, position, velocity, m_time, dt,
                                            noNoiseOffset, static_cast<int>(force.index), m_volumetric, force.amount);
            }
            particle.vx = velocity.x;
            particle.vy = velocity.y;
            particle.vz = velocity.z;
        }
        m_predictedX[particleIndex] = particle.x + particle.vx * dt;
        m_predictedY[particleIndex] = particle.y + particle.vy * dt;
        m_predictedZ[particleIndex] = particle.z + particle.vz * dt;
    }

    // 予測位置の近傍で密度を求め、その密度が静止時の値へ近づくよう位置を補正する。
    BuildNeighbors();
    const int iterations = std::clamp(m_settings.solverIterations, 1, 20);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        SolveDensity();
    }

    CommitParticlePositions(dt, boundary);
    ApplyViscosity();

    // 排出判定は接触と粘性まで反映した位置・速度を受け取る。近傍索引を使い終えてから粒子を詰める。
    for (Particle& particle : m_particles) {
        particle.age += dt;
    }
    std::erase_if(m_particles, [&boundary](const Particle& particle) {
        const auto beyondAxisBounds = [](float position, float minimum, float maximum) {
            return position < minimum || position > maximum;
        };
        const bool outsideDomain = beyondAxisBounds(particle.x, MIN_X, MAX_X)
            || beyondAxisBounds(particle.y, MIN_Y, MAX_Y)
            || beyondAxisBounds(particle.z, MIN_Z, MAX_Z);
        if (outsideDomain) {
            return true;
        }
        return boundary.drain && boundary.drain(boundary.context,
            {particle.x, particle.y, particle.z}, {particle.vx, particle.vy, particle.vz});
    });
    m_time += dt;
}

void FluidLiquidSolver::CommitParticlePositions(float dt, const FluidLiquidBoundary& boundary)
{
    const std::size_t particleCount = m_particles.size();
    const float floorContact = m_settings.floorHeight + m_particleRadius * 1.05f;
    const float frictionKeep = std::exp(-(std::max)(m_settings.floorFriction, 0.0f) * 10.0f * dt);
    for (std::size_t particleIndex = 0; particleIndex < particleCount; ++particleIndex) {
        Particle& particle = m_particles[particleIndex];
        FluidLiquidContact surface;
        bool hasBoundaryContact = false;
        float approachSpeed = 0.0f;
        if (boundary.sample) {
            const math::Vector3 predicted{m_predictedX[particleIndex], m_predictedY[particleIndex], m_predictedZ[particleIndex]};
            hasBoundaryContact = boundary.sample(boundary.context, predicted, m_particleRadius, surface);
            // 反発は位置補正から速度を再構成する前の、法線方向への入射速度で決める。
            if (hasBoundaryContact) {
                approachSpeed = math::Vector3::Dot({particle.vx, particle.vy, particle.vz}, surface.normal);
            }
        }
        if (hasBoundaryContact && surface.distance < m_particleRadius) {
            const auto offset = surface.normal * (m_particleRadius - surface.distance);
            m_predictedX[particleIndex] += offset.x;
            m_predictedY[particleIndex] += offset.y;
            m_predictedZ[particleIndex] += offset.z;
        }
        // (補正後の位置 − 前の位置) / dt と積分済み速度との差が補正由来の速度。粒子の飛散を抑えるため、この寄与だけを制限する。
        float velocityCorrectionX = (m_predictedX[particleIndex] - particle.x) / dt - particle.vx;
        float velocityCorrectionY = (m_predictedY[particleIndex] - particle.y) / dt - particle.vy;
        float velocityCorrectionZ = (m_predictedZ[particleIndex] - particle.z) / dt - particle.vz;
        const float correctionSpeed = std::sqrt(velocityCorrectionX * velocityCorrectionX + velocityCorrectionY * velocityCorrectionY + velocityCorrectionZ * velocityCorrectionZ);
        if (correctionSpeed > MAX_CORRECTION_SPEED) {
            const float correctionScale = MAX_CORRECTION_SPEED / correctionSpeed;
            velocityCorrectionX *= correctionScale;
            velocityCorrectionY *= correctionScale;
            velocityCorrectionZ *= correctionScale;
        }
        particle.vx += velocityCorrectionX;
        particle.vy += velocityCorrectionY;
        particle.vz += velocityCorrectionZ;
        particle.x  = m_predictedX[particleIndex];
        particle.y  = m_predictedY[particleIndex];
        particle.z  = m_predictedZ[particleIndex];
        if (hasBoundaryContact) {
            math::Vector3 velocity{particle.vx, particle.vy, particle.vz};
            const float normalSpeed = math::Vector3::Dot(velocity, surface.normal);
            const float rebound = (std::max)(-approachSpeed, 0.0f) * surface.restitution;
            velocity += surface.normal * ((std::max)(normalSpeed, rebound) - normalSpeed);
            particle.vx = velocity.x;
            particle.vy = velocity.y;
            particle.vz = velocity.z;
        }
        if (m_settings.floor && particle.y <= floorContact) {
            particle.vx *= frictionKeep;
            particle.vz *= frictionKeep;
            particle.vy = (std::max)(particle.vy, 0.0f);
        }
        // 障害物との相対速度を法線と接線に分け、内向き成分を切り、摩擦は接線だけに掛ける。
        for (const ActiveCollider& contact : m_activeColliders) {
            const FluidCollider& collider = m_colliders[contact.index];
            const math::Vector3 particlePosition = { particle.x, particle.y, particle.z };
            if (FluidColliderDistance(collider, contact.center, particlePosition, 0.0f, m_volumetric) > m_particleRadius * COLLIDER_CONTACT_SCALE) {
                continue;
            }
            const math::Vector3 surfaceNormal = FluidColliderNormal(collider, contact.center, particlePosition, 0.0f, m_volumetric);
            const float keep = std::exp(-(std::max)(collider.friction, 0.0f) * 10.0f * dt);
            const float rx = particle.vx - contact.velocity.x;
            const float ry = particle.vy - contact.velocity.y;
            const float rz = particle.vz - contact.velocity.z;
            const float normalSpeed = rx * surfaceNormal.x + ry * surfaceNormal.y + rz * surfaceNormal.z;
            const float outwardSpeed = (std::max)(normalSpeed, 0.0f);
            particle.vx = contact.velocity.x + surfaceNormal.x * outwardSpeed + (rx - surfaceNormal.x * normalSpeed) * keep;
            particle.vy = contact.velocity.y + surfaceNormal.y * outwardSpeed + (ry - surfaceNormal.y * normalSpeed) * keep;
            particle.vz = contact.velocity.z + surfaceNormal.z * outwardSpeed + (rz - surfaceNormal.z * normalSpeed) * keep;
        }
    }
}

}
