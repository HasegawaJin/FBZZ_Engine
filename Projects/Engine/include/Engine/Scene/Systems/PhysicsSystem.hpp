// FBZZ Engine
// PhysicsSystem.hpp | fbzz::scene
// Scene と physics::World の同期 System
// RigidBodyComponent から World へ入力し、シミュレーション後の結果を Transform へ戻す。
// 固定ステップは SystemScheduler::PhaseConfig で制御する。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class PhysicsSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "PhysicsSystem"; }
    Phase            GetPhase()   const override { return Phase::Physics; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override;
    // ctx.fixedDt を使う。固定ステップループは SystemScheduler::RunPhase が担う
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
