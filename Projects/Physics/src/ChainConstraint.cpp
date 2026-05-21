// FBZZ Engine
// ChainConstraint.cpp | fbzz::physics
// 複数剛体を固定距離でつなぐ鎖制約
#include <Physics/ChainConstraint.hpp>
#include <utility>

namespace fbzz::physics
{
    ChainConstraint::ChainConstraint(std::vector<RigidBody*> bodies,
                                     float segmentLength,
                                     int solverIterations)
        : Constraint(nullptr, nullptr),
          m_bodies(std::move(bodies)),
          m_segmentLength(segmentLength),
          m_solverIterations(solverIterations)
    {
    }

    void ChainConstraint::SolvePosition(float /*dt*/)
    {
        if (m_bodies.size() < 2) return;

        for (int iter = 0; iter < m_solverIterations; ++iter)
        {
            for (size_t i = 0; i + 1 < m_bodies.size(); ++i)
            {
                RigidBody* a = m_bodies[i];
                RigidBody* b = m_bodies[i + 1];
                if (!a || !b) continue;

                const math::Vector3 delta = b->GetPosition() - a->GetPosition();
                const float length = delta.Length();
                if (length < 1e-6f) continue;

                const float invA = a->GetInvMass();
                const float invB = b->GetInvMass();
                const float invSum = invA + invB;
                if (invSum == 0.0f) continue;

                const math::Vector3 correction = delta * ((length - m_segmentLength) / length);
                a->SetPosition(a->GetPosition() + correction * (invA / invSum));
                b->SetPosition(b->GetPosition() - correction * (invB / invSum));
            }
        }
    }
} // namespace fbzz::physics
