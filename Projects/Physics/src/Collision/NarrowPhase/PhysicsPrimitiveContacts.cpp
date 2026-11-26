/// @file    PhysicsPrimitiveContacts.cpp
/// @brief   プリミティブ形状同士の接触判定。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/Collision/NarrowPhase/PhysicsSolver.hpp>
#include "PhysicsContactGeometry.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace fbzz::physics
{
    namespace contact_geometry_detail
    {
        void ClosestPointsOnSegments(const math::Vector3& startA,
                                     const math::Vector3& endA,
                                     const math::Vector3& startB,
                                     const math::Vector3& endB,
                                     math::Vector3& outClosestA,
                                     math::Vector3& outClosestB)
        {
            constexpr float EPS = 1e-6f;

            const math::Vector3 segmentA = endA - startA;
            const math::Vector3 segmentB = endB - startB;
            const math::Vector3 r = startA - startB;
            const float a = math::Vector3::Dot(segmentA, segmentA);
            const float e = math::Vector3::Dot(segmentB, segmentB);
            const float f = math::Vector3::Dot(segmentB, r);

            float fractionA = 0.0f;
            float fractionB = 0.0f;

            if (a <= EPS && e <= EPS)
            {
                // どちらも点に縮退している。
                outClosestA = startA;
                outClosestB = startB;
                return;
            }

            if (a <= EPS)
            {
                fractionB = std::clamp(f / e, 0.0f, 1.0f);
            }
            else
            {
                const float c = math::Vector3::Dot(segmentA, r);
                if (e <= EPS)
                {
                    fractionA = std::clamp(-c / a, 0.0f, 1.0f);
                }
                else
                {
                    const float b = math::Vector3::Dot(segmentA, segmentB);
                    const float denom = a * e - b * b;
                    if (denom > EPS)
                    {
                        fractionA = std::clamp((b * f - c * e) / denom, 0.0f, 1.0f);
                    }
                    else
                    {
                        // ほぼ平行な線分は、重なっている区間の中央を代表点にする。
                        const float s0 = math::Vector3::Dot(startB - startA, segmentA) / a;
                        const float s1 = math::Vector3::Dot(endB - startA, segmentA) / a;
                        const float overlapMin = std::max(0.0f, std::min(s0, s1));
                        const float overlapMax = std::min(1.0f, std::max(s0, s1));
                        fractionA = overlapMin <= overlapMax
                            ? (overlapMin + overlapMax) * 0.5f
                            : std::clamp((s0 + s1) * 0.5f, 0.0f, 1.0f);
                    }

                    fractionB = (b * fractionA + f) / e;
                    if (fractionB < 0.0f)
                    {
                        fractionB = 0.0f;
                        fractionA = std::clamp(-c / a, 0.0f, 1.0f);
                    }
                    else if (fractionB > 1.0f)
                    {
                        fractionB = 1.0f;
                        fractionA = std::clamp((b - c) / a, 0.0f, 1.0f);
                    }
                }
            }

            outClosestA = startA + segmentA * fractionA;
            outClosestB = startB + segmentB * fractionB;
        }
    }

    bool PhysicsSolver::TestSphereSphere(const SphereCollider& a, const SphereCollider& b,
                                        ContactPoint& out)
    {
        math::Vector3 posA = a.GetAABB().Center();
        math::Vector3 posB = b.GetAABB().Center();
        math::Vector3 diff = posA - posB;
        float         dist = diff.Length();
        float         sumR = a.m_radius + b.m_radius;

        if (dist >= sumR || dist < 1e-6f) {
            return false;
        }

        out.normal = diff * (1.0f / dist);
        out.depth  = sumR - dist;
        out.point  = posB + out.normal * b.m_radius;
        return true;
    }

    bool PhysicsSolver::TestAABBAABB(const AABBCollider& a, const AABBCollider& b,
                                    ContactPoint& out)
    {
        AABB aabbA = a.GetAABB();
        AABB aabbB = b.GetAABB();

        // 全軸で重なっていれば、重なりが最も浅い方向に箱を離せる。
        float ox = std::min(aabbA.max.x, aabbB.max.x) - std::max(aabbA.min.x, aabbB.min.x);
        float oy = std::min(aabbA.max.y, aabbB.max.y) - std::max(aabbA.min.y, aabbB.min.y);
        float oz = std::min(aabbA.max.z, aabbB.max.z) - std::max(aabbA.min.z, aabbB.min.z);

        if (ox <= 0.0f || oy <= 0.0f || oz <= 0.0f) {
            return false;
        }

        // 法線を B から A へ向け、その軸の向かい合う面の中間に接触点を置く。
        math::Vector3 dir = aabbA.Center() - aabbB.Center();

        if (ox <= oy && ox <= oz)
        {
            out.depth  = ox;
            out.normal = { dir.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f };
            out.point = {
                out.normal.x > 0.0f ? (aabbA.min.x + aabbB.max.x) * 0.5f
                                    : (aabbA.max.x + aabbB.min.x) * 0.5f,
                (std::max(aabbA.min.y, aabbB.min.y) + std::min(aabbA.max.y, aabbB.max.y)) * 0.5f,
                (std::max(aabbA.min.z, aabbB.min.z) + std::min(aabbA.max.z, aabbB.max.z)) * 0.5f
            };
        }
        else if (oy <= ox && oy <= oz)
        {
            out.depth  = oy;
            out.normal = { 0.0f, dir.y >= 0.0f ? 1.0f : -1.0f, 0.0f };
            out.point = {
                (std::max(aabbA.min.x, aabbB.min.x) + std::min(aabbA.max.x, aabbB.max.x)) * 0.5f,
                out.normal.y > 0.0f ? (aabbA.min.y + aabbB.max.y) * 0.5f
                                    : (aabbA.max.y + aabbB.min.y) * 0.5f,
                (std::max(aabbA.min.z, aabbB.min.z) + std::min(aabbA.max.z, aabbB.max.z)) * 0.5f
            };
        }
        else
        {
            out.depth  = oz;
            out.normal = { 0.0f, 0.0f, dir.z >= 0.0f ? 1.0f : -1.0f };
            out.point = {
                (std::max(aabbA.min.x, aabbB.min.x) + std::min(aabbA.max.x, aabbB.max.x)) * 0.5f,
                (std::max(aabbA.min.y, aabbB.min.y) + std::min(aabbA.max.y, aabbB.max.y)) * 0.5f,
                out.normal.z > 0.0f ? (aabbA.min.z + aabbB.max.z) * 0.5f
                                    : (aabbA.max.z + aabbB.min.z) * 0.5f
            };
        }

        return true;
    }

    bool PhysicsSolver::TestOBBOBB(const OBBCollider& a, const OBBCollider& b,
                                   ContactPoint& out)
    {
        constexpr float EPS = 1e-6f;

        const math::Vector3 axesA[3] = { a.GetAxis(0), a.GetAxis(1), a.GetAxis(2) };
        const math::Vector3 axesB[3] = { b.GetAxis(0), b.GetAxis(1), b.GetAxis(2) };
        const float extA[3] = { a.m_halfExtents.x, a.m_halfExtents.y, a.m_halfExtents.z };
        const float extB[3] = { b.m_halfExtents.x, b.m_halfExtents.y, b.m_halfExtents.z };

        float minOverlap = std::numeric_limits<float>::max();
        math::Vector3 bestAxis = math::Vector3::UP;

        // 箱の各半辺を判定軸へ射影し、中心からその軸上の端までの距離を求める。
        auto ProjectRadius = [](const math::Vector3 boxAxes[3],
                                const float extents[3],
                                const math::Vector3& axis) -> float
        {
            return extents[0] * std::abs(math::Vector3::Dot(boxAxes[0], axis))
                 + extents[1] * std::abs(math::Vector3::Dot(boxAxes[1], axis))
                 + extents[2] * std::abs(math::Vector3::Dot(boxAxes[2], axis));
        };

        auto TestAxis = [&](math::Vector3 axis) -> bool
        {
            const float lenSq = axis.LengthSq();
            if (lenSq < EPS) {
                return true;
            }

            axis = axis * (1.0f / std::sqrt(lenSq));
            const float centerA = math::Vector3::Dot(a.GetCenter(), axis);
            const float centerB = math::Vector3::Dot(b.GetCenter(), axis);
            const float radiusA = ProjectRadius(axesA, extA, axis);
            const float radiusB = ProjectRadius(axesB, extB, axis);
            const float overlap = radiusA + radiusB - std::abs(centerA - centerB);

            if (overlap <= 0.0f) {
                return false;
            }
            if (overlap < minOverlap)
            {
                minOverlap = overlap;
                bestAxis = axis;
            }
            return true;
        };

        // 両箱の面法線 6 軸と、異なる箱の辺の外積 9 軸を調べる。軸のいずれかで離れていれば交差しない。
        for (int i = 0; i < 3; ++i)
        {
            if (!TestAxis(axesA[i])) {
                return false;
            }
            if (!TestAxis(axesB[i])) {
                return false;
            }
        }

        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                if (!TestAxis(math::Vector3::Cross(axesA[i], axesB[j]))) {
                    return false;
                }
            }
        }

        // 重なりが最小の軸を B から A へ向け、向かい合う支持点の中間を接触点とする。
        const math::Vector3 delta = a.GetCenter() - b.GetCenter();
        if (math::Vector3::Dot(bestAxis, delta) < 0.0f) {
            bestAxis = -bestAxis;
        }

        out.normal = bestAxis;
        out.depth = minOverlap;
        out.point = (a.SupportPoint(-bestAxis) + b.SupportPoint(bestAxis)) * 0.5f;
        return true;
    }

    bool PhysicsSolver::TestSphereAABB(const SphereCollider& s, const AABBCollider& b,
                                        ContactPoint& out)
    {
        math::Vector3 center = s.GetAABB().Center();
        AABB          aabb   = b.GetAABB();

        math::Vector3 closest = {
            std::max(aabb.min.x, std::min(center.x, aabb.max.x)),
            std::max(aabb.min.y, std::min(center.y, aabb.max.y)),
            std::max(aabb.min.z, std::min(center.z, aabb.max.z))
        };

        math::Vector3 diff = center - closest;
        float         dist = diff.Length();

        if (dist >= s.m_radius) {
            return false;
        }

        if (dist < 1e-6f)
        {
            // 球中心が AABB の内部にある。最近点が中心そのものになるため法線を作れず、
            // 6 面のうち «一番近い面» を選び押し出す。上向き固定にすると壁の中に生成
            // された・高速貫通した球が形状に関係なく真上へ飛ぶため、最短の面を使う。
            const float toFace[6] = {
                center.x - aabb.min.x, aabb.max.x - center.x,
                center.y - aabb.min.y, aabb.max.y - center.y,
                center.z - aabb.min.z, aabb.max.z - center.z
            };
            const math::Vector3 outward[6] = {
                -math::Vector3::RIGHT,   math::Vector3::RIGHT,
                -math::Vector3::UP,      math::Vector3::UP,
                -math::Vector3::FORWARD, math::Vector3::FORWARD
            };

            int best = 0;
            for (int i = 1; i < 6; ++i) {
                if (toFace[i] < toFace[best]) {
                    best = i;
                }
            }

            out.normal = outward[best];
            // 面まで戻る分と、そこから半径ぶん抜ける分の合計。
            out.depth  = s.m_radius + toFace[best];
            out.point  = center + outward[best] * toFace[best];
        }
        else
        {
            out.normal = diff * (1.0f / dist);
            out.depth  = s.m_radius - dist;
            out.point  = closest;
        }

        return true;
    }

    bool PhysicsSolver::TestSphereOBB(const SphereCollider& s, const OBBCollider& b,
                                      ContactPoint& out)
    {
        const math::Vector3 center = s.GetAABB().Center();
        const math::Vector3 delta = center - b.GetCenter();
        math::Vector3 closest = b.GetCenter();

        // 各箱軸の範囲内に射影し、球中心から箱までの最近点を求める。
        const float halfExtents[] = {b.m_halfExtents.x, b.m_halfExtents.y, b.m_halfExtents.z};
        for (int axisIndex = 0; axisIndex < 3; ++axisIndex)
        {
            const math::Vector3 axis = b.GetAxis(axisIndex);
            const float halfExtent = halfExtents[axisIndex];
            const float distance = std::clamp(math::Vector3::Dot(delta, axis), -halfExtent, halfExtent);
            closest += axis * distance;
        }

        const math::Vector3 diff = center - closest;
        const float distSq = diff.LengthSq();
        if (distSq >= s.m_radius * s.m_radius) {
            return false;
        }

        const float dist = std::sqrt(distSq);
        if (dist < 1e-6f)
        {
            // 球中心が箱内なら、最も近い面の法線を押し出し方向に使う。
            math::Vector3 bestAxis = b.GetAxis(0);
            float minFaceDistance = halfExtents[0] - std::abs(math::Vector3::Dot(delta, bestAxis));
            for (int axisIndex = 1; axisIndex < 3; ++axisIndex)
            {
                const math::Vector3 axis = b.GetAxis(axisIndex);
                const float faceDistance = halfExtents[axisIndex] - std::abs(math::Vector3::Dot(delta, axis));
                if (faceDistance < minFaceDistance)
                {
                    minFaceDistance = faceDistance;
                    bestAxis = axis * (math::Vector3::Dot(delta, axis) >= 0.0f ? 1.0f : -1.0f);
                }
            }

            out.normal = bestAxis;
            out.depth = s.m_radius + minFaceDistance;
        }
        else
        {
            out.normal = diff * (1.0f / dist);
            out.depth = s.m_radius - dist;
        }

        out.point = closest;
        return true;
    }

    bool PhysicsSolver::TestSphereCapsule(const SphereCollider& s,
                                        const CapsuleCollider& c,
                                        ContactPoint& out)
    {
        const math::Vector3 center = s.GetAABB().Center();
        const math::Vector3 segStart = c.GetSegmentStart();
        const math::Vector3 segEnd = c.GetSegmentEnd();
        const math::Vector3 seg = segEnd - segStart;
        const float segLenSq = seg.LengthSq();
        float t = 0.0f;
        if (segLenSq > 1e-6f) {
            t = std::clamp(math::Vector3::Dot(center - segStart, seg) / segLenSq, 0.0f, 1.0f);
        }

        const math::Vector3 closest = segStart + seg * t;
        const math::Vector3 diff = center - closest;
        const float dist = diff.Length();
        const float sumR = s.m_radius + c.m_radius;
        if (dist >= sumR) {
            return false;
        }

        out.normal = dist < 1e-6f ? math::Vector3::UP : diff * (1.0f / dist);
        out.depth = sumR - dist;
        out.point = closest + out.normal * c.m_radius;
        return true;
    }

    namespace
    {
        /// @note 箱の内部に埋まった線分を外へ出すときの «出口»。
        struct BoxExit
        {
            math::Vector3 outward;     ///< @note 押し出す向き (ワールド空間)
            float         distance;    ///< @note 線分の最深点を面まで戻すのに要る距離
            bool          bindsStart;  ///< @note その距離を決めているのが線分の始点側か
        };

        /// @brief 箱の 6 面のうち «線分全体を出すのに一番浅くて済む面» を選ぶ。
        /// @note 線分が箱に埋まると最近点は距離 0 の点が無数にあり、1 点だけで決めると
        /// @note たまたま拾った端点次第で押し出し方向が変わる。面は平面なので最深点は
        /// @note 必ず端点のどちらかになり、線分全体 (両端点) で判定すれば足りる。
        /// @note localStart / localEnd は箱の中心を原点、axes を基底とした座標。
        BoxExit ShallowestBoxExit(const math::Vector3& localStart,
                                  const math::Vector3& localEnd,
                                  const math::Vector3& halfExtents,
                                  const math::Vector3 axes[3])
        {
            const float head[3]   = { localStart.x,  localStart.y,  localStart.z  };
            const float tail[3]   = { localEnd.x,    localEnd.y,    localEnd.z    };
            const float extent[3] = { halfExtents.x, halfExtents.y, halfExtents.z };

            BoxExit best{ axes[0], std::numeric_limits<float>::max(), true };
            for (int axis = 0; axis < 3; ++axis)
            {
                // + 側の面から出すなら一番 - 寄りの点が、- 側から出すなら + 寄りの点が縛りになる。
                const float toPositive = extent[axis] - std::min(head[axis], tail[axis]);
                const float toNegative = extent[axis] + std::max(head[axis], tail[axis]);

                if (toPositive < best.distance) {
                    best = { axes[axis], toPositive, head[axis] <= tail[axis] };
                }
                if (toNegative < best.distance) {
                    best = { -axes[axis], toNegative, head[axis] >= tail[axis] };
                }
            }
            return best;
        }
    } /// @note namespace

    bool PhysicsSolver::TestAABBCapsule(const AABBCollider& b,
                                        const CapsuleCollider& c,
                                        ContactPoint& out)
    {
        AABB aabb = b.GetAABB();
        math::Vector3 bestCapsulePoint = c.GetSegmentStart();
        math::Vector3 bestBoxPoint = aabb.Center();
        float bestDistSq = 3.402823466e+38f;

        for (int i = 0; i <= 6; ++i)
        {
            const float t = static_cast<float>(i) / 6.0f;
            const math::Vector3 p = c.GetSegmentStart() + (c.GetSegmentEnd() - c.GetSegmentStart()) * t;
            const math::Vector3 q = {
                std::max(aabb.min.x, std::min(p.x, aabb.max.x)),
                std::max(aabb.min.y, std::min(p.y, aabb.max.y)),
                std::max(aabb.min.z, std::min(p.z, aabb.max.z))
            };
            const float distSq = (p - q).LengthSq();
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                bestCapsulePoint = p;
                bestBoxPoint = q;
            }
        }

        if (bestDistSq >= c.m_radius * c.m_radius) {
            return false;
        }

        const float dist = std::sqrt(bestDistSq);
        if (dist < 1e-6f)
        {
            // カプセル軸がボックスの内部に埋まっている。最近点が軸上の点そのものになるため
            // 法線を作れない。線分全体を «一番浅い面» から押し出す。
            const math::Vector3 center = aabb.Center();
            const math::Vector3 worldAxes[3] = {
                math::Vector3::RIGHT, math::Vector3::UP, math::Vector3::FORWARD
            };
            const BoxExit boxExit = ShallowestBoxExit(c.GetSegmentStart() - center,
                                                   c.GetSegmentEnd() - center,
                                                   aabb.Extents(), worldAxes);

            // 接触法線は「B(カプセル)→A(ボックス)」方向に統一。押し出す向きの逆。
            out.normal = -boxExit.outward;
            out.depth  = c.m_radius + boxExit.distance;

            const math::Vector3 deepest =
                boxExit.bindsStart ? c.GetSegmentStart() : c.GetSegmentEnd();
            bestBoxPoint = deepest + boxExit.outward * boxExit.distance;
        }
        else
        {
            out.normal = (bestBoxPoint - bestCapsulePoint) * (1.0f / dist);
            out.depth = c.m_radius - dist;
        }

        out.point = bestBoxPoint;
        return true;
    }

    bool PhysicsSolver::TestCapsuleCapsule(const CapsuleCollider& a,
                                           const CapsuleCollider& b,
                                           ContactPoint& out)
    {
        math::Vector3 bestA;
        math::Vector3 bestB;
        contact_geometry_detail::ClosestPointsOnSegments(a.GetSegmentStart(), a.GetSegmentEnd(),
                                b.GetSegmentStart(), b.GetSegmentEnd(),
                                bestA, bestB);

        const float sumR = a.m_radius + b.m_radius;
        const float bestDistSq = (bestA - bestB).LengthSq();
        if (bestDistSq >= sumR * sumR) {
            return false;
        }

        const float dist = std::sqrt(bestDistSq);
        out.normal = dist < 1e-6f ? math::Vector3::UP : (bestA - bestB) * (1.0f / dist);
        out.depth = sumR - dist;
        out.point = (bestA + bestB) * 0.5f;
        return true;
    }

    bool PhysicsSolver::TestOBBCapsule(const OBBCollider& box,
                                       const CapsuleCollider& capsule,
                                       ContactPoint& out)
    {
        const math::Vector3 axes[3] = { box.GetAxis(0), box.GetAxis(1), box.GetAxis(2) };
        const math::Vector3 segment = capsule.GetSegmentEnd() - capsule.GetSegmentStart();

        math::Vector3 bestCapsulePoint = capsule.GetSegmentStart();
        math::Vector3 bestBoxPoint = box.GetCenter();
        float bestDistanceSquared = std::numeric_limits<float>::max();

        for (int i = 0; i <= 8; ++i)
        {
            const float t = static_cast<float>(i) / 8.0f;
            const math::Vector3 capsulePoint = capsule.GetSegmentStart() + segment * t;
            const math::Vector3 delta = capsulePoint - box.GetCenter();
            const math::Vector3 localPoint = {
                math::Vector3::Dot(delta, axes[0]),
                math::Vector3::Dot(delta, axes[1]),
                math::Vector3::Dot(delta, axes[2])
            };
            const math::Vector3 clampedLocal = {
                std::clamp(localPoint.x, -box.m_halfExtents.x, box.m_halfExtents.x),
                std::clamp(localPoint.y, -box.m_halfExtents.y, box.m_halfExtents.y),
                std::clamp(localPoint.z, -box.m_halfExtents.z, box.m_halfExtents.z)
            };
            const math::Vector3 boxPoint =
                box.GetCenter() +
                axes[0] * clampedLocal.x +
                axes[1] * clampedLocal.y +
                axes[2] * clampedLocal.z;
            const float distSq = (capsulePoint - boxPoint).LengthSq();
            if (distSq < bestDistanceSquared)
            {
                bestDistanceSquared = distSq;
                bestCapsulePoint = capsulePoint;
                bestBoxPoint = boxPoint;
            }
        }

        if (bestDistanceSquared >= capsule.m_radius * capsule.m_radius) {
            return false;
        }

        const float distance = std::sqrt(bestDistanceSquared);
        if (distance < 1e-6f)
        {
            // カプセル軸が OBB の内部に埋まっている。AABB 版と同じく、拾った 1 点ではなく
            // 線分全体を出せる面を選ぶ。
            const math::Vector3 toStart = capsule.GetSegmentStart() - box.GetCenter();
            const math::Vector3 toEnd   = capsule.GetSegmentEnd()   - box.GetCenter();
            const math::Vector3 localStart = {
                math::Vector3::Dot(toStart, axes[0]),
                math::Vector3::Dot(toStart, axes[1]),
                math::Vector3::Dot(toStart, axes[2])
            };
            const math::Vector3 localEnd = {
                math::Vector3::Dot(toEnd, axes[0]),
                math::Vector3::Dot(toEnd, axes[1]),
                math::Vector3::Dot(toEnd, axes[2])
            };
            const BoxExit boxExit =
                ShallowestBoxExit(localStart, localEnd, box.m_halfExtents, axes);

            // 接触法線は「B(カプセル)→A(OBB)」方向に統一。押し出す向きの逆。
            out.normal = -boxExit.outward;
            out.depth  = capsule.m_radius + boxExit.distance;

            const math::Vector3 deepest =
                boxExit.bindsStart ? capsule.GetSegmentStart() : capsule.GetSegmentEnd();
            bestBoxPoint = deepest + boxExit.outward * boxExit.distance;
        }
        else
        {
            out.normal = (bestBoxPoint - bestCapsulePoint) * (1.0f / distance);
            out.depth = capsule.m_radius - distance;
        }

        out.point = bestBoxPoint;
        return true;
    }

    bool PhysicsSolver::TestSphereCylinder(const SphereCollider& s,
                                            const CylinderCollider& c,
                                            ContactPoint& out)
    {
        const math::Vector3 center  = s.GetAABB().Center();
        const math::Vector3 closest = c.ClosestPoint(center);
        const math::Vector3 diff    = center - closest;
        const float         distSq  = diff.LengthSq();
        if (distSq >= s.m_radius * s.m_radius) {
            return false;
        }

        const float dist = std::sqrt(distSq);
        if (dist < 1e-6f)
        {
            // 球中心が円柱の内部にあり方向が決まらない。側面と円板のうち脱出が浅い方へ押し出す。
            const math::Vector3 delta     = center - c.GetCenter();
            const float         axial     = math::Vector3::Dot(delta, c.GetAxis());
            const math::Vector3 radial    = delta - c.GetAxis() * axial;
            const float         radialLen = radial.Length();

            const float sideDistance = c.m_radius - radialLen;
            const float capDistance  = c.m_halfHeight - std::abs(axial);

            // 中心軸上に完全に乗ると側面方向が定まらないため、その場合は必ず円板側へ逃がす。
            if (radialLen > 1e-6f && sideDistance <= capDistance)
            {
                out.normal = radial * (1.0f / radialLen);
                out.depth  = s.m_radius + sideDistance;
            }
            else
            {
                out.normal = c.GetAxis() * (axial >= 0.0f ? 1.0f : -1.0f);
                out.depth  = s.m_radius + capDistance;
            }
        }
        else
        {
            out.normal = diff * (1.0f / dist);
            out.depth  = s.m_radius - dist;
        }

        out.point = closest;
        return true;
    }
}
