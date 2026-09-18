/// @file    PhysicsSystem.hpp
/// @brief   Scene と physics::World の同期 System。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// RigidBodyComponent から World へ入力し、シミュレーション後の結果を Transform へ戻す。
/// 固定ステップは SystemScheduler::PhaseConfig で制御する。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"
#include <Physics/BodyHandle.hpp>

namespace fbzz::scene {

class PhysicsSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "PhysicsSystem"; }
    Phase            GetPhase()   const override { return Phase::Physics; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override;
    /// ctx.fixedDt を使う。固定ステップループは SystemScheduler::RunPhase が担う
    void Update(SystemContext& ctx) override;

private:
    /// 流れの場を配る physics::FlowVolume の枠。
    /// @note コンポーネントではなく System が持つ理由: 場はシーン全体に 1 つで、
    ///       どの GameObject にも属さない (WaterComponent::volumeHandle は水面 1 枚ごと)。
    physics::VolumeHandle m_flowVolumeHandle;
};

} // namespace fbzz::scene
