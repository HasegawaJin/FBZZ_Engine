/// @file    GJK.cpp
/// @brief   GJK の交差判定と EPA へ渡す初期単体の構築。
/// @author  Hasegawa Jin
/// @date    2026-05-24

#include <Physics/GJK.hpp>
#include "MinkowskiSupport.hpp"
#include <cmath>

namespace fbzz::physics
{
    namespace
    {
        using convex_contact_detail::MinkowskiSupport;

        void ExpandForEPA(Simplex& simplex, const MinkowskiSupport& support)
        {
            const auto sampleUnitDirection = [&](const math::Vector3& direction)
            {
                const float length = direction.Length();
                const math::Vector3 unitDirection = length > 1e-10f
                    ? direction * (1.0f / length) : math::Vector3::UP;
                return support.SampleDifference(unitDirection);
            };

            if (simplex.size == 2)
            {
                const math::Vector3 segment = simplex.verts[0].point - simplex.verts[1].point;
                const math::Vector3 referenceAxis = std::abs(segment.x) < 0.57f
                    ? math::Vector3::RIGHT : math::Vector3::UP;
                simplex.Add(sampleUnitDirection(math::Vector3::Cross(segment, referenceAxis)));
            }

            if (simplex.size == 3)
            {
                const math::Vector3 firstEdge = simplex.verts[1].point - simplex.verts[0].point;
                const math::Vector3 secondEdge = simplex.verts[2].point - simplex.verts[0].point;
                const math::Vector3 faceNormal = math::Vector3::Cross(firstEdge, secondEdge);
                Simplex::Vertex supportVertex = sampleUnitDirection(faceNormal);
                /// @note 追加点が面と同一平面に乗るときは反対側も試す。
                if (std::abs(math::Vector3::Dot(supportVertex.point - simplex.verts[0].point, faceNormal)) < 1e-6f)
                    supportVertex = sampleUnitDirection(-faceNormal);
                simplex.Add(supportVertex);
            }
        }
    }

    GJKResult GJK_Intersect(
        const void* shapeA, SupportFn supportA,
        const void* shapeB, SupportFn supportB,
        int maxIterations)
    {
        GJKResult result;
        const MinkowskiSupport support{shapeA, supportA, shapeB, supportB};

        /// @note 初期方向だけは両形状の同じ側の支持点の差を使う。
        math::Vector3 searchDirection = support.SampleA(math::Vector3::RIGHT)
                                      - support.SampleB(math::Vector3::RIGHT);
        if (searchDirection.LengthSq() < 1e-10f)
            searchDirection = math::Vector3::UP;

        Simplex simplex;
        simplex.Add(support.SampleDifference(searchDirection));
        searchDirection = -simplex.verts[0].point;

        for (int iteration = 0; iteration < maxIterations; ++iteration)
        {
            if (searchDirection.LengthSq() < 1e-10f) break;

            const Simplex::Vertex supportVertex = support.SampleDifference(searchDirection);
            /// @note 探索方向の最遠点が原点まで届かないなら、Minkowski 差は原点を含まない。
            if (math::Vector3::Dot(supportVertex.point, searchDirection) < 0.0f)
                return result;

            simplex.Add(supportVertex);
            if (DoSimplex(simplex, searchDirection))
            {
                if (simplex.size < 4)
                    ExpandForEPA(simplex, support);
                result.intersects = true;
                result.simplex = simplex;
                return result;
            }
        }

        return result;
    }
}
