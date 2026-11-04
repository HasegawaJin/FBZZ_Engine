/// @file    NavMeshSensorSystem.hpp
/// @brief   NavMeshSensorComponent の視野角・距離・遮蔽判定を毎フレーム評価し、。
/// @author  Hasegawa Jin
/// @date    2026-06-17

/// @note autoChase が有効なら同 GO の NavMeshAgentComponent を自動で追跡させる。
/// @note NavMeshPatrolSystem より前、NavigationSystem より前に呼ぶ。
#pragma once
#include <Engine/Api.hpp>
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class NavMeshSensorSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "NavMeshSensorSystem"; }
    Phase            GetPhase()   const override { return Phase::Navigation; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    FBZZ_ENGINE_API ComponentAccess  GetAccess()  const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

} /// @note namespace fbzz::scene
