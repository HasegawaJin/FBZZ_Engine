/// @file    ClothConstraints.cpp
/// @brief   布の伸び・曲げ・移動制約と空気抵抗。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/Cloth/ClothSolver.hpp>
#include "ClothSolverDetail.hpp"
#include <cmath>

namespace fbzz::physics {

namespace {

using math::Vector3;

using cloth_detail::GEOMETRY_EPSILON;

using cloth_detail::ComputeDihedral;
using cloth_detail::IsFiniteVector;

}

/// @see https://matthias-research.github.io/pages/publications/posBasedDyn.pdf 4.1 / Appendix A 二面角。atan2 で折り返しの符号を保持する。
bool cloth_detail::ComputeDihedral(const std::array<Vector3,4>& positions, float& outAngle, std::array<Vector3,4>& outGradient)
{
    const Vector3 edge = positions[3]-positions[2];
    const float length = edge.Length();
    Vector3 firstNormal = Vector3::Cross(positions[2]-positions[0],positions[3]-positions[0]);
    Vector3 secondNormal = Vector3::Cross(positions[3]-positions[1],positions[2]-positions[1]);
    const float firstNormalSquared = firstNormal.LengthSq(), secondNormalSquared = secondNormal.LengthSq();
    if (length <= GEOMETRY_EPSILON || firstNormalSquared < GEOMETRY_EPSILON*GEOMETRY_EPSILON || secondNormalSquared < GEOMETRY_EPSILON*GEOMETRY_EPSILON) {
        return false;
    }
    const auto unit0 = firstNormal/std::sqrt(firstNormalSquared), unit1 = secondNormal/std::sqrt(secondNormalSquared);
    outAngle = std::atan2(Vector3::Dot(Vector3::Cross(unit0,unit1),edge/length),Vector3::Dot(unit0,unit1));
    firstNormal = firstNormal/firstNormalSquared; secondNormal = secondNormal/secondNormalSquared;
    outGradient[0] = firstNormal * -length;
    outGradient[1] = secondNormal * -length;
    outGradient[2] = (firstNormal*Vector3::Dot(positions[0]-positions[3],edge)+secondNormal*Vector3::Dot(positions[1]-positions[3],edge)) / -length;
    outGradient[3] = -outGradient[0]-outGradient[1]-outGradient[2];
    return std::isfinite(outAngle);
}

/// @see https://www1.grc.nasa.gov/beginners-guide-to-aeronautics/dynamic-pressure/ 動圧 q=ρv²/2。法線抗力への適用は初期版の近似。
bool ClothSolver::ApplyWind(float substepDuration, const ClothWindSampler& wind)
{
    for (size_t i = 0; i < m_indices.size(); i += 3) {
        const uint32_t a = m_indices[i], b = m_indices[i + 1], c = m_indices[i + 2];
        const Vector3 cross = Vector3::Cross(m_positions[b] - m_positions[a], m_positions[c] - m_positions[a]);
        const float twiceArea = cross.Length();
        if (twiceArea <= GEOMETRY_EPSILON) {
            continue;
        }
        const Vector3 normal = cross / twiceArea;
        const Vector3 additionalWind = wind ? wind((m_positions[a]+m_positions[b]+m_positions[c])/3.0f) : Vector3{};
        if (!IsFiniteVector(additionalWind)) {
            return false;
        }
        const Vector3 relativeWind = m_settings.windVelocity + additionalWind - (m_velocities[a] + m_velocities[b] + m_velocities[c]) / 3.0f;
        const float speed = Vector3::Dot(relativeWind, normal);
        // q=ρv²/2 の法線抗力を面積で積分し、3 頂点へ等分する。
        const Vector3 impulse = normal * (m_settings.airDensity * m_settings.dragCoefficient
            * twiceArea * speed * std::abs(speed) * substepDuration / 12.0f);
        for (uint32_t vertex : {a, b, c}) {
            if (m_stepInverseMasses[vertex] > 0.0f) {
                m_velocities[vertex] += impulse * m_stepInverseMasses[vertex];
            }
        }
    }
    return true;
}

/// @see https://matthias-research.github.io/pages/publications/XPBD.pdf 式 (18)–(19)、compliance/h² と累積 lambda。
void ClothSolver::SolveDistances(float substepDuration)
{
    for (auto& constraint : m_constraints) {
        if (constraint.bending && m_settings.dihedralBending) {
            continue;
        }
        const float wa = m_stepInverseMasses[constraint.a], wb = m_stepInverseMasses[constraint.b];
        if (wa + wb == 0.0f) {
            continue;
        }
        const Vector3 delta = m_positions[constraint.a] - m_positions[constraint.b];
        const float length = delta.Length();
        if (length <= GEOMETRY_EPSILON) {
            continue;
        }
        const float alpha = (constraint.bending ? m_settings.bendCompliance : m_settings.stretchCompliance) / (substepDuration * substepDuration);
        const float change = (-(length - constraint.restLength) - alpha * constraint.lambda) / (wa + wb + alpha);
        constraint.lambda += change;
        const Vector3 correction = delta * (change / length);
        m_positions[constraint.a] += correction * wa;
        m_positions[constraint.b] -= correction * wb;
    }
}

/// @see https://matthias-research.github.io/pages/publications/XPBD.pdf 式 (18)–(19)、角度勾配の質量重み付き射影。
void ClothSolver::SolveBending(float substepDuration)
{
    if (!m_settings.dihedralBending) {
        return;
    }
    const float alpha = m_settings.bendCompliance/(substepDuration*substepDuration);
    for (auto& bend : m_bends) {
        std::array<Vector3,4> p, angleGradient;
        for (size_t i = 0; i < 4; ++i) {
            p[i] = m_positions[bend.v[i]];
        }
        float angle;
        if (!ComputeDihedral(p,angle,angleGradient)) {
            continue;
        }
        float denominator = alpha;
        for (size_t i = 0; i < 4; ++i) {
            denominator += m_stepInverseMasses[bend.v[i]]*angleGradient[i].LengthSq();
        }
        if (denominator < 1.0e-12f) {
            continue;
        }
        const float error = std::remainder(angle-bend.restAngle,6.28318530718f);
        const float delta = (-error-alpha*bend.lambda)/denominator;
        bend.lambda += delta;
        for (size_t i = 0; i < 4; ++i) {
            m_positions[bend.v[i]] += angleGradient[i]*(m_stepInverseMasses[bend.v[i]]*delta);
        }
    }
}

/// @see https://nvidiagameworks.github.io/PhysX/3.3/PhysXGuide/Manual/Cloth.html#motion-constraints 中心から半径を超えた位置を球面へ射影する。
void ClothSolver::SolveMotion(std::span<const ClothMotionConstraint> motion, float fraction, float substepDuration, bool updateVelocity)
{
    for (const auto& constraint : motion) {
        const Vector3 center = Vector3::Lerp(constraint.previousCenter, constraint.center, fraction);
        auto& position = m_positions[constraint.particle];
        const Vector3 positionBeforeProjection = position;
        const Vector3 delta = position - center;
        const float length = delta.Length();
        if (constraint.radius == 0.0f) {
            position = center;
        }
        else if (length > constraint.radius) {
            position = center + delta * (constraint.radius / length);
        }
        // 接触の接線摩擦を保持したまま、最終射影の位置補正だけ速度へ加える。
        if (updateVelocity) {
            m_velocities[constraint.particle] += (position - positionBeforeProjection) / substepDuration;
        }
    }
}

}
