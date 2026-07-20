// FBZZ Engine
// VFXGraphSystem.hpp | fbzz::scene
// .vfx DAGを評価して複数Componentの生成・開始・停止を統括するScheduler System
#pragma once

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

class VFXGraphSystem final : public ISystem {
public:
    std::string_view Name() const override { return "VFXGraphSystem"; }
    Phase GetPhase() const override { return Phase::LateScript; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    ComponentAccess GetAccess() const override;
    OrderingHints GetOrder() const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
