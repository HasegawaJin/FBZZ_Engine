/// @file    LODSystem.hpp
/// @brief   メインカメラ上の投影サイズから LODGroupComponent の表示レベルを決定する。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

class LODSystem final : public ISystem {
public:
    std::string_view Name() const override { return "LODSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    FBZZ_ENGINE_API ComponentAccess GetAccess() const override;
    FBZZ_ENGINE_API OrderingHints GetOrder() const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

} /// @note namespace fbzz::scene
