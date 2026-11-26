/// @file    GJKDistance.cpp
/// @brief   非交差の凸形状間の距離と、両形状上の最近点を求める。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/Collision/NarrowPhase/GJK.hpp>
#include <Math/Scalar/MathUtils.hpp>
#include "MinkowskiSupport.hpp"
#include <array>
#include <cmath>
#include <limits>

namespace fbzz::physics
{
    namespace
    {
        using SimplexWeights = std::array<float, 4>;

        std::array<float, 2> ClosestSegmentWeights(const math::Vector3& startPoint,
                                                  const math::Vector3& endPoint)
        {
            const math::Vector3 segment = endPoint - startPoint;
            const float lengthSquared = segment.LengthSq();
            if (lengthSquared < 1e-20f) {
                return {1.0f, 0.0f};
            }

            const float projection = math::Vector3::Dot(-startPoint, segment) / lengthSquared;
            const float fraction = math::Clamp01(projection);
            return {1.0f - fraction, fraction};
        }

        /// @note Ericson の領域判定で、頂点・辺・面のうち原点に最も近い特徴を選ぶ。
        /// @see https://realtimecollisiondetection.net/ Real-Time Collision Detection, §5.1.5。
        std::array<float, 3> ClosestTriangleWeights(const math::Vector3& vertexA,
                                                   const math::Vector3& vertexB,
                                                   const math::Vector3& vertexC)
        {
            const math::Vector3 edgeAB = vertexB - vertexA;
            const math::Vector3 edgeAC = vertexC - vertexA;

            // 各頂点から原点への方向を辺へ射影し、最近点が頂点か辺にある場合は対応する重みで返す。
            const float originFromAAlongAB = math::Vector3::Dot(edgeAB, -vertexA);
            const float originFromAAlongAC = math::Vector3::Dot(edgeAC, -vertexA);
            if (originFromAAlongAB <= 0.0f && originFromAAlongAC <= 0.0f) {
                return {1.0f, 0.0f, 0.0f};
            }

            const float originFromBAlongAB = math::Vector3::Dot(edgeAB, -vertexB);
            const float originFromBAlongAC = math::Vector3::Dot(edgeAC, -vertexB);
            if (originFromBAlongAB >= 0.0f && originFromBAlongAC <= originFromBAlongAB) {
                return {0.0f, 1.0f, 0.0f};
            }

            const float regionC = originFromAAlongAB * originFromBAlongAC
                                - originFromBAlongAB * originFromAAlongAC;
            if (regionC <= 0.0f && originFromAAlongAB >= 0.0f && originFromBAlongAB <= 0.0f)
            {
                const float fractionAB = originFromAAlongAB / (originFromAAlongAB - originFromBAlongAB);
                return {1.0f - fractionAB, fractionAB, 0.0f};
            }

            const float originFromCAlongAB = math::Vector3::Dot(edgeAB, -vertexC);
            const float originFromCAlongAC = math::Vector3::Dot(edgeAC, -vertexC);
            if (originFromCAlongAC >= 0.0f && originFromCAlongAB <= originFromCAlongAC) {
                return {0.0f, 0.0f, 1.0f};
            }

            const float regionB = originFromCAlongAB * originFromAAlongAC
                                - originFromAAlongAB * originFromCAlongAC;
            if (regionB <= 0.0f && originFromAAlongAC >= 0.0f && originFromCAlongAC <= 0.0f)
            {
                const float fractionAC = originFromAAlongAC / (originFromAAlongAC - originFromCAlongAC);
                return {1.0f - fractionAC, 0.0f, fractionAC};
            }

            const float regionA = originFromBAlongAB * originFromCAlongAC
                                - originFromCAlongAB * originFromBAlongAC;
            const float originFromBAlongBC = originFromBAlongAC - originFromBAlongAB;
            const float originFromCAlongCB = originFromCAlongAB - originFromCAlongAC;
            if (regionA <= 0.0f && originFromBAlongBC >= 0.0f && originFromCAlongCB >= 0.0f)
            {
                const float fractionBC = originFromBAlongBC / (originFromBAlongBC + originFromCAlongCB);
                return {0.0f, 1.0f - fractionBC, fractionBC};
            }

            // 面内に落ちる場合は、各領域の符号付き面積を全体の面積で割って重みにする。
            const float regionSum = regionA + regionB + regionC;
            if (std::abs(regionSum) < 1e-20f)
            {
                // 面の重みを求められない場合は AB 辺へ射影する。
                const auto [weightA, weightB] = ClosestSegmentWeights(vertexA, vertexB);
                return {weightA, weightB, 0.0f};
            }

            const float inverseSum = 1.0f / regionSum;
            const float weightB = regionB * inverseSum;
            const float weightC = regionC * inverseSum;
            return {1.0f - weightB - weightC, weightB, weightC};
        }

        SimplexWeights ClosestTetrahedronWeights(const Simplex& simplex)
        {
            // 原点を含む四面体の判定で終了した場合は、4 面から近いものを拾う。
            constexpr int FACE_VERTICES[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
            SimplexWeights closestWeights{};
            float closestDistanceSquared = std::numeric_limits<float>::max();

            for (const auto& face : FACE_VERTICES)
            {
                const auto& vertexA = simplex.verts[face[0]].point;
                const auto& vertexB = simplex.verts[face[1]].point;
                const auto& vertexC = simplex.verts[face[2]].point;
                const auto [weightA, weightB, weightC] = ClosestTriangleWeights(vertexA, vertexB, vertexC);
                const math::Vector3 closestOnFace = vertexA * weightA + vertexB * weightB + vertexC * weightC;
                const float distanceSquared = closestOnFace.LengthSq();
                if (distanceSquared < closestDistanceSquared)
                {
                    closestDistanceSquared = distanceSquared;
                    closestWeights = {};
                    closestWeights[face[0]] = weightA;
                    closestWeights[face[1]] = weightB;
                    closestWeights[face[2]] = weightC;
                }
            }
            return closestWeights;
        }

        SimplexWeights ClosestOriginWeights(const Simplex& simplex)
        {
            switch (simplex.size)
            {
            case 1:
                return {1.0f};
            case 2:
            {
                const auto [weightA, weightB] = ClosestSegmentWeights(simplex.verts[0].point, simplex.verts[1].point);
                return {weightA, weightB};
            }
            case 3:
            {
                const auto [weightA, weightB, weightC] = ClosestTriangleWeights(
                    simplex.verts[0].point, simplex.verts[1].point, simplex.verts[2].point);
                return {weightA, weightB, weightC};
            }
            case 4:
                return ClosestTetrahedronWeights(simplex);
            default:
                return {};
            }
        }

        math::Vector3 ClosestPointInDifference(const Simplex& simplex, const SimplexWeights& weights)
        {
            math::Vector3 closestPoint = math::Vector3::ZERO;
            for (int vertexIndex = 0; vertexIndex < simplex.size; ++vertexIndex) {
                closestPoint += simplex.verts[vertexIndex].point * weights[vertexIndex];
            }
            return closestPoint;
        }

        void RecoverClosestPoints(const Simplex& simplex, GJKResult& result)
        {
            if (simplex.size <= 0) {
                return;
            }

            // 差空間の重みを両形状の支持点へ適用すると、元形状上の最近点が得られる。
            const SimplexWeights weights = ClosestOriginWeights(simplex);
            math::Vector3 closestA = math::Vector3::ZERO;
            math::Vector3 closestB = math::Vector3::ZERO;
            for (int vertexIndex = 0; vertexIndex < simplex.size; ++vertexIndex)
            {
                closestA += simplex.verts[vertexIndex].suppA * weights[vertexIndex];
                closestB += simplex.verts[vertexIndex].suppB * weights[vertexIndex];
            }

            result.closestA = closestA;
            result.closestB = closestB;
            result.distance = (closestA - closestB).Length();
        }

        bool RepeatsSupportPoint(const Simplex& simplex, const Simplex::Vertex& supportVertex)
        {
            for (int vertexIndex = 0; vertexIndex < simplex.size; ++vertexIndex)
            {
                if ((simplex.verts[vertexIndex].point - supportVertex.point).LengthSq() < 1e-12f) {
                    return true;
                }
            }
            return false;
        }
    }

    GJKResult GJK_Distance(
        const void* shapeA, SupportFn supportA,
        const void* shapeB, SupportFn supportB,
        int maxIterations, float tolerance)
    {
        // 重なりによる単体の振動は距離の反復で区別できないため、交差は先に確定する。
        GJKResult result = GJK_Intersect(shapeA, supportA, shapeB, supportB, maxIterations);
        if (result.intersects) {
            return result;
        }

        const convex_contact_detail::MinkowskiSupport support{shapeA, supportA, shapeB, supportB};
        Simplex simplex;
        simplex.Add(support.SampleDifference(math::Vector3::RIGHT));
        math::Vector3 closestPoint = simplex.verts[0].point;

        for (int iteration = 0; iteration < maxIterations; ++iteration)
        {
            const float distanceSquared = closestPoint.LengthSq();
            // 非交差でも数値誤差で原点に達した場合は、その時点の単体で最近点を復元する。
            if (distanceSquared < 1e-12f) {
                break;
            }

            const Simplex::Vertex supportVertex = support.SampleDifference(-closestPoint);
            // 距離の上界と下界の差に上界を掛け、平方根一回で許容差と比較する。
            const float distanceUpperBound = std::sqrt(distanceSquared);
            if (distanceSquared - math::Vector3::Dot(supportVertex.point, closestPoint)
                <= tolerance * distanceUpperBound) {
                break;
            }

            if (RepeatsSupportPoint(simplex, supportVertex)) {
                break;
            }

            simplex.Add(supportVertex);
            math::Vector3 unusedSearchDirection;
            if (DoSimplex(simplex, unusedSearchDirection)) {
                break;
            }
            closestPoint = ClosestPointInDifference(simplex, ClosestOriginWeights(simplex));
        }

        RecoverClosestPoints(simplex, result);
        return result;
    }
}
