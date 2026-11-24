/// @file    LightFlickerSystem.hpp
/// @brief   LightComponent の明滅設定から intensity を毎フレーム作り直す
/// @author  Hasegawa Jin
/// @date    2026-09-12

/// @note 揺れの形は LightComponent (flickerMode/Amplitude/Frequency/Noise/Phase/Seed/Curve) が持ち、
/// @note ここは «掛けて書く» だけを担う。
/// @note Play 中だけ動かす理由: intensity は保存されるフィールドのため、編集中も動かすと揺れの
/// @note 途中値をシーンへ保存してしまい、オーサリングしたピークが戻らなくなる。
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

class LightFlickerSystem final : public ISystem {
public:
    std::string_view Name() const override { return "LightFlickerSystem"; }
    Phase GetPhase() const override { return Phase::LateScript; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    FBZZ_ENGINE_API ComponentAccess GetAccess() const override;
    FBZZ_ENGINE_API OrderingHints GetOrder() const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

} /// @note namespace fbzz::scene
