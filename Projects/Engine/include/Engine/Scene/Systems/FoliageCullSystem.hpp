// FBZZ Engine
// FoliageCullSystem.hpp | fbzz::scene
// STAMP 子 GO のコライダーをメインカメラ距離で有効/無効切り替えするシステム。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class FoliageCullSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "FoliageCullSystem"; }
    Phase            GetPhase()   const override { return Phase::PreScript; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override;
    OrderingHints    GetOrder()   const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
