/// @file    FlowVolume.cpp
/// @brief   媒質の流れ (風・水流) が剛体を運ぶ Volume。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <Physics/FlowVolume.hpp>
#include <Physics/FlowDrag.hpp>
#include <utility>

namespace fbzz::physics
{
    FlowVolume::FlowVolume(FlowVolumeDesc desc)
        : m_desc(std::move(desc))
    {
    }

    bool FlowVolume::Contains(const math::Vector3& /*position*/) const
    {
        return true;
    }

    void FlowVolume::Apply(RigidBody& body, float /*dt*/)
    {
        if (body.IsStatic() || !m_desc.flowVelocity) return;

        /// @note 結合係数を «体» が持つ理由: 既定 0 = 流れを受けない (オプトイン)。
        ///       既定で全剛体が風に流されると、置いてある箱や敵が勝手に動き出す。
        const float coupling = body.GetFlowCoupling();
        if (coupling <= 0.0f) return;

        const math::Vector3 flow = m_desc.flowVelocity(body.GetPosition());
        /// @note NoWake で入れる理由: 流れは毎 substep 掛かる環境力。WakeUp すると
        ///       風の中で止まった body が永久に Sleep できず World::Step が重くなる。
        body.ApplyForceNoWake(FlowDragForce(coupling, body.GetMass(), flow, body.GetVelocity()));
    }
} // namespace fbzz::physics
