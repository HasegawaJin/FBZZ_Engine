/// @file    WaterInteractionSystem.hpp
/// @brief   Cleanupの直列境界で接触演出の寿命・粒子・数値snapshotを更新する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#pragma once

#include <Engine/Core/Scheduler/ISystem.hpp>
#include <Engine/Scene/WaterInteraction.hpp>
#include <memory>

namespace fbzz::scene {
class WaterInteractionSystem final : public ISystem {
public:
    std::string_view Name() const override { return "WaterInteractionSystem"; }
    Phase GetPhase() const override { return Phase::Cleanup; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    ComponentAccess GetAccess() const override { return ComponentAccess{}.Unrestricted(); }
    void Update(SystemContext& context) override;
private:
    std::shared_ptr<WaterInteractionSceneStore> m_store;
};
} /// @note namespace fbzz::scene
