// FBZZ Engine
// FootIKSystem.hpp | fbzz::scene
// AnimatorSystem 後に Humanoid 足接地補正を適用する専用システム
#pragma once

#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class FootIKSystem final : public ISystem {
public:
    std::string_view Name() const override { return "FootIKSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    ComponentAccess GetAccess() const override;
    OrderingHints GetOrder() const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
