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
        Step(dt, nullptr, projectPositions, context);
    }

    void XPBDSolver::Step(float dt, void (*prepareContacts)(void*),
                         void (*projectPositions)(void*), void* context)
    {
        Step(dt, prepareContacts, projectPositions, context, XPBDProjectionMode::MOTION);
    }

    void XPBDSolver::Step(float dt, void (*prepareContacts)(void*),
                         void (*projectPositions)(void*), void* context, XPBDProjectionMode mode,
                          int correctionIterations)
    {
        if (!std::isfinite(dt) || dt <= 0.0f || m_bodies.empty()) return;

        const int   substeps = std::max(m_substeps, 1);
        const float h        = dt / static_cast<float>(substeps);
        const int positionIterations = std::max(correctionIterations, 1);

        const auto project = [&]() {
            for (auto& state : m_bodies) {
                state.projectionTranslation = state.body->GetPosition();
                state.projectionRotation = state.body->GetRotation();
            }
            projectPositions(context);
            for (auto& state : m_bodies) {
                state.projectionTranslation = state.body->GetPosition() - state.projectionTranslation;
                const auto rotation = state.body->GetRotation();
                const auto before = state.projectionRotation;
                const bool moved = rotation.x != before.x || rotation.y != before.y
                    || rotation.z != before.z || rotation.w != before.w;
                state.projectionRotation = moved ? rotation * before.Inverse() : math::Quaternion::Identity();
            }
        };

        for (int step = 0; step < substeps; ++step) {
            for (BodyState& state : m_bodies) Integrate(state, h);
            if (prepareContacts) prepareContacts(context);

            for (auto& constraint : m_constraints) constraint->ResetLambda();
            for (XPBDConstraint* constraint : m_transient) constraint->ResetLambda();
            for (auto& constraint : m_constraints) constraint->SolvePhysicalPosition(h);
            for (XPBDConstraint* constraint : m_transient) constraint->SolvePhysicalPosition(h);
            for (auto& state:m_bodies) {
                state.physicalPosition=state.body->GetPosition();
                state.physicalRotation=state.body->GetRotation();
            }

            /// @note 立位制限の後に接触を解き、接地した足を最終投影で床へ押し戻さない。
            if (projectPositions && mode == XPBDProjectionMode::LIMIT) project();

            /// @note 硬い接続・可動域・貫通補正は姿勢だけを直し、補正量/h の偽の速度を作らない。
            /// @see https://box2d.org/posts/2024/02/solver2d/ Nonlinear Gauss-Seidel.
            /// @note 硬い拘束だけを再投影し、接触で再発した関節誤差を収束させる。ばね・把持の有限出力は反復しない。
            for (int iteration = 0; iteration < positionIterations; ++iteration) {
                for (auto& constraint : m_constraints) constraint->SolveCorrectionPosition(h);
                for (XPBDConstraint* constraint : m_transient) constraint->SolveCorrectionPosition(h);
            }

            if (projectPositions && mode == XPBDProjectionMode::MOTION) {
                project();
                /// @note 明示的な運動の投影だけを速度へ反映し、先行する接触・関節の bias は含めない。
                for (auto& state : m_bodies) {
                    state.physicalPosition += state.projectionTranslation;
                    const auto& delta = state.projectionRotation;
                    if (delta.x != 0.0f || delta.y != 0.0f || delta.z != 0.0f || delta.w != 1.0f)
                        state.physicalRotation = (delta * state.physicalRotation).Normalized();
                }
            }
            for (const BodyState& state : m_bodies) DeriveVelocity(state, h);
            if (projectPositions && mode == XPBDProjectionMode::LIMIT) {
                for (const BodyState& state : m_bodies) LimitProjectionVelocity(state);
            }

            for (auto& constraint : m_constraints) constraint->SolveVelocity(h);
            for (XPBDConstraint* constraint : m_transient) constraint->SolveVelocity(h);
        }
    }

    void XPBDSolver::LimitProjectionVelocity(const BodyState& state)
    {
        auto* body = state.body;
        if (!body || body->IsStatic()) return;

        /// @note 投影の向きは許容領域の内側。外向きだけを止め、補正量/h を新しい運動にしない。
        const auto inward = state.projectionTranslation.NormalizedOr(math::Vector3::ZERO);
        const auto velocity = body->GetVelocity();
        const float speed = math::Vector3::Dot(velocity, inward);
        if (speed < 0.0f) body->SetVelocity(velocity - inward * speed);

        const auto delta = RotationVector(state.projectionRotation.Normalized());
        const auto axis = delta.NormalizedOr(math::Vector3::ZERO);
        const float angularSpeed = math::Vector3::Dot(body->GetAngularVelocity(), axis);
        if (angularSpeed < 0.0f) ApplyAngularVelocityChange(body, nullptr, axis * angularSpeed);
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

        /// @note ジャイロ項 (ω × Iω) は積まない。明示的に積むと高速回転で発散しやすく PhysX も
        /// @note 既定で切っている。関節で拘束された肢では効きが小さいため安定と引き換えに
        /// @note 落とし、自由回転する単体剛体では «テニスラケットの定理» が出ない既知の差になる。
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

        body->SetVelocity((state.physicalPosition - state.prevPosition) * (1.0f / h));
        auto angularVelocity=AngularVelocityFromDelta(state.prevRotation,state.physicalRotation,h);
        const auto rotation=body->GetRotation();
        const auto physical=state.physicalRotation;
        if (angularVelocity.LengthSq()>0.0f && (rotation.x!=physical.x || rotation.y!=physical.y
            || rotation.z!=physical.z || rotation.w!=physical.w)) {
            /// @note 3D の幾何回転で I(q) が変わるため、同じ ω でも E=ωᵀI(q)ω/2 が増える。補正だけでは増幅させない。
            /// @see https://box2d.org/posts/2024/02/solver2d/ NGS の幾何補正は運動エネルギーへ混ぜない。
            const auto inverseInertia=[&](const math::Vector3& localAxis) {
                const auto axis=rotation*localAxis;
                return math::Vector3::Dot(axis,body->ApplyInvInertia(axis));
            };
            const math::Vector3 inverse{inverseInertia(math::Vector3::RIGHT),inverseInertia(math::Vector3::UP),
                inverseInertia(math::Vector3::FORWARD)};
            const auto energy=[&](const math::Vector3& local) {
                return (inverse.x>0.0f ? static_cast<double>(local.x)*local.x/inverse.x : 0.0)
                    +(inverse.y>0.0f ? static_cast<double>(local.y)*local.y/inverse.y : 0.0)
                    +(inverse.z>0.0f ? static_cast<double>(local.z)*local.z/inverse.z : 0.0);
            };
            const double before=energy(physical.Conjugate()*angularVelocity);
            const double after=energy(rotation.Conjugate()*angularVelocity);
            if (after>before && std::isfinite(before) && std::isfinite(after))
                angularVelocity=angularVelocity*static_cast<float>(std::sqrt(before/after));
        }
        body->SetAngularVelocity(angularVelocity);
    }
} /// @note namespace fbzz::physics
