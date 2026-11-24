/// @file    EPAContact.cpp
/// @brief   EPA の最近面から接触点・貫通深度・法線を復元する。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "EPADetail.hpp"
#include "MinkowskiSupport.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace fbzz::physics::convex_contact_detail
{
    namespace
    {
        /// @note 原点が面上に乗る深さは、軸方向の重なりを測り直す。
        constexpr float DEGENERATE_CONTACT_DEPTH = 1.0e-5f;

        void RecoverFaceContactPoints(const PolytopeFace& face, EPAResult& contact)
        {
            const auto& firstVertex = face.vertices[0];
            const auto& secondVertex = face.vertices[1];
            const auto& thirdVertex = face.vertices[2];
            const math::Vector3 projectedOrigin = face.outwardNormal * face.originDistance;
            const math::Vector3 firstEdge = secondVertex.point - firstVertex.point;
            const math::Vector3 secondEdge = thirdVertex.point - firstVertex.point;
            const math::Vector3 towardProjection = projectedOrigin - firstVertex.point;

            const float firstEdgeSquared = math::Vector3::Dot(firstEdge, firstEdge);
            const float edgeDot = math::Vector3::Dot(firstEdge, secondEdge);
            const float secondEdgeSquared = math::Vector3::Dot(secondEdge, secondEdge);
            const float projectedAlongFirst = math::Vector3::Dot(towardProjection, firstEdge);
            const float projectedAlongSecond = math::Vector3::Dot(towardProjection, secondEdge);
            const float denominator = firstEdgeSquared * secondEdgeSquared - edgeDot * edgeDot;

            float secondWeight = 0.0f;
            float thirdWeight = 0.0f;
            if (std::abs(denominator) > 1e-10f)
            {
                secondWeight = (secondEdgeSquared * projectedAlongFirst - edgeDot * projectedAlongSecond) / denominator;
                thirdWeight = (firstEdgeSquared * projectedAlongSecond - edgeDot * projectedAlongFirst) / denominator;
            }
            const float firstWeight = 1.0f - secondWeight - thirdWeight;

            contact.contactA = firstVertex.suppA * firstWeight + secondVertex.suppA * secondWeight
                             + thirdVertex.suppA * thirdWeight;
            contact.contactB = firstVertex.suppB * firstWeight + secondVertex.suppB * secondWeight
                             + thirdVertex.suppB * thirdWeight;
        }

        void RefineAlongCardinalAxes(const MinkowskiSupport& support,
                                     EPAResult& contact, bool requireAxisSearch)
        {
            const float normalX = std::abs(contact.normal.x);
            const float normalY = std::abs(contact.normal.y);
            const float normalZ = std::abs(contact.normal.z);
            if (!requireAxisSearch && std::max({normalX, normalY, normalZ}) >= 0.75f) return;

            const math::Vector3 axes[6] = {
                math::Vector3::RIGHT, -math::Vector3::RIGHT,
                math::Vector3::UP, -math::Vector3::UP,
                math::Vector3::FORWARD, -math::Vector3::FORWARD
            };

            math::Vector3 shallowestAxis = contact.normal;
            math::Vector3 axisContactA = contact.contactA;
            math::Vector3 axisContactB = contact.contactB;
            float shallowestDepth = std::numeric_limits<float>::max();

            for (const math::Vector3& axis : axes)
            {
                /// @note B→A 法線では A の反対側と B の軸側の支持点で重なりを測る。
                const math::Vector3 supportPointA = support.SampleA(-axis);
                const math::Vector3 supportPointB = support.SampleB(axis);
                const float axisDepth = math::Vector3::Dot(supportPointB - supportPointA, axis);
                if (axisDepth > 0.0f && axisDepth < shallowestDepth)
                {
                    shallowestDepth = axisDepth;
                    shallowestAxis = axis;
                    axisContactA = supportPointA;
                    axisContactB = supportPointB;
                }
            }

            if (shallowestDepth != std::numeric_limits<float>::max())
            {
                contact.normal = shallowestAxis;
                contact.depth = shallowestDepth;
                contact.contactA = axisContactA;
                contact.contactB = axisContactB;
            }
        }
    }

    EPAResult BuildEPAContact(const PolytopeFace& face, const MinkowskiSupport& support,
                              bool faceBudgetExceeded)
    {
        EPAResult contact;
        RecoverFaceContactPoints(face, contact);
        /// @note A−B の外向き法線を反転して、B→A の押し戻し方向に揃える。
        /// @see EPAResult::normal
        contact.normal = -face.outwardNormal;
        contact.depth = face.originDistance;
        contact.valid = true;
        RefineAlongCardinalAxes(support, contact, faceBudgetExceeded || contact.depth <= DEGENERATE_CONTACT_DEPTH);
        return contact;
    }
}
