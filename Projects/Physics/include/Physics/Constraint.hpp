// FBZZ Engine
// Constraint.hpp | fbzz::physics
// 剛体間制約の基底クラス
#pragma once
#include <Physics/RigidBody.hpp>

namespace fbzz::physics
{
    enum class ConstraintType
    {
        DISTANCE,
        SPRING,
        ROPE,
        CHAIN,
        HINGE,
        FIXED,
        SLIDER
    };

    // World が unique_ptr で寿命を管理し、制約対象の RigidBody は非所有ポインタで参照する。
    class Constraint
    {
    public:
        Constraint(RigidBody* bodyA, RigidBody* bodyB);
        virtual ~Constraint() = default;

        // バネのような力ベース制約は積分前に ApplyForce() で処理する。
        virtual void ApplyForce(float dt) {}
        // ロープやヒンジのような位置ベース制約は積分後に SolvePosition() で補正する。
        virtual void SolvePosition(float dt) {}
        virtual ConstraintType GetType() const = 0;

        RigidBody* GetBodyA() const { return m_bodyA; }
        RigidBody* GetBodyB() const { return m_bodyB; }

    protected:
        RigidBody* m_bodyA = nullptr;
        RigidBody* m_bodyB = nullptr;
    };
} // namespace fbzz::physics
