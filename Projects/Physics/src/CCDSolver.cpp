// FBZZ Engine
// CCDSolver.cpp | fbzz::physics
// Swept Sphere の TOI 計算実装
#include <Physics/CCDSolver.hpp>
#include <Physics/RigidBody.hpp>
#include <cmath>
#include <limits>

namespace fbzz::physics
{

    CCDResult CCDSolver::SweptSphereSphere(
        const math::Vector3& centerA, float radiusA,
        const math::Vector3& velA,
        const math::Vector3& centerB, float radiusB,
        float dt)
    {
        CCDResult result;

        // 相対速度・相対位置
        const math::Vector3 relVel = velA * dt; // フレーム内の移動量
        const math::Vector3 w      = centerA - centerB;
        const float sumR  = radiusA + radiusB;

        // |w + t * relVel|^2 = sumR^2 を解く二次方程式
        // a*t^2 + b*t + c = 0, t ∈ [0, 1]
        const float a = math::Vector3::Dot(relVel, relVel);
        const float b = 2.0f * math::Vector3::Dot(relVel, w);
        const float c = math::Vector3::Dot(w, w) - sumR * sumR;

        if (a < 1e-10f) return result; // 静止 or 極めて遅い

        const float disc = b * b - 4.0f * a * c;
        if (disc < 0.0f) return result; // 交差なし

        const float sqrtDisc = std::sqrt(disc);
        const float t0 = (-b - sqrtDisc) / (2.0f * a);
        const float t1 = (-b + sqrtDisc) / (2.0f * a);

        // 最小の正の根 ∈ [0, 1] を採用する
        float toi = -1.0f;
        if (t0 >= 0.0f && t0 <= 1.0f)      toi = t0;
        else if (t1 >= 0.0f && t1 <= 1.0f) toi = t1;
        if (toi < 0.0f) return result;

        result.hit          = true;
        result.toi          = toi;
        const math::Vector3 hitPosA = centerA + relVel * toi;
        const math::Vector3 diff    = hitPosA - centerB;
        const float dist = diff.Length();
        result.normal       = dist > 1e-6f ? diff * (1.0f / dist) : math::Vector3::UP;
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
        const float vDotN = math::Vector3::Dot(movement, planeNormal);

        // 平面に近づいていない場合はスキップ
        if (vDotN >= 0.0f) return result;

        // 現在の符号付き距離
        const float dist0 = math::Vector3::Dot(center, planeNormal) - planeD;

        // t = (radius - dist0) / vDotN
        const float toi = (radius - dist0) / vDotN;
        if (toi < 0.0f || toi > 1.0f) return result;

        result.hit          = true;
        result.toi          = toi;
        result.normal       = planeNormal;
        result.contactPoint = center + movement * toi - planeNormal * radius;
        return result;
    }

    bool CCDSolver::NeedsCCD(const RigidBody& body, float radius, float dt)
    {
        const float speed = body.GetVelocity().Length();
        return speed * dt > radius * CCD_THRESHOLD;
    }

} // namespace fbzz::physics
