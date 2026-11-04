/// @file    LifetimeSystem.hpp
/// @brief   LifetimeComponent を持つ GO の残り寿命を毎フレーム減算し、。
/// @author  Hasegawa Jin
/// @date    2026-06-10

/// @note 0 以下になったら DestroyQueue に積む。
/// @note FlushDestroyQueueSystem が実際の削除を行う。
#pragma once
#include <Engine/Api.hpp>
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class LifetimeSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "LifetimeSystem"; }
    Phase            GetPhase()   const override { return Phase::Cleanup; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    FBZZ_ENGINE_API ComponentAccess  GetAccess()  const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

/// @note Scene::FlushDestroyQueue() を ISystem として包む。
/// @note ComponentAccess は Unrestricted — 任意の Component を持つ GO が破棄されうるため。
class FlushDestroyQueueSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "FlushDestroyQueueSystem"; }
    Phase            GetPhase()   const override { return Phase::Cleanup; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override { return ComponentAccess{}.Unrestricted(); }
    FBZZ_ENGINE_API OrderingHints    GetOrder()   const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

} /// @note namespace fbzz::scene
