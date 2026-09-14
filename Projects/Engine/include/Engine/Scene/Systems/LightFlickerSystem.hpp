/// @file    LightFlickerSystem.hpp
/// @brief   LightComponent の明滅設定から intensity を毎フレーム作り直す
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// 揺れの形は LightComponent が持ち (flickerMode / flickerAmplitude / flickerFrequency /
/// flickerNoise / flickerPhase / flickerSeed / flickerCurve)、ここは «掛けて書く» だけを担う。
///
/// WHY Play 中だけ動かすか: 明滅は intensity という «保存される» フィールドを書き換える。
///     編集中も動かすと、揺れている途中の値をシーンへ保存してしまい、オーサリングした
///     ピークが二度と戻らない。Play を抜ける瞬間に必ず捕獲した値へ戻す。
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
