/// @file    ClothSystem.hpp
/// @brief   布の固定更新と描画メッシュの転送。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {
class ClothSystem final : public ISystem {
public:
    std::string_view Name() const override { return "ClothSystem"; }
    Phase GetPhase() const override { return Phase::Physics; }
    RunMode GetRunMode() const override { return RunMode::SimOnly; }
    FBZZ_ENGINE_API ComponentAccess GetAccess() const override;
    FBZZ_ENGINE_API OrderingHints GetOrder() const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};
class ClothRenderSystem final : public ISystem {
public:
    std::string_view Name() const override { return "ClothRenderSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    FBZZ_ENGINE_API ComponentAccess GetAccess() const override;
    FBZZ_ENGINE_API OrderingHints GetOrder() const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};
}
