/// @file    ChainConstraint.hpp
/// @brief   複数剛体を固定距離でつなぐ鎖制約。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#pragma once
#include <Physics/Constraint.hpp>
#include <vector>

namespace fbzz::physics
{
    // 隣接ボディ同士の距離を反復補正する。布やロープの簡易表現用。
    class ChainConstraint : public Constraint
    {
    public:
        ChainConstraint(std::vector<RigidBody*> bodies, float segmentLength, int solverIterations = 4);

        // 位置制約として解くため、力の段階では何もしない。
        void ApplyForce(float /*dt*/) override {}
        void SolvePosition(float dt) override;
        ConstraintType GetType() const override { return ConstraintType::CHAIN; }

        const std::vector<RigidBody*>& GetBodies() const { return m_bodies; }

        // WHY 公開するか: 他の制約 (m_distance / m_maxLength / m_stiffness) と同じく、
        //     張ったあとに調整する値。連なりだけは並びの整合が要るので private に残す。
        float m_segmentLength = 1.0f;
        int m_solverIterations = 4;

    private:
        std::vector<RigidBody*> m_bodies;
    };
} // namespace fbzz::physics
