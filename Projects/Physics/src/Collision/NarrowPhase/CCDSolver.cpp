/// @file    CCDSolver.cpp
/// @brief   Swept Sphere の TOI 計算実装。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include <Physics/Collision/NarrowPhase/CCDSolver.hpp>
#include <Physics/Dynamics/RigidBody.hpp>

#include <cmath>

namespace fbzz::physics
{

    CCDResult CCDSolver::SweptSphereSphere(
        const math::Vector3& centerA, float radiusA,
        const math::Vector3& velA,
        const math::Vector3& centerB, float radiusB,
        float dt)
    {
        CCDResult result;

        const math::Vector3 displacement = velA * dt;
        const math::Vector3 initialOffset = centerA - centerB;
        const float combinedRadius = radiusA + radiusB;

        /// @note |initialOffset + t * displacement|² = combinedRadius²。移動全体を 1 とするので、根は時間の割合になる。
        const float a = math::Vector3::Dot(displacement, displacement);
        const float b = 2.0f * math::Vector3::Dot(displacement, initialOffset);
        const float c = math::Vector3::Dot(initialOffset, initialOffset) - combinedRadius * combinedRadius;

        /// @note 移動量がほぼ 0 の場合は、二次方程式の 2a で除算できない。
        if (a < 1e-10f)
            return result;

        const float discriminant = b * b - 4.0f * a * c;
        if (discriminant < 0.0f)
            return result;

        const float sqrtDiscriminant = std::sqrt(discriminant);
        const float entryTime = (-b - sqrtDiscriminant) / (2.0f * a);
        const float exitTime = (-b + sqrtDiscriminant) / (2.0f * a);

        /// @note 開始時点で重なっている場合も、区間内の離脱時刻があれば採用する。
        float toi = -1.0f;
        if (entryTime >= 0.0f && entryTime <= 1.0f)
            toi = entryTime;
        else if (exitTime >= 0.0f && exitTime <= 1.0f)
            toi = exitTime;
        if (toi < 0.0f)
            return result;

        const math::Vector3 impactCenterA = centerA + displacement * toi;
        const math::Vector3 impactOffset = impactCenterA - centerB;
        const float centerDistance = impactOffset.Length();
        result.hit = true;
        result.toi = toi;
        result.normal = centerDistance > 1e-6f ? impactOffset * (1.0f / centerDistance) : math::Vector3::UP;
        result.contactPoint = centerB + result.normal * radiusB;
        return result;
    }

    CCDResult CCDSolver::SweptSpherePlane(
        const math::Vector3& center, float radius,
        const math::Vector3& vel,
        const math::Vector3& planeNormal, float planeD,
        float dt)
    {
        CCDResult result;

        const math::Vector3 movement = vel * dt;
        const float normalDisplacement = math::Vector3::Dot(movement, planeNormal);

        if (normalDisplacement >= 0.0f)
            return result;

        /// @note 球の中心から平面までの符号付き距離が半径と一致する時刻を求める。
        const float initialDistance = math::Vector3::Dot(center, planeNormal) - planeD;
        const float toi = (radius - initialDistance) / normalDisplacement;
        if (toi < 0.0f || toi > 1.0f)
            return result;

        result.hit = true;
        result.toi = toi;
        result.normal = planeNormal;
        result.contactPoint = center + movement * toi - planeNormal * radius;
        return result;
    }

    bool CCDSolver::NeedsCCD(const RigidBody& body, float radius, float dt)
    {
        const float speed = body.GetVelocity().Length();
        return speed * dt > radius * CCD_THRESHOLD;
    }

}
