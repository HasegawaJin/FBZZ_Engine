// FBZZ Engine
// NavigationSystem.hpp | fbzz::scene
// NavMeshAgentComponent を NavMesh 上で A* + Funnel Algorithm によりパス追従させる毎フレームシステム。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class NavigationSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "NavigationSystem"; }
    Phase            GetPhase()   const override { return Phase::Navigation; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override;
    OrderingHints    GetOrder()   const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
