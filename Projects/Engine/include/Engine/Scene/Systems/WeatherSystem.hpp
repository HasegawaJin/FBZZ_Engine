/// @file    WeatherSystem.hpp
/// @brief   WeatherComponent の降雨量から濡れ量を時間積分する
/// @author  Hasegawa Jin
/// @date    2026-08-25
#pragma once

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

class WeatherSystem final : public ISystem {
public:
    std::string_view Name() const override { return "WeatherSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    // 編集中も走らせる。
    //
    // WHY: 以前は SimOnly で «編集中は Inspector で入れた wetness をそのまま出す» 設計
    //      だったが、その結果 rainIntensity を上げても編集中は何も起きなかった。
    //      «手で置いた wetness を残す» 用途は autoWetness を切れば成立するので、
    //      その 1 フラグへ寄せる。編集中は時間積分せず目標値へ即座に合わせる (Update 参照)。
    RunMode GetRunMode() const override { return RunMode::Always; }
    ComponentAccess GetAccess() const override;
    OrderingHints GetOrder() const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
