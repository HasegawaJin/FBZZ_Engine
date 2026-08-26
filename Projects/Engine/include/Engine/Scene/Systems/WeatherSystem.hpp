/// @file WeatherSystem.hpp
/// @brief WeatherComponent の降雨量から濡れ量を時間積分する
/// @author Hasegawa Jin
/// @date 2026-08-25
#pragma once

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

class WeatherSystem final : public ISystem {
public:
    std::string_view Name() const override { return "WeatherSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    // 編集中は Inspector で入れた wetness をそのまま絵に出したいので走らせない。
    RunMode GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess GetAccess() const override;
    OrderingHints GetOrder() const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
