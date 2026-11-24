/// @file    ParticleSimulationSystem.hpp
/// @brief   Particleの再生時間・Emission要求を描画回数から独立してフレーム単位で更新する。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

/// @note ParticleSimulationSystem — Particleの再生状態とEmissionスケジュールをLateUpdateで一度だけ進める。
class ParticleSimulationSystem final : public ISystem {
public:
    std::string_view Name() const override { return "ParticleSimulationSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    FBZZ_ENGINE_API ComponentAccess GetAccess() const override;
    FBZZ_ENGINE_API OrderingHints GetOrder() const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

} /// @note namespace fbzz::scene
