// FBZZ Engine
// LifetimeSystem.hpp | fbzz::scene
// LifetimeComponent を持つ GO の残り寿命を毎フレーム減算し、
// 0 以下になったら DestroyQueue に積む。
// FlushDestroyQueueSystem が実際の削除を行う。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class LifetimeSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "LifetimeSystem"; }
    Phase            GetPhase()   const override { return Phase::Cleanup; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override;
    void Update(SystemContext& ctx) override;
};

// Scene::FlushDestroyQueue() を ISystem として包む。
// ComponentAccess は Unrestricted — 任意の Component を持つ GO が破棄されうるため。
class FlushDestroyQueueSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "FlushDestroyQueueSystem"; }
    Phase            GetPhase()   const override { return Phase::Cleanup; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override { return ComponentAccess{}.Unrestricted(); }
    OrderingHints    GetOrder()   const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
