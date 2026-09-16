/// @file    XPBDSolver.cpp
/// @brief   substep 積分と拘束解決のループ
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <Physics/XPBDSolver.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::physics
{
    void XPBDSolver::SetSubsteps(int substeps)
    {
        m_substeps = std::clamp(substeps, 1, 64);
    }

    void XPBDSolver::AddBody(RigidBody* body)
    {
        if (!body) return;
        for (const BodyState& state : m_bodies)
            if (state.body == body) return;

        BodyState state{};
        state.body            = body;
        state.prevPosition    = body->GetPosition();
        state.prevRotation    = body->GetRotation();
        state.restoreSleeping = body->m_allowSleeping;

        body->m_allowSleeping = false;
        body->WakeUp();
        m_bodies.push_back(state);
    }

    void XPBDSolver::RemoveBody(RigidBody* body)
    {
        const auto it = std::find_if(m_bodies.begin(), m_bodies.end(),
                                     [body](const BodyState& s) { return s.body == body; });
        if (it == m_bodies.end()) return;
        if (it->body) it->body->m_allowSleeping = it->restoreSleeping;
        m_bodies.erase(it);
    }

    void XPBDSolver::ClearBodies()
    {
        for (const BodyState& state : m_bodies)
            if (state.body) state.body->m_allowSleeping = state.restoreSleeping;
        m_bodies.clear();
    }

    void XPBDSolver::AddConstraint(std::unique_ptr<XPBDConstraint> constraint)
    {
        if (constraint) m_constraints.push_back(std::move(constraint));
    }

    void XPBDSolver::ClearConstraints()
    {
        m_constraints.clear();
    }

    void XPBDSolver::ClearTransient()
    {
        m_transient.clear();
    }

    void XPBDSolver::AddTransient(XPBDConstraint* constraint)
    {
        if (constraint) m_transient.push_back(constraint);
    }

    void XPBDSolver::Step(float dt)
    {
        Step(dt, nullptr, nullptr);
    }

    void XPBDSolver::Step(float dt, void (*projectPositions)(void*), void* context)
    {
        if (!std::isfinite(dt) || dt <= 0.0f || m_bodies.empty()) return;

        const int   substeps = std::max(m_substeps, 1);
        const float h        = dt / static_cast<float>(substeps);

        for (int step = 0; step < substeps; ++step) {
            for (BodyState& state : m_bodies) Integrate(state, h);

            for (auto& constraint : m_constraints) {
                constraint->ResetLambda();
                constraint->SolvePosition(h);
            }
            for (XPBDConstraint* constraint : m_transient) {
                constraint->ResetLambda();
                constraint->SolvePosition(h);
            }

            if (projectPositions) projectPositions(context);
            for (const BodyState& state : m_bodies) DeriveVelocity(state, h);

            for (auto& constraint : m_constraints) constraint->SolveVelocity(h);
            for (XPBDConstraint* constraint : m_transient) constraint->SolveVelocity(h);
        }
    }

    void XPBDSolver::Integrate(BodyState& state, float h) const
    {
        RigidBody* body = state.body;
        if (!body || body->IsStatic()) return;

        state.prevPosition = body->GetPosition();
        state.prevRotation = body->GetRotation();

        math::Vector3 velocity = body->GetVelocity();
        if (body->m_useGravity) velocity += m_gravity * (body->m_gravityScale * h);
        velocity = velocity * (1.0f / (1.0f + std::max(body->m_linearDrag, 0.0f) * h));

        body->SetVelocity(velocity);
        body->SetPosition(state.prevPosition + velocity * h);

        // WHY ジャイロ項 (ω × Iω) を積まないか: 明示的に積むと高速回転で発散しやすく、
        //     PhysX も既定で切っている。関節で拘束された肢では効きが小さいので、
        //     安定と引き換えに落とす。自由回転する単体剛体では «テニスラケットの定理»
        //     が出ないという既知の差になる。
        const math::Vector3 angularVelocity =
            body->GetAngularVelocity() *
            (1.0f / (1.0f + std::max(body->m_angularDrag, 0.0f) * h));
        body->SetAngularVelocity(angularVelocity);
        body->SetRotation(IntegrateRotation(state.prevRotation, angularVelocity, h));
    }

    void XPBDSolver::DeriveVelocity(const BodyState& state, float h) const
    {
        RigidBody* body = state.body;
        if (!body || body->IsStatic() || h <= 0.0f) return;

        body->SetVelocity((body->GetPosition() - state.prevPosition) * (1.0f / h));
        body->SetAngularVelocity(
            AngularVelocityFromDelta(state.prevRotation, body->GetRotation(), h));
    }
} // namespace fbzz::physics
