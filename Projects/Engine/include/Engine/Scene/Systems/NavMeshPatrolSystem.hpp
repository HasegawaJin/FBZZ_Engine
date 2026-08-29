/// @file    NavMeshPatrolSystem.hpp
/// @brief   NavMeshPatrolComponent を持つ GameObject の NavMeshAgentComponent へ、。
/// @author  Hasegawa Jin
/// @date    2026-06-17
///
/// 巡回ウェイポイントの目的地を順番に SetDestination する。NavigationSystem の前に呼ぶ。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class NavMeshPatrolSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "NavMeshPatrolSystem"; }
    Phase            GetPhase()   const override { return Phase::Navigation; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override;
    OrderingHints    GetOrder()   const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
