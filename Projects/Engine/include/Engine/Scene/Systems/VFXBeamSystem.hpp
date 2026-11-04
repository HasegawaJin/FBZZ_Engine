/// @file    VFXBeamSystem.hpp
/// @brief   VFXBeamComponent の端点を解決し、TrailComponent へ経路を書き込む。
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

class VFXBeamSystem final : public ISystem {
public:
    std::string_view Name() const override { return "VFXBeamSystem"; }
    Phase GetPhase() const override { return Phase::LateScript; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    FBZZ_ENGINE_API ComponentAccess GetAccess() const override;
    FBZZ_ENGINE_API OrderingHints GetOrder() const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

} /// @note namespace fbzz::scene
