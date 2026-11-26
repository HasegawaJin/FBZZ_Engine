/// @file    EPA.cpp
/// @brief   交差が確定した凸形状のポリトープを拡張し、接触面を選ぶ。
/// @author  Hasegawa Jin
/// @date    2026-05-24

#include <Physics/Collision/NarrowPhase/EPA.hpp>
#include "EPADetail.hpp"
#include "MinkowskiSupport.hpp"

namespace fbzz::physics
{
    EPAResult EPA_GetContactInfo(
        const void* shapeA, SupportFn supportA,
        const void* shapeB, SupportFn supportB,
        const Simplex& gjkSimplex,
        int maxIterations, float tolerance)
    {
        if (gjkSimplex.size < 4) {
            return {};
        }

        const convex_contact_detail::MinkowskiSupport support{shapeA, supportA, shapeB, supportB};
        convex_contact_detail::EPAPolytope polytope(gjkSimplex);
        bool faceBudgetExceeded = false;

        for (int iteration = 0; iteration < maxIterations; ++iteration)
        {
            if (polytope.ExceedsFaceBudget())
            {
                faceBudgetExceeded = true;
                break;
            }

            const auto* closestFace = polytope.FindClosestFace();
            if (!closestFace) {
                return {};
            }

            const Simplex::Vertex supportVertex = support.SampleDifference(closestFace->outwardNormal);
            const float supportDistance = math::Vector3::Dot(closestFace->outwardNormal, supportVertex.point);
            if (supportDistance - closestFace->originDistance < tolerance) {
                return convex_contact_detail::BuildEPAContact(*closestFace, support, false);
            }

            polytope.Expand(supportVertex);
        }

        // 反復上限でも、残った有効面から接触を近似する。
        const auto* closestFace = polytope.FindClosestFace();
        if (!closestFace) {
            return {};
        }
        return convex_contact_detail::BuildEPAContact(*closestFace, support, faceBudgetExceeded);
    }
}
