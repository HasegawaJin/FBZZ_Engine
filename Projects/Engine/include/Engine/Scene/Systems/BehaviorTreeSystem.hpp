/// @file    BehaviorTreeSystem.hpp
/// @brief   BehaviorTreeComponent を毎フレーム評価し、NavMeshAgent 等へ指示を出す System。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// 実行位置 (Phase::Navigation):
/// NavMeshSensorSystem → (PerceptionSystem) → BehaviorTreeSystem → NavMeshPatrolSystem → NavigationSystem
///
/// WHY Patrol より前か: NavMeshPatrolSystem は `agent->target.IsValid()` で
/// 追跡中の巡回を止める。BT が先に SetTarget を呼べば同じフレームで巡回が抑止される。
/// 逆順だと Patrol の SetDestination (target を INVALID にする) と
/// 毎フレーム上書きし合って振動する。
///
/// WHY Sensor より後か: 条件ノードはそのフレームに書かれた知覚結果を読む必要がある。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class BehaviorTreeSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "BehaviorTreeSystem"; }
    Phase            GetPhase()   const override { return Phase::Navigation; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override;
    OrderingHints    GetOrder()   const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
