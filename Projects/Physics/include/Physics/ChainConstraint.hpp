// FBZZ Engine
// ChainConstraint.hpp | fbzz::physics
// 複数剛体を固定距離でつなぐ鎖制約
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

    private:
        std::vector<RigidBody*> m_bodies;
        float m_segmentLength = 1.0f;
        int m_solverIterations = 4;
    };
} // namespace fbzz::physics
