/// @file    IKSystem.hpp
/// @brief   AnimatorSystem が確定した FK ポーズに順序付き IK Solver を後処理として適用する。
/// @author  Hasegawa Jin
/// @date    2026-05-30
///
/// TwoBone と FootPlace を同一システムで解き、地面判定は ctx.world に委譲する。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class IKSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "IKSystem"; }
    Phase            GetPhase()   const override { return Phase::LateUpdate; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override;
    OrderingHints    GetOrder()   const override;
    /// ctx.resources が nullptr の場合はスキップ（FK ポーズ後に IK 補正）
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
