/// @file    VFXBeamSystem.hpp
/// @brief   VFXBeamComponent の端点を解決し、TrailComponent へ経路を書き込む。
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

class VFXBeamSystem final : public ISystem {
public:
    std::string_view Name() const override { return "VFXBeamSystem"; }
    Phase GetPhase() const override { return Phase::LateScript; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    ComponentAccess GetAccess() const override;
    OrderingHints GetOrder() const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
