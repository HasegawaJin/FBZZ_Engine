/// @file    Constraint.hpp
/// @brief   剛体間制約の基底クラス。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#pragma once
#include <Physics/Api.hpp>
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

    /// @note World が unique_ptr で寿命を管理し、制約対象の RigidBody は非所有ポインタで参照する。
    class Constraint
    {
    public:
        FBZZ_PHYSICS_API Constraint(RigidBody* bodyA, RigidBody* bodyB);
        virtual ~Constraint() = default;

        /// @note バネのような力ベース制約は積分前に ApplyForce() で処理する。
        virtual void ApplyForce(float dt) {}
        /// @note ロープやヒンジのような位置ベース制約は積分後に SolvePosition() で補正する。
        virtual void SolvePosition(float dt) {}
        virtual ConstraintType GetType() const = 0;

        RigidBody* GetBodyA() const { return m_bodyA; }
        RigidBody* GetBodyB() const { return m_bodyB; }

    private:
        friend class World;
        [[nodiscard]] bool HasLiveBodies() const
        {
            return (!m_bodyA || !m_bodyALifetime.expired()) && (!m_bodyB || !m_bodyBLifetime.expired());
        }
        std::weak_ptr<const void> m_bodyALifetime;
        std::weak_ptr<const void> m_bodyBLifetime;

    protected:
        RigidBody* m_bodyA = nullptr;
        RigidBody* m_bodyB = nullptr;
    };
} /// @note namespace fbzz::physics
