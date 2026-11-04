/// @file    NavMeshPatrolSystem.hpp
/// @brief   NavMeshPatrolComponent を持つ GameObject の NavMeshAgentComponent へ、。
/// @author  Hasegawa Jin
/// @date    2026-06-17

/// @note 巡回ウェイポイントの目的地を順番に SetDestination する。NavigationSystem の前に呼ぶ。
#pragma once
#include <Engine/Api.hpp>
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class NavMeshPatrolSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "NavMeshPatrolSystem"; }
    Phase            GetPhase()   const override { return Phase::Navigation; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    FBZZ_ENGINE_API ComponentAccess  GetAccess()  const override;
    FBZZ_ENGINE_API OrderingHints    GetOrder()   const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

} /// @note namespace fbzz::scene
