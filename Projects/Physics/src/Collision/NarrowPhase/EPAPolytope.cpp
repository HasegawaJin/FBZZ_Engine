/// @file    EPAPolytope.cpp
/// @brief   EPA の四面体の構築、最近面の選択、輪郭からの面の再構築。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "EPADetail.hpp"
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace fbzz::physics::convex_contact_detail
{
    namespace
    {
        /// @note 縮退で面が増えたときの輪郭探索を反復数とは別に制限する。
        constexpr std::size_t MAX_POLYTOPE_FACES = 256;
        constexpr float UNUSABLE_FACE_DISTANCE = std::numeric_limits<float>::max();

        PolytopeFace MakeFace(const Simplex::Vertex& firstVertex,
                              const Simplex::Vertex& secondVertex,
                              const Simplex::Vertex& thirdVertex)
        {
            PolytopeFace face;
            face.vertices[0] = firstVertex;
            face.vertices[1] = secondVertex;
            face.vertices[2] = thirdVertex;
            const math::Vector3 faceNormal = math::Vector3::Cross(
                secondVertex.point - firstVertex.point, thirdVertex.point - firstVertex.point);
            const float normalLengthSquared = faceNormal.LengthSq();
            if (normalLengthSquared < 1e-12f)
            {
                // 縮退面は近接面の候補から外す。同じ支持点の繰り返し追加を避けるため。
                face.outwardNormal = math::Vector3::UP;
                face.originDistance = UNUSABLE_FACE_DISTANCE;
                return face;
            }

            face.outwardNormal = faceNormal * (1.0f / std::sqrt(normalLengthSquared));
            face.originDistance = math::Vector3::Dot(face.outwardNormal, firstVertex.point);
            if (face.originDistance < 0.0f)
            {
                std::swap(face.vertices[1], face.vertices[2]);
                face.outwardNormal = -face.outwardNormal;
                face.originDistance = -face.originDistance;
            }
            return face;
        }

        struct HorizonEdge
        {
            Simplex::Vertex start; ///< 地平線の向き付き辺の始点。
            Simplex::Vertex end; ///< 地平線の向き付き辺の終点。
        };

        void AppendHorizonEdge(std::vector<HorizonEdge>& horizonEdges,
                               const Simplex::Vertex& start, const Simplex::Vertex& end)
        {
            // 削除面が共有する逆向きの辺を打ち消し、再構築に必要な輪郭だけを残す。
            for (int edgeIndex = static_cast<int>(horizonEdges.size()) - 1; edgeIndex >= 0; --edgeIndex)
            {
                const auto& edge = horizonEdges[edgeIndex];
                if ((edge.start.point - end.point).LengthSq() < 1e-10f &&
                    (edge.end.point - start.point).LengthSq() < 1e-10f)
                {
                    horizonEdges.erase(horizonEdges.begin() + edgeIndex);
                    return;
                }
            }
            horizonEdges.push_back({start, end});
        }
    }

    EPAPolytope::EPAPolytope(const Simplex& simplex)
    {
        m_faces.reserve(64);
        const auto& vertices = simplex.verts;
        m_faces.push_back(MakeFace(vertices[0], vertices[1], vertices[2]));
        m_faces.push_back(MakeFace(vertices[0], vertices[2], vertices[3]));
        m_faces.push_back(MakeFace(vertices[0], vertices[3], vertices[1]));
        m_faces.push_back(MakeFace(vertices[1], vertices[3], vertices[2]));
    }

    bool EPAPolytope::ExceedsFaceBudget() const
    {
        return m_faces.size() > MAX_POLYTOPE_FACES;
    }

    const PolytopeFace* EPAPolytope::FindClosestFace() const
    {
        const PolytopeFace* closestFace = nullptr;
        float closestDistance = UNUSABLE_FACE_DISTANCE;
        for (const PolytopeFace& face : m_faces)
        {
            if (face.originDistance < closestDistance)
            {
                closestFace = &face;
                closestDistance = face.originDistance;
            }
        }
        return closestFace;
    }

    void EPAPolytope::Expand(const Simplex::Vertex& supportVertex)
    {
        std::vector<HorizonEdge> horizonEdges;
        for (int faceIndex = static_cast<int>(m_faces.size()) - 1; faceIndex >= 0; --faceIndex)
        {
            const PolytopeFace& face = m_faces[faceIndex];
            const float visibility = math::Vector3::Dot(face.outwardNormal,
                                                        supportVertex.point - face.vertices[0].point);
            if (visibility > 0.0f)
            {
                AppendHorizonEdge(horizonEdges, face.vertices[0], face.vertices[1]);
                AppendHorizonEdge(horizonEdges, face.vertices[1], face.vertices[2]);
                AppendHorizonEdge(horizonEdges, face.vertices[2], face.vertices[0]);
                m_faces.erase(m_faces.begin() + faceIndex);
            }
        }

        for (const HorizonEdge& edge : horizonEdges) {
            m_faces.push_back(MakeFace(edge.start, edge.end, supportVertex));
        }
    }
}
