/// @file    ConvexHullBuilder.cpp
/// @brief   初期四面体の選択、凸包面の拡張、頂点と面の確定。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/ConvexHullCollider.hpp>
#include "ConvexHullSupport.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <utility>

namespace fbzz::physics
{
    namespace
    {
        using PointCloud = std::span<const math::Vector3>;
        using TetrahedronIndices = std::array<int, 4>;
        using TriangleIndices = std::array<int, 3>;
        using HullTriangle = std::array<uint32_t, 3>;

        constexpr TriangleIndices TETRAHEDRON_FACES[] = {{0, 1, 2}, {0, 2, 3}, {0, 3, 1}, {1, 3, 2}};
        constexpr float OUTSIDE_FACE_TOLERANCE = 1e-5f;

        struct HullFace
        {
            TriangleIndices pointIndices;
            math::Vector3 outwardNormal;
        };

        struct HullGeometry
        {
            std::vector<math::Vector3> vertices;
            std::vector<HullTriangle> triangles;
        };

        struct HorizonEdge
        {
            int startIndex;
            int endIndex;
        };

        float SignedDistanceToPlane(const math::Vector3& normal, const math::Vector3& planeOrigin,
                                    const math::Vector3& point)
        {
            return math::Vector3::Dot(normal, point - planeOrigin);
        }

        HullFace MakeOutwardFace(PointCloud points, TriangleIndices pointIndices,
                                 const math::Vector3& interiorPoint)
        {
            const math::Vector3& firstPoint = points[pointIndices[0]];
            const math::Vector3 firstEdge = points[pointIndices[1]] - firstPoint;
            const math::Vector3 secondEdge = points[pointIndices[2]] - firstPoint;
            const math::Vector3 faceNormal = math::Vector3::Cross(firstEdge, secondEdge);
            const float length = faceNormal.Length();
            HullFace face{pointIndices, length > 1e-8f ? faceNormal * (1.0f / length) : math::Vector3::UP};
            if (SignedDistanceToPlane(face.outwardNormal, firstPoint, interiorPoint) > 0.0f)
            {
                std::swap(face.pointIndices[1], face.pointIndices[2]);
                face.outwardNormal = -face.outwardNormal;
            }
            return face;
        }

        float DegeneracyTolerance(PointCloud points)
        {
            /// @note 凸包が作れる厚みは点群の大きさに対する相対値で判定する。
            math::Vector3 lowerBound = points.front();
            math::Vector3 upperBound = points.front();
            for (const math::Vector3& point : points)
            {
                lowerBound = {std::min(lowerBound.x, point.x), std::min(lowerBound.y, point.y),
                              std::min(lowerBound.z, point.z)};
                upperBound = {std::max(upperBound.x, point.x), std::max(upperBound.y, point.y),
                              std::max(upperBound.z, point.z)};
            }
            return std::max({upperBound.x - lowerBound.x, upperBound.y - lowerBound.y,
                             upperBound.z - lowerBound.z}) * 1e-6f;
        }

        std::optional<TetrahedronIndices> FindInitialTetrahedron(PointCloud points)
        {
            const int pointCount = static_cast<int>(points.size());
            int minXIndex = 0;
            int maxXIndex = 0;
            for (int pointIndex = 1; pointIndex < pointCount; ++pointIndex)
            {
                if (points[pointIndex].x < points[minXIndex].x) minXIndex = pointIndex;
                if (points[pointIndex].x > points[maxXIndex].x) maxXIndex = pointIndex;
            }
            if (minXIndex == maxXIndex) return std::nullopt;

            const float tolerance = DegeneracyTolerance(points);
            const math::Vector3& baselineStart = points[minXIndex];
            const math::Vector3 baseline = points[maxXIndex] - baselineStart;
            const float baselineLength = baseline.Length();
            int thirdPointIndex = -1;
            float maxLineDistance = tolerance;
            for (int pointIndex = 0; pointIndex < pointCount; ++pointIndex)
            {
                if (pointIndex == minXIndex || pointIndex == maxXIndex) continue;
                const float distance = math::Vector3::Cross(baseline, points[pointIndex] - baselineStart).Length()
                                     / baselineLength;
                if (distance > maxLineDistance)
                {
                    maxLineDistance = distance;
                    thirdPointIndex = pointIndex;
                }
            }
            if (thirdPointIndex < 0) return std::nullopt;

            const math::Vector3 seedPlaneNormal =
                math::Vector3::Cross(baseline, points[thirdPointIndex] - baselineStart)
                    .NormalizedOr(math::Vector3::ZERO);
            if (seedPlaneNormal.LengthSq() < 0.5f) return std::nullopt;

            int fourthPointIndex = -1;
            float maxPlaneDistance = tolerance;
            for (int pointIndex = 0; pointIndex < pointCount; ++pointIndex)
            {
                if (pointIndex == minXIndex || pointIndex == maxXIndex || pointIndex == thirdPointIndex)
                    continue;
                const float distance = std::abs(SignedDistanceToPlane(seedPlaneNormal, baselineStart, points[pointIndex]));
                if (distance > maxPlaneDistance)
                {
                    maxPlaneDistance = distance;
                    fourthPointIndex = pointIndex;
                }
            }
            if (fourthPointIndex < 0) return std::nullopt;
            return TetrahedronIndices{minXIndex, maxXIndex, thirdPointIndex, fourthPointIndex};
        }

        math::Vector3 TetrahedronCenter(PointCloud points, const TetrahedronIndices& tetrahedron)
        {
            return (points[tetrahedron[0]] + points[tetrahedron[1]]
                  + points[tetrahedron[2]] + points[tetrahedron[3]]) * 0.25f;
        }

        HullTriangle ToHullTriangle(const TriangleIndices& pointIndices)
        {
            return {static_cast<uint32_t>(pointIndices[0]), static_cast<uint32_t>(pointIndices[1]),
                    static_cast<uint32_t>(pointIndices[2])};
        }

        void AppendHorizonEdge(std::vector<HorizonEdge>& horizon, HorizonEdge candidate)
        {
            /// @note 削除する面同士が共有する逆向きの辺を相殺する。
            for (int edgeIndex = static_cast<int>(horizon.size()) - 1; edgeIndex >= 0; --edgeIndex)
            {
                const HorizonEdge& edge = horizon[edgeIndex];
                if (edge.startIndex == candidate.endIndex && edge.endIndex == candidate.startIndex)
                {
                    horizon.erase(horizon.begin() + edgeIndex);
                    return;
                }
            }
            horizon.push_back(candidate);
        }

        void ExpandHullWithPoint(PointCloud points, int pointIndex,
                                 std::vector<HullFace>& faces, math::Vector3& interiorPoint)
        {
            const math::Vector3& point = points[pointIndex];
            const std::size_t previousFaceCount = faces.size();
            std::vector<HorizonEdge> horizon;
            for (int faceIndex = static_cast<int>(faces.size()) - 1; faceIndex >= 0; --faceIndex)
            {
                const HullFace& face = faces[faceIndex];
                const TriangleIndices& indices = face.pointIndices;
                const float distance = SignedDistanceToPlane(face.outwardNormal, points[indices[0]], point);
                if (distance > OUTSIDE_FACE_TOLERANCE)
                {
                    AppendHorizonEdge(horizon, {indices[0], indices[1]});
                    AppendHorizonEdge(horizon, {indices[1], indices[2]});
                    AppendHorizonEdge(horizon, {indices[2], indices[0]});
                    faces.erase(faces.begin() + faceIndex);
                }
            }
            if (faces.size() == previousFaceCount) return;

            for (const HorizonEdge& edge : horizon)
                faces.push_back(MakeOutwardFace(points, {edge.startIndex, edge.endIndex, pointIndex}, interiorPoint));

            /// @note 面を張り替えた後の面数で内点の更新を重み付けする。
            interiorPoint = (interiorPoint * static_cast<float>(faces.size() - 1) + point)
                          * (1.0f / static_cast<float>(faces.size()));
        }

        HullGeometry CompactHull(PointCloud points, const std::vector<HullFace>& faces)
        {
            std::vector<bool> usedByFace(points.size(), false);
            for (const HullFace& face : faces)
                for (const int pointIndex : face.pointIndices)
                    usedByFace[pointIndex] = true;

            HullGeometry hull;
            hull.vertices.reserve(points.size());
            std::vector<int> hullIndexByInput(points.size(), -1);
            /// @note 入力順で頂点を詰め、確定した添字を面にも適用する。
            for (std::size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex)
            {
                if (!usedByFace[pointIndex]) continue;
                hullIndexByInput[pointIndex] = static_cast<int>(hull.vertices.size());
                hull.vertices.push_back(points[pointIndex]);
            }

            hull.triangles.reserve(faces.size());
            for (const HullFace& face : faces)
            {
                const TriangleIndices localIndices{
                    hullIndexByInput[face.pointIndices[0]],
                    hullIndexByInput[face.pointIndices[1]],
                    hullIndexByInput[face.pointIndices[2]]
                };
                if (localIndices[0] < 0 || localIndices[1] < 0 || localIndices[2] < 0) continue;
                hull.triangles.push_back(ToHullTriangle(localIndices));
            }
            return hull;
        }

        HullGeometry BuildIncrementalHull(std::vector<math::Vector3> points)
        {
            if (points.size() <= 4)
            {
                HullGeometry hull{std::move(points), {}};
                if (hull.vertices.size() == 4)
                {
                    /// @note 入力の頂点順によって向きが変わるため、面は内点を基準に揃える。
                    const math::Vector3 interiorPoint = TetrahedronCenter(hull.vertices, {0, 1, 2, 3});
                    hull.triangles.reserve(4);
                    for (const TriangleIndices& face : TETRAHEDRON_FACES)
                        hull.triangles.push_back(ToHullTriangle(
                            MakeOutwardFace(hull.vertices, face, interiorPoint).pointIndices));
                }
                return hull;
            }

            const auto seed = FindInitialTetrahedron(points);
            /// @note 体積のない点群でも、支持点探索に使えるよう頂点を保持する。
            if (!seed) return {std::move(points), {}};

            math::Vector3 interiorPoint = TetrahedronCenter(points, *seed);
            std::vector<HullFace> faces;
            for (const TriangleIndices& face : TETRAHEDRON_FACES)
                faces.push_back(MakeOutwardFace(points,
                    {(*seed)[face[0]], (*seed)[face[1]], (*seed)[face[2]]}, interiorPoint));

            for (int pointIndex = 0; pointIndex < static_cast<int>(points.size()); ++pointIndex)
                ExpandHullWithPoint(points, pointIndex, faces, interiorPoint);

            return CompactHull(points, faces);
        }
    }

    void ConvexHullCollider::BuildHull(std::vector<math::Vector3> points)
    {
        if (points.empty()) return;

        if (static_cast<int>(points.size()) > MAX_HULL_VERTS)
            points = convex_hull_detail::SelectExtremePoints(points, MAX_HULL_VERTS);

        /// @note 頂点と面を同じ生成結果から受け取り、面が未確定の頂点を参照しないようにする。
        HullGeometry hull = BuildIncrementalHull(std::move(points));
        m_localVerts = std::move(hull.vertices);
        m_faces = std::move(hull.triangles);
    }
}
