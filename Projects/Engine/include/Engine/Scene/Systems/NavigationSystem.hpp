/// @file    NavigationSystem.hpp
/// @brief   NavMeshAgentComponent を NavMesh 上で A* + Funnel Algorithm によりパス追従させる毎フレームシステム。
/// @author  Hasegawa Jin
/// @date    2026-06-17
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"
#include <Engine/Scene/Entity.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::scene {
struct NavMeshSurfaceComponent;

class NavigationSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "NavigationSystem"; }
    Phase            GetPhase()   const override { return Phase::Navigation; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override;
    OrderingHints    GetOrder()   const override;
    void Update(SystemContext& ctx) override;

private:
    struct SurfaceEntry {
        int agentTypeId = 0;
        NavMeshSurfaceComponent* surface = nullptr;
    };
    [[nodiscard]] SurfaceEntry& FindSurfaceSlot(int agentTypeId);
    struct AvoidanceEntry {
        EntityID entity;
        size_t next = SIZE_MAX;
    };
    struct AvoidanceBucket {
        int x = 0;
        int z = 0;
        size_t first = SIZE_MAX;
        size_t last = SIZE_MAX;
    };
    [[nodiscard]] AvoidanceBucket& FindAvoidanceBucket(int x, int z);
    /// @note Scheduler の非再入 Update ごとに値を作り直し、callback を跨ぐ Entity 一覧の容量だけを保持する。
    std::vector<EntityID> m_agentEntities;
    std::vector<SurfaceEntry> m_surfaces;
    std::vector<AvoidanceEntry> m_avoidanceEntries;
    std::vector<AvoidanceBucket> m_avoidanceBuckets;
};

} /// @note namespace fbzz::scene
