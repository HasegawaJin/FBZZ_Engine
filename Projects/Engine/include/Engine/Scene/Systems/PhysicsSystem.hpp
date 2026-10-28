/// @file    PhysicsSystem.hpp
/// @brief   Scene と physics::World の同期 System。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note RigidBodyComponent から World へ入力し、シミュレーション後の結果を Transform へ戻す。
/// @note 固定ステップは SystemScheduler::PhaseConfig で制御する。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"
#include <Engine/Scene/SpatialDebugSnapshot.hpp>
#include <Physics/BodyHandle.hpp>

namespace fbzz::scene {

class PhysicsSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "PhysicsSystem"; }
    Phase            GetPhase()   const override { return Phase::Physics; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override;
    /// @note ctx.fixedDt を使う。固定ステップループは SystemScheduler::RunPhase が担う。
    void Update(SystemContext& ctx) override;

    /// @note Thread-safe; effective changes retire the previous publication and advance its revision.
    /// @return The current request revision; an unchanged request preserves the revision.
    static uint64_t SetSpatialDebugRequest(const PhysicsSpatialDebugRequest& request);
    /// @note Thread-safe; disables capture and rejects any in-flight publication immediately.
    static void InvalidateSpatialDebugSnapshot();
    /// @return Immutable current-revision values for this scene, or null while disabled or awaiting a step.
    [[nodiscard]] static std::shared_ptr<const PhysicsSpatialDebugSnapshot>
        GetSpatialDebugSnapshot(uint64_t sceneGeneration);

private:
    /// @brief 流れの場を配る physics::FlowVolume の枠。
    /// @note コンポーネントではなく System が持つ理由: 場はシーン全体に 1 つで、
    /// @note どの GameObject にも属さない (WaterComponent::volumeHandle は水面 1 枚ごと)。
    physics::VolumeHandle m_flowVolumeHandle;
};

} /// @note namespace fbzz::scene
