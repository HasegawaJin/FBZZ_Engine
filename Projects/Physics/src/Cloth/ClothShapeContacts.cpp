/// @file    ClothShapeContacts.cpp
/// @brief   布の外部コライダーとの連続・離散接触、摩擦、反作用。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/Cloth/ClothSolver.hpp>
#include "ClothSolverDetail.hpp"
#include "ClothContactGeometry.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace fbzz::physics {

namespace {

using math::Vector3;

using cloth_detail::GEOMETRY_EPSILON;

using cloth_detail::Vector3d;
using cloth_detail::ToDouble;

using cloth_detail::Dot;

/// @return start が外側にあり、線分が半径 radius のカプセル (a==b は球) に入る割合 [0,1]。入らなければ負。
/// @see https://iquilezles.org/articles/intersectors/ Inigo Quilez, Intersectors の Sphere / Capsule。
double SweepCapsule(const Vector3& start, const Vector3& end, const Vector3& a, const Vector3& b, float radius)
{
    // 開始点が既に内部にある場合は、境界に入る時刻を報告しない。
    const Vector3 axis = b - a;
    const float axisSq = axis.LengthSq();
    const Vector3 nearest = axisSq > GEOMETRY_EPSILON
        ? a + axis * std::clamp(Vector3::Dot(start - a, axis) / axisSq, 0.0f, 1.0f) : a;
    if ((start - nearest).LengthSq() <= radius * radius) {
        return -1.0;
    }
    const Vector3d ro = ToDouble(start), delta = ToDouble(end) - ro;
    const double length = std::sqrt(Dot(delta, delta));
    if (!(length > 1.0e-12)) {
        return -1.0;
    }

    // 交差を線分上の距離で求め、円筒側面と球端の候補のうち最も近いものを採用する。
    const Vector3d rd = delta * (1.0 / length);
    const double r2 = static_cast<double>(radius) * radius;
    double best = std::numeric_limits<double>::infinity();
    const auto sphere = [&](const Vector3d& center) {
        const Vector3d oc = ro - center;
        const double half = Dot(oc, rd);
        const double h = half * half - (Dot(oc, oc) - r2);
        if (h < 0.0) {
            return;
        }
        const double t = -half - std::sqrt(h);
        if (t >= 0.0) {
            best = std::min(best, t);
        }
    };
    const Vector3d pa = ToDouble(a), ba = ToDouble(b) - pa;
    const double baba = Dot(ba, ba);
    if (baba > 1.0e-24) {
        // 軸への射影が両端の間にある解だけが円筒側面に当たる。軸が退化していれば球端だけを調べる。
        const Vector3d oa = ro - pa;
        const double bard = Dot(ba, rd), baoa = Dot(ba, oa);
        const double qa = baba - bard * bard;
        const double qb = baba * Dot(rd, oa) - baoa * bard;
        const double qc = baba * Dot(oa, oa) - baoa * baoa - r2 * baba;
        if (qa > 1.0e-12 * baba) {
            const double h = qb * qb - qa * qc;
            if (h >= 0.0) {
                const double t = (-qb - std::sqrt(h)) / qa;
                const double y = baoa + t * bard;
                if (t >= 0.0 && y > 0.0 && y < baba) {
                    best = std::min(best, t);
                }
            }
        }
        sphere(ToDouble(b));
    }
    sphere(pa);
    return best <= length ? best / length : -1.0;
}

}

/// @see https://graphics.stanford.edu/papers/cloth-sig02/cloth.pdf Contact and Friction、相対速度に対する接触力積。
void ClothSolver::SolveContacts(std::span<const ClothContact> contacts, float substepDuration)
{
    for (size_t particleIndex = 0; particleIndex < m_positions.size(); ++particleIndex) {
        if (m_stepInverseMasses[particleIndex] == 0.0f) {
            continue;
        }
        for (size_t contactIndex = 0; contactIndex < contacts.size(); ++contactIndex) {
            const auto& contact = contacts[contactIndex];
            Vector3 normal;
            float depth;
            bool swept = false;
            // 形状は現在位置で与えられるため、形状に固定した座標で開始点を velocity*h だけ戻す。平面は半空間なので掃引しない。
            if (m_settings.continuousCollision && contact.type != ClothContactType::PLANE) {
                const Vector3 start = m_previous[particleIndex] + contact.velocity * substepDuration;
                const Vector3 end = contact.type == ClothContactType::CAPSULE ? contact.b : contact.a;
                const double hit = SweepCapsule(start, m_positions[particleIndex], contact.a, end, contact.radius + m_settings.thickness);
                if (hit >= 0.0) {
                    m_positions[particleIndex] = start + (m_positions[particleIndex] - start) * static_cast<float>(hit);
                    swept = true;
                }
            }
            if (contact.type == ClothContactType::PLANE) {
                normal = contact.normal;
                depth = contact.offset + m_settings.thickness - Vector3::Dot(m_positions[particleIndex], normal);
            } else {
                Vector3 center = contact.a;
                if (contact.type == ClothContactType::CAPSULE) {
                    const Vector3 axis = contact.b - contact.a;
                    if (axis.LengthSq() > GEOMETRY_EPSILON) {
                        center += axis * std::clamp(Vector3::Dot(m_positions[particleIndex] - contact.a, axis) / axis.LengthSq(), 0.0f, 1.0f);
                    }
                }
                const Vector3 delta = m_positions[particleIndex] - center;
                const float length = delta.Length();
                normal = delta.NormalizedOr((m_previous[particleIndex] - center).NormalizedOr(Vector3::UP));
                depth = contact.radius + m_settings.thickness - length;
            }
            if (depth < 0.0f && !swept) {
                continue;
            }
            m_positions[particleIndex] += normal * std::max(depth, 0.0f);
            // 摩擦は相対接線速度を減衰させる近似。位置射影の後に評価し、法線方向の侵入速度を除く。
            const Vector3 contactOffset = m_positions[particleIndex]-normal*m_settings.thickness-contact.rotationCenter;
            const Vector3 surfaceVelocity = contact.velocity+Vector3::Cross(contact.angularVelocity,contactOffset);
            const Vector3 relativeVelocity = m_velocities[particleIndex] - surfaceVelocity;
            const float normalSpeed = Vector3::Dot(relativeVelocity, normal);
            const Vector3 tangent = relativeVelocity - normal * normalSpeed;
            const auto effectiveMass = [&](const Vector3& direction) {
                const Vector3 angular = Vector3::Cross(contactOffset,direction);
                return m_stepInverseMasses[particleIndex]+contact.inverseMass
                    + std::max(0.0f,Vector3::Dot(angular,contact.inverseInertia*angular));
            };
            Vector3 impulse = normal*(-std::min(0.0f,normalSpeed)/effectiveMass(normal));
            const float tangentSpeed = tangent.Length();
            if (tangentSpeed > GEOMETRY_EPSILON) {
                const Vector3 direction = tangent/tangentSpeed;
                impulse -= direction*(tangentSpeed*m_settings.friction/effectiveMass(direction));
            }
            m_velocities[particleIndex] += impulse*m_stepInverseMasses[particleIndex];
            if (contact.inverseMass > 0.0f) {
                m_contactResponses[contactIndex].impulse -= impulse;
                m_contactResponses[contactIndex].angularImpulse -= Vector3::Cross(contactOffset,impulse);
            }
            m_previous[particleIndex] = m_positions[particleIndex] - m_velocities[particleIndex] * substepDuration;
        }
    }
}

}
