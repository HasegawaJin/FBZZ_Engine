/// @file    ConstraintDebugGeometry.cpp
/// @brief   制約可視化用のワイヤージオメトリ生成。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include <Physics/ConstraintDebugGeometry.hpp>
#include <Physics/ChainConstraint.hpp>
#include <Physics/HingeConstraint.hpp>
#include <Physics/SliderConstraint.hpp>

namespace fbzz::physics
{
namespace
{
    void AddBodyLine(ConstraintDebugGeometry& out, const RigidBody* bodyA, const RigidBody* bodyB)
    {
        if (!bodyA || !bodyB) return;
        out.lines.push_back({ bodyA->GetPosition(), bodyB->GetPosition() });
    }

    void AddChainLines(ConstraintDebugGeometry& out, const ChainConstraint& chain)
    {
        const std::vector<RigidBody*>& bodies = chain.GetBodies();
        for (size_t i = 0; i + 1 < bodies.size(); ++i)
            AddBodyLine(out, bodies[i], bodies[i + 1]);
    }

    void AddHingeLines(ConstraintDebugGeometry& out, const HingeConstraint& hinge)
    {
        const RigidBody* bodyA = hinge.GetBodyA();
        const RigidBody* bodyB = hinge.GetBodyB();
        if (!bodyA || !bodyB) return;

        const math::Vector3 anchorA = bodyA->GetPosition() + (bodyA->GetRotation() * hinge.m_localAnchorA);
        const math::Vector3 anchorB = bodyB->GetPosition() + (bodyB->GetRotation() * hinge.m_localAnchorB);
        const math::Vector3 center = (anchorA + anchorB) * 0.5f;
        const math::Vector3 axis = hinge.m_axis.Normalized() * 0.35f;

        /// @note 2 つのアンカーのずれ
        out.lines.push_back({ anchorA, anchorB });
        /// @note ヒンジ軸の向き
        out.lines.push_back({ center - axis, center + axis });
    }
} // namespace

ConstraintDebugGeometry BuildConstraintDebugGeometry(const Constraint& constraint)
{
    ConstraintDebugGeometry out;

    switch (constraint.GetType())
    {
    case ConstraintType::CHAIN:
        AddChainLines(out, static_cast<const ChainConstraint&>(constraint));
        break;
        case ConstraintType::HINGE:
            AddHingeLines(out, static_cast<const HingeConstraint&>(constraint));
            break;
        case ConstraintType::SLIDER:
        {
            const auto& slider = static_cast<const SliderConstraint&>(constraint);
            if (const RigidBody* bodyA = slider.GetBodyA()) {
                const math::Vector3 axis = slider.m_axis.Normalized() * 0.5f;
                out.lines.push_back({ bodyA->GetPosition() - axis, bodyA->GetPosition() + axis });
            }
            AddBodyLine(out, constraint.GetBodyA(), constraint.GetBodyB());
            break;
        }
        case ConstraintType::FIXED:
            AddBodyLine(out, constraint.GetBodyA(), constraint.GetBodyB());
            break;
        case ConstraintType::DISTANCE:
        case ConstraintType::SPRING:
        case ConstraintType::ROPE:
        AddBodyLine(out, constraint.GetBodyA(), constraint.GetBodyB());
        break;
    }

    return out;
}

} // namespace fbzz::physics
