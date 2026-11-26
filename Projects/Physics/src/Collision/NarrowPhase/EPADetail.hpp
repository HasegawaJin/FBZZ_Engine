/// @file    EPADetail.hpp
/// @brief   EPA のポリトープ面と接触結果生成の境界。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#pragma once
#include <Physics/Collision/NarrowPhase/EPA.hpp>
#include <vector>

namespace fbzz::physics::convex_contact_detail
{
    struct MinkowskiSupport;

    struct PolytopeFace
    {
        Simplex::Vertex vertices[3]; ///< 差空間の面を構成する支持頂点の添字。
        math::Vector3 outwardNormal; ///< 差空間の多面体の外側を向く面の単位法線。
        float originDistance; ///< 差空間の原点から面までの法線に沿う距離 [m]。
    };

    class EPAPolytope
    {
    public:
        /// @pre simplex は 4 頂点を持つ。
        explicit EPAPolytope(const Simplex& simplex);

        [[nodiscard]] bool ExceedsFaceBudget() const;
        /// @return 有効な面がなければ nullptr。返した面は次の Expand まで有効。
        [[nodiscard]] const PolytopeFace* FindClosestFace() const;
        void Expand(const Simplex::Vertex& supportVertex);

    private:
        std::vector<PolytopeFace> m_faces; ///< 貫通方向の候補として保持する差空間の面。
    };

    [[nodiscard]] EPAResult BuildEPAContact(const PolytopeFace& face,
                                            const MinkowskiSupport& support,
                                            bool faceBudgetExceeded);
}
