/// @file    LightFlickerSystem.hpp
/// @brief   LightComponent の明滅設定から intensity を毎フレーム作り直す
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// 揺れの形は LightComponent (flickerMode/Amplitude/Frequency/Noise/Phase/Seed/Curve) が持ち、
/// ここは «掛けて書く» だけを担う。
/// @note Play 中だけ動かす理由: intensity は保存されるフィールドのため、編集中も動かすと揺れの
///       途中値をシーンへ保存してしまい、オーサリングしたピークが戻らなくなる。
#pragma once

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

class LightFlickerSystem final : public ISystem {
public:
    std::string_view Name() const override { return "LightFlickerSystem"; }
    Phase GetPhase() const override { return Phase::LateScript; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    ComponentAccess GetAccess() const override;
    OrderingHints GetOrder() const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
