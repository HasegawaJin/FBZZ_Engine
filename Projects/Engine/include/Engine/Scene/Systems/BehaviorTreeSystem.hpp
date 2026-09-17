/// @file    BehaviorTreeSystem.hpp
/// @brief   BehaviorTreeComponent を毎フレーム評価し、NavMeshAgent 等へ指示を出す System。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// 実行順 (Phase::Navigation): NavMeshSensorSystem → BehaviorTreeSystem → NavMeshPatrolSystem → NavigationSystem
/// @note Patrol より前: BT が先に `SetTarget` を呼べば同フレームで巡回を抑止できる。逆順だと
///       Patrol の `SetDestination` (target を INVALID にする) と毎フレーム上書きし合い振動する。
/// @note Sensor より後: 条件ノードはそのフレームに書かれた知覚結果を読む必要がある。
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
