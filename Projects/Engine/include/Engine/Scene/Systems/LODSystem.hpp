// FBZZ Engine
// LODSystem.hpp | fbzz::scene
// メインカメラ上の投影サイズから LODGroupComponent の表示レベルを決定する。
#pragma once

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

class LODSystem final : public ISystem {
public:
    std::string_view Name() const override { return "LODSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    ComponentAccess GetAccess() const override;
    OrderingHints GetOrder() const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
