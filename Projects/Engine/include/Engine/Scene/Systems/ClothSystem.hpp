/// @file    ClothSystem.hpp
/// @brief   布の固定更新と描画メッシュの転送。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {
class ClothSystem final : public ISystem {
public:
    std::string_view Name() const override { return "ClothSystem"; }
    Phase GetPhase() const override { return Phase::Physics; }
    RunMode GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess GetAccess() const override;
    OrderingHints GetOrder() const override;
    void Update(SystemContext& ctx) override;
};
class ClothRenderSystem final : public ISystem {
public:
    std::string_view Name() const override { return "ClothRenderSystem"; }
    Phase GetPhase() const override { return Phase::LateUpdate; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    ComponentAccess GetAccess() const override;
    OrderingHints GetOrder() const override;
    void Update(SystemContext& ctx) override;
};
}
