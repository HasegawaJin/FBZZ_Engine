/// @file    PhysicsMeshContacts.cpp
/// @brief   三角形・メッシュ・高さ場の接触判定。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/Collision/NarrowPhase/PhysicsSolver.hpp>
#include "PhysicsContactGeometry.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace fbzz::physics
{
    namespace
    {
        /// @note 三角形上の最近傍点を Voronoi 領域分類で求める
        math::Vector3 ClosestPointOnTriangle(const math::Vector3& p, const Triangle& tri)
        {
            const math::Vector3& a = tri.v[0];
            const math::Vector3& b = tri.v[1];
            const math::Vector3& c = tri.v[2];

            const math::Vector3 ab = b - a;
            const math::Vector3 ac = c - a;
            const math::Vector3 ap = p - a;

            // 各頂点から p への射影で領域を判別する。最近点が辺にある場合は、その辺に沿った位置を返す。
            const float d1 = math::Vector3::Dot(ab, ap);
            const float d2 = math::Vector3::Dot(ac, ap);
            if (d1 <= 0.0f && d2 <= 0.0f) {
                return a;
            }

            const math::Vector3 bp = p - b;
            const float d3 = math::Vector3::Dot(ab, bp);
            const float d4 = math::Vector3::Dot(ac, bp);
            if (d3 >= 0.0f && d4 <= d3) {
                return b;
            }

            const float vc = d1 * d4 - d3 * d2;
            if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
            {
                const float v = d1 / (d1 - d3);
                return a + ab * v;
            }

            const math::Vector3 cp2 = p - c;
            const float d5 = math::Vector3::Dot(ab, cp2);
            const float d6 = math::Vector3::Dot(ac, cp2);
            if (d6 >= 0.0f && d5 <= d6) {
                return c;
            }

            const float vb = d5 * d2 - d1 * d6;
            if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
            {
                const float w = d2 / (d2 - d6);
                return a + ac * w;
            }

            const float va = d3 * d6 - d5 * d4;
            if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
            {
                const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
                return b + (c - b) * w;
            }

            // 3 頂点や各辺の領域から外れた点は、面に落とした重心座標から復元する。
            const float denom = 1.0f / (va + vb + vc);
            const float v = vb * denom;
            const float w = vc * denom;
            return a + ab * v + ac * w;
        }

        /// @brief 点が三角形の内側または辺上にあるかを符号付き面積で判定する。
        /// @note 線分と三角形面の交差候補が、三角形の外側へ誤って採用されるのを防ぐ。
        bool IsPointOnTriangle(const math::Vector3& p, const Triangle& tri)
        {
            constexpr float EPS = 1e-5f;
            const math::Vector3& a = tri.v[0];
            const math::Vector3& b = tri.v[1];
            const math::Vector3& c = tri.v[2];
            const math::Vector3& n = tri.normal;

            const float edge0 = math::Vector3::Dot(math::Vector3::Cross(b - a, p - a), n);
            const float edge1 = math::Vector3::Dot(math::Vector3::Cross(c - b, p - b), n);
            const float edge2 = math::Vector3::Dot(math::Vector3::Cross(a - c, p - c), n);
            return edge0 >= -EPS && edge1 >= -EPS && edge2 >= -EPS;
        }

        /// @brief 線分と三角形の最近接点を、端点サンプリングなしで求める。
        /// @note 端点と三角形、各辺、面内部交差を全候補として比較する。端点・中点だけの近似は
        /// @note 傾斜面や三角形境界で最近接点と法線が移動方向に応じて跳び、カプセルの
        /// @note 前後振動を誘発する。
        float ClosestPointsOnSegmentTriangle(const math::Vector3& segmentStart,
                                              const math::Vector3& segmentEnd,
                                              const Triangle&       tri,
                                              math::Vector3&        outSegment,
                                              math::Vector3&        outTriangle)
        {
            float bestDistSq = std::numeric_limits<float>::max();
            outSegment = segmentStart;
            outTriangle = tri.v[0];

            auto Consider = [&](const math::Vector3& segmentPoint,
                                const math::Vector3& trianglePoint)
            {
                const float distanceSq = (segmentPoint - trianglePoint).LengthSq();
                if (distanceSq < bestDistSq)
                {
                    bestDistSq = distanceSq;
                    outSegment = segmentPoint;
                    outTriangle = trianglePoint;
                }
            };

            // 線分の端点と三角形の最近接点。
            Consider(segmentStart, ClosestPointOnTriangle(segmentStart, tri));
            Consider(segmentEnd, ClosestPointOnTriangle(segmentEnd, tri));

            // 線分と三角形の各辺の最近接点。
            for (int edge = 0; edge < 3; ++edge)
            {
                math::Vector3 segmentPoint;
                math::Vector3 edgePoint;
                contact_geometry_detail::ClosestPointsOnSegments(segmentStart, segmentEnd,
                                         tri.v[edge], tri.v[(edge + 1) % 3],
                                         segmentPoint, edgePoint);
                Consider(segmentPoint, edgePoint);
            }

            // 線分が三角形面を横切り、交点が三角形内にある場合は距離 0。
            const float signedStart = math::Vector3::Dot(segmentStart - tri.v[0], tri.normal);
            const float signedEnd   = math::Vector3::Dot(segmentEnd - tri.v[0], tri.normal);
            const float planeDelta  = signedStart - signedEnd;
            if (std::abs(planeDelta) > 1e-6f)
            {
                const float planeT = signedStart / planeDelta;
                if (planeT >= 0.0f && planeT <= 1.0f)
                {
                    const math::Vector3 intersection = segmentStart + (segmentEnd - segmentStart) * planeT;
                    if (IsPointOnTriangle(intersection, tri)) {
                        Consider(intersection, intersection);
                    }
                }
            }
            else if (std::abs(signedStart) <= 1e-5f &&
                     (IsPointOnTriangle(segmentStart, tri) || IsPointOnTriangle(segmentEnd, tri)))
            {
                // 線分全体が面とほぼ平行かつ同一平面にある退化ケース。
                Consider(IsPointOnTriangle(segmentStart, tri) ? segmentStart : segmentEnd,
                         IsPointOnTriangle(segmentStart, tri) ? segmentStart : segmentEnd);
            }

            return bestDistSq;
        }
    } /// @note namespace

    bool PhysicsSolver::TestSphereTriangle(const SphereCollider& s,
                                            const Triangle& tri,
                                            ContactPoint& out)
    {
        const math::Vector3 center  = s.GetAABB().Center();
        const math::Vector3 closest = ClosestPointOnTriangle(center, tri);
        const math::Vector3 diff    = center - closest;
        const float         distSq  = diff.LengthSq();

        if (distSq >= s.m_radius * s.m_radius) {
            return false;
        }

        const float dist = std::sqrt(distSq);

        if (dist < 1e-6f)
        {
            // 球中心が三角形面上またはほぼ一致: 面法線を使用
            // 双面: 球中心が裏側なら法線を反転
            math::Vector3 n = tri.normal;
            if (math::Vector3::Dot(n, center - tri.v[0]) < 0.0f) {
                n = -n;
            }
            out.normal = n;
            out.depth  = s.m_radius;
        }
        else
        {
            out.normal = diff * (1.0f / dist);
            out.depth  = s.m_radius - dist;
        }

        out.point = closest;
        return true;
    }

    bool PhysicsSolver::TestAABBTriangle(const AABBCollider& b,
                                          const Triangle& tri,
                                          ContactPoint& out)
    {
        // SAT (Separating Axis Theorem): 13 軸をテストする
        // 軸: 3 面法線 (AABB 軸) + 3 辺 × 3 AABB 軸 = 9 + 1 三角形法線 = 13
        const AABB&         aabb = b.GetAABB();
        const math::Vector3 center = aabb.Center();
        const math::Vector3 half   = aabb.Extents();

        // 三角形頂点を AABB 中心相対座標に変換
        math::Vector3 v[3];
        for (int i = 0; i < 3; ++i) {
            v[i] = tri.v[i] - center;
        }

        math::Vector3 e[3];
        e[0] = v[1] - v[0];
        e[1] = v[2] - v[1];
        e[2] = v[0] - v[2];

        // AABB の 3 軸 (X, Y, Z)
        const math::Vector3 aabbAxes[3] = {
            {1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f}
        };

        float minOverlap = std::numeric_limits<float>::max();
        math::Vector3 bestAxis;

        constexpr float EPS = 1e-6f;

        auto TestAxis = [&](math::Vector3 axis) -> bool
        {
            const float lenSq = axis.LengthSq();
            // 縮退軸はスキップ (分離なし扱い)
            if (lenSq < EPS) {
                return true;
            }
            axis = axis * (1.0f / std::sqrt(lenSq));

            // AABB の投影半幅
            const float r = half.x * std::abs(axis.x)
                          + half.y * std::abs(axis.y)
                          + half.z * std::abs(axis.z);

            // 三角形の投影範囲
            const float p0 = math::Vector3::Dot(v[0], axis);
            const float p1 = math::Vector3::Dot(v[1], axis);
            const float p2 = math::Vector3::Dot(v[2], axis);
            const float triMin = std::min({p0, p1, p2});
            const float triMax = std::max({p0, p1, p2});

            const float overlap = std::min(r - triMin, triMax + r);
            // 分離軸発見
            if (overlap <= 0.0f) {
                return false;
            }

            if (overlap < minOverlap)
            {
                minOverlap = overlap;
                bestAxis   = axis;
            }
            return true;
        };

        // 3 AABB 軸
        for (int i = 0; i < 3; ++i) {
            if (!TestAxis(aabbAxes[i])) {
                return false;
            }
        }

        // 三角形法線
        if (!TestAxis(tri.normal)) {
            return false;
        }

        // 9 クロス積軸 (edgeI × aabbAxisJ)
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                if (!TestAxis(math::Vector3::Cross(e[i], aabbAxes[j]))) {
                    return false;
                }
            }
        }

        // 全軸で重なり → 衝突
        // 法線方向: AABB 中心 → 三角形 重心 に対して bestAxis を合わせる
        const math::Vector3 triCentroid = (v[0] + v[1] + v[2]) / 3.0f;
        if (math::Vector3::Dot(bestAxis, triCentroid) < 0.0f) {
            bestAxis = -bestAxis;
        }

        // AABB → 三角形 方向
        out.normal = -bestAxis;
        out.depth  = minOverlap;
        // 近似接触点
        out.point  = center + bestAxis * (half.x + half.y + half.z) / 3.0f;
        return true;
    }

    bool PhysicsSolver::TestCapsuleTriangle(const CapsuleCollider& c,
                                              const Triangle& tri,
                                              ContactPoint& out)
    {
        const math::Vector3 segmentStart = c.GetSegmentStart();
        const math::Vector3 segmentEnd = c.GetSegmentEnd();

        // Terrain MeshCollider では BroadPhase/BVH の AABB が重なっても、カプセルが
        // 三角形面から半径以上離れているケースが多い。符号付き距離だけで届かないと
        // 分かれば、線分-線分最近傍や ClosestPointOnTriangle の重い計算へ進まない。
        const float signedDistS = math::Vector3::Dot(segmentStart - tri.v[0], tri.normal);
        const float signedDistE = math::Vector3::Dot(segmentEnd - tri.v[0], tri.normal);
        if ((signedDistS > c.m_radius && signedDistE > c.m_radius) ||
            (signedDistS < -c.m_radius && signedDistE < -c.m_radius))
        {
            return false;
        }

        math::Vector3 bestCapsule;
        math::Vector3 closestTrianglePoint;
        const float bestDistSq = ClosestPointsOnSegmentTriangle(
            segmentStart, segmentEnd, tri, bestCapsule, closestTrianglePoint);

        if (bestDistSq >= c.m_radius * c.m_radius) {
            return false;
        }

        const float dist = std::sqrt(bestDistSq);
        if (dist < 1e-6f)
        {
            math::Vector3 n = tri.normal;
            if (math::Vector3::Dot(n, bestCapsule - tri.v[0]) < 0.0f) {
                n = -n;
            }
            out.normal = n;
            out.depth  = c.m_radius;
        }
        else
        {
            out.normal = (bestCapsule - closestTrianglePoint) * (1.0f / dist);
            out.depth  = c.m_radius - dist;
        }

        out.point = closestTrianglePoint;
        return true;
    }

    namespace
    {
        template<typename TestTriangle>
        const Triangle* FindDeepestContact(const BVHTree& tree, const AABB& bounds,
                                           TestTriangle testTriangle, ContactPoint& out)
        {
            float deepestPenetration = -1.0f;
            const Triangle* deepestTriangle = nullptr;
            ContactPoint bestContact;
            tree.Query(bounds, [&](const Triangle& triangle) {
                ContactPoint candidate;
                if (testTriangle(triangle, candidate) && candidate.depth > deepestPenetration) {
                    deepestPenetration = candidate.depth;
                    deepestTriangle = &triangle;
                    bestContact = candidate;
                }
            });
            if (deepestTriangle) {
                out = bestContact;
            }
            return deepestTriangle;
        }

        AABB SphereQueryBounds(const SphereCollider& sphere)
        {
            const math::Vector3 center = sphere.GetAABB().Center();
            const math::Vector3 radius(sphere.m_radius, sphere.m_radius, sphere.m_radius);
            return {center - radius, center + radius};
        }

        Triangle TriangleInBoxSpace(const Triangle& triangle, const OBBCollider& box,
                                    const math::Quaternion& inverseRotation)
        {
            Triangle localTriangle;
            for (int vertex = 0; vertex < 3; ++vertex) {
                localTriangle.v[vertex] = inverseRotation * (triangle.v[vertex] - box.GetCenter());
            }
            localTriangle.normal = (inverseRotation * triangle.normal).Normalized();
            localTriangle.index = triangle.index;
            return localTriangle;
        }

        /// @note 傾いた箱の縁に働く反力の腕を保つため、最深の支持特徴を三角形へ投影する。
        math::Vector3 ProjectBoxSupport(const math::Vector3& normal, const math::Vector3& halfExtents,
                                        const Triangle& localTriangle)
        {
            const auto supportCoordinate = [](float axis, float extent) {
                return std::abs(axis) <= 1.0e-5f ? 0.0f : std::copysign(extent, -axis);
            };
            return ClosestPointOnTriangle({supportCoordinate(normal.x, halfExtents.x),
                                           supportCoordinate(normal.y, halfExtents.y),
                                           supportCoordinate(normal.z, halfExtents.z)}, localTriangle);
        }

        bool MergeCapsuleContactPatch(const std::vector<ContactPoint>& candidates, float capsuleRadius,
                                      ContactPoint& out)
        {
            if (candidates.empty()) {
                return false;
            }

            // 最深接触を基準に、同じ接触パッチに属する三角形だけをマージする。三角形ごとに
            // 法線を持つため、境界を跨ぐたびに最深の1枚を選ぶと法線・摩擦方向がフレーム
            // 単位で切り替わる。近い接触点かつ近い法線の候補に限定し、鋭い角は丸めない。
            auto deepest = std::max_element(candidates.begin(), candidates.end(),
                [](const ContactPoint& lhs, const ContactPoint& rhs) {
                    return lhs.depth < rhs.depth;
                });
            const float deepestPenetration = deepest->depth;
            const math::Vector3 referencePoint = deepest->point;
            const math::Vector3 referenceNormal = deepest->normal;
            const float mergeRadius = std::max(0.05f, capsuleRadius * 0.5f);
            const float mergeRadiusSq = mergeRadius * mergeRadius;
            constexpr float DEPTH_TOLERANCE = 0.02f;
            constexpr float NORMAL_MERGE_DOT = 0.96f;

            math::Vector3 normalSum = math::Vector3::ZERO;
            math::Vector3 pointSum = math::Vector3::ZERO;
            float weightSum = 0.0f;
            int mergedCount = 0;
            for (const ContactPoint& candidate : candidates)
            {
                if (deepestPenetration - candidate.depth > DEPTH_TOLERANCE) {
                    continue;
                }
                if ((candidate.point - referencePoint).LengthSq() > mergeRadiusSq) {
                    continue;
                }
                if (math::Vector3::Dot(candidate.normal, referenceNormal) < NORMAL_MERGE_DOT) {
                    continue;
                }

                // 深い接触ほど大きく反映し、接触点・法線の急な切り替わりを抑える。
                const float weight = std::max(candidate.depth, 1e-4f);
                normalSum += candidate.normal * weight;
                pointSum += candidate.point * weight;
                weightSum += weight;
                ++mergedCount;
            }

            out = *deepest;
            if (mergedCount > 1 && weightSum > 0.0f && normalSum.LengthSq() > 1e-8f)
            {
                out.normal = normalSum.Normalized();
                out.point = pointSum * (1.0f / weightSum);
                // 深度は最大値を残す。平均すると位置補正が不足して再び貫通するため。
                out.depth = deepestPenetration;
            }
            return true;
        }
    }

    bool PhysicsSolver::TestSphereTriangleMesh(const SphereCollider& sphere,
        const TriangleMeshCollider& mesh, ContactPoint& out)
    {
        return FindDeepestContact(mesh.GetBVH(), SphereQueryBounds(sphere),
            [&](const Triangle& triangle, ContactPoint& contact) {
                return TestSphereTriangle(sphere, triangle, contact);
            }, out) != nullptr;
    }

    bool PhysicsSolver::TestAABBTriangleMesh(const AABBCollider& box,
        const TriangleMeshCollider& mesh, ContactPoint& out)
    {
        return FindDeepestContact(mesh.GetBVH(), box.GetAABB(),
            [&](const Triangle& triangle, ContactPoint& contact) {
                return TestAABBTriangle(box, triangle, contact);
            }, out) != nullptr;
    }

    bool PhysicsSolver::TestCapsuleTriangleMesh(const CapsuleCollider& capsule,
        const TriangleMeshCollider& mesh, ContactPoint& out)
    {
        std::vector<ContactPoint> candidates;
        candidates.reserve(8);
        mesh.GetBVH().Query(capsule.GetAABB(), [&](const Triangle& triangle) {
            ContactPoint candidate;
            if (TestCapsuleTriangle(capsule, triangle, candidate)) {
                candidates.push_back(candidate);
            }
        });
        return MergeCapsuleContactPatch(candidates, capsule.m_radius, out);
    }

    bool PhysicsSolver::TestOBBTriangleMesh(const OBBCollider& box,
        const TriangleMeshCollider& mesh, ContactPoint& out)
    {
        const math::Quaternion inverseRotation = box.GetRotation().Inverse();
        AABBCollider localBox(box.m_halfExtents);
        localBox.Update(math::Vector3::ZERO, math::Quaternion::Identity());
        const Triangle* deepestTriangle = FindDeepestContact(mesh.GetBVH(), box.GetAABB(),
            [&](const Triangle& triangle, ContactPoint& contact) {
                return TestAABBTriangle(localBox, TriangleInBoxSpace(triangle, box, inverseRotation), contact);
            }, out);
        if (!deepestTriangle) {
            return false;
        }
        out.point = ProjectBoxSupport(out.normal, box.m_halfExtents,
                                      TriangleInBoxSpace(*deepestTriangle, box, inverseRotation));
        out.normal = (box.GetRotation() * out.normal).Normalized();
        out.point = box.GetCenter() + box.GetRotation() * out.point;
        return true;
    }

    bool PhysicsSolver::TestConvexHullTriangleMesh(const ConvexHullCollider& hull,
        const TriangleMeshCollider& mesh, ContactPoint& out)
    {
        return FindDeepestContact(mesh.GetBVH(), hull.GetAABB(),
            [&](const Triangle& triangle, ContactPoint& contact) {
                return contact_geometry_detail::TestConvexTriangle(&hull, ConvexHullCollider::SupportFnImpl, triangle, contact);
            }, out) != nullptr;
    }

    bool PhysicsSolver::TestCylinderTriangleMesh(const CylinderCollider& cylinder,
        const TriangleMeshCollider& mesh, ContactPoint& out)
    {
        return FindDeepestContact(mesh.GetBVH(), cylinder.GetAABB(),
            [&](const Triangle& triangle, ContactPoint& contact) {
                return contact_geometry_detail::TestConvexTriangle(&cylinder, CylinderCollider::SupportFnImpl, triangle, contact);
            }, out) != nullptr;
    }

    bool PhysicsSolver::TestSphereHeightField(const SphereCollider& sphere,
        const HeightFieldCollider& heightField, ContactPoint& out)
    {
        return FindDeepestContact(heightField.GetBVH(), SphereQueryBounds(sphere),
            [&](const Triangle& triangle, ContactPoint& contact) {
                return TestSphereTriangle(sphere, triangle, contact);
            }, out) != nullptr;
    }

    bool PhysicsSolver::TestAABBHeightField(const AABBCollider& box,
        const HeightFieldCollider& heightField, ContactPoint& out)
    {
        return FindDeepestContact(heightField.GetBVH(), box.GetAABB(),
            [&](const Triangle& triangle, ContactPoint& contact) {
                return TestAABBTriangle(box, triangle, contact);
            }, out) != nullptr;
    }

    bool PhysicsSolver::TestCapsuleHeightField(const CapsuleCollider& capsule,
        const HeightFieldCollider& heightField, ContactPoint& out)
    {
        return FindDeepestContact(heightField.GetBVH(), capsule.GetAABB(),
            [&](const Triangle& triangle, ContactPoint& contact) {
                return TestCapsuleTriangle(capsule, triangle, contact);
            }, out) != nullptr;
    }

    bool PhysicsSolver::TestOBBHeightField(const OBBCollider& box,
        const HeightFieldCollider& heightField, ContactPoint& out)
    {
        const math::Quaternion inverseRotation = box.GetRotation().Inverse();
        AABBCollider localBox(box.m_halfExtents);
        localBox.Update(math::Vector3::ZERO, math::Quaternion::Identity());
        const Triangle* deepestTriangle = FindDeepestContact(heightField.GetBVH(), box.GetAABB(),
            [&](const Triangle& triangle, ContactPoint& contact) {
                return TestAABBTriangle(localBox, TriangleInBoxSpace(triangle, box, inverseRotation), contact);
            }, out);
        if (!deepestTriangle) {
            return false;
        }
        out.normal = (box.GetRotation() * out.normal).Normalized();
        out.point = box.GetCenter() + box.GetRotation() * out.point;
        return true;
    }

    bool PhysicsSolver::TestConvexHullHeightField(const ConvexHullCollider& hull,
        const HeightFieldCollider& heightField, ContactPoint& out)
    {
        return FindDeepestContact(heightField.GetBVH(), hull.GetAABB(),
            [&](const Triangle& triangle, ContactPoint& contact) {
                return contact_geometry_detail::TestConvexTriangle(&hull, ConvexHullCollider::SupportFnImpl, triangle, contact);
            }, out) != nullptr;
    }

    bool PhysicsSolver::TestCylinderHeightField(const CylinderCollider& cylinder,
        const HeightFieldCollider& heightField, ContactPoint& out)
    {
        return FindDeepestContact(heightField.GetBVH(), cylinder.GetAABB(),
            [&](const Triangle& triangle, ContactPoint& contact) {
                return contact_geometry_detail::TestConvexTriangle(&cylinder, CylinderCollider::SupportFnImpl, triangle, contact);
            }, out) != nullptr;
    }
}
