/// @file    ParticleSimulationSystem.hpp
/// @brief   Particleの再生時間・Emission要求を描画回数から独立してフレーム単位で更新する。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

// ParticleSimulationSystem — Particleの再生状態とEmissionスケジュールをLateUpdateで一度だけ進める。
class ParticleSimulationSystem final : public ISystem {
public:
    std::string_view Name() const override { return "ParticleSimulationSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    ComponentAccess GetAccess() const override;
    OrderingHints GetOrder() const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
