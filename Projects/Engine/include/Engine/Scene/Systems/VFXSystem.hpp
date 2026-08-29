/// @file    VFXSystem.hpp
/// @brief   VFXComponent の時刻を進め、配下の VFXElement とエンベロープへ配る Scheduler System。
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

class VFXSystem final : public ISystem {
public:
    std::string_view Name() const override { return "VFXSystem"; }
    Phase GetPhase() const override { return Phase::LateScript; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    ComponentAccess GetAccess() const override;
    OrderingHints GetOrder() const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
