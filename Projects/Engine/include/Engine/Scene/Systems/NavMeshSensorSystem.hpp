// FBZZ Engine
// NavMeshSensorSystem.hpp | fbzz::scene
// NavMeshSensorComponent の視野角・距離・遮蔽判定を毎フレーム評価し、
// autoChase が有効なら同 GO の NavMeshAgentComponent を自動で追跡させる。
// NavMeshPatrolSystem より前、NavigationSystem より前に呼ぶ。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class NavMeshSensorSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "NavMeshSensorSystem"; }
    Phase            GetPhase()   const override { return Phase::Navigation; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
