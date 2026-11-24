/// @file    IKSystem.hpp
/// @brief   AnimatorSystem が確定した FK ポーズに順序付き IK Solver を後処理として適用する。
/// @author  Hasegawa Jin
/// @date    2026-05-30

/// @note TwoBone と FootPlace を同一システムで解き、地面判定は ctx.world に委譲する。
#pragma once
#include <Engine/Api.hpp>
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class IKSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "IKSystem"; }
    Phase            GetPhase()   const override { return Phase::LateUpdate; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    FBZZ_ENGINE_API ComponentAccess  GetAccess()  const override;
    FBZZ_ENGINE_API OrderingHints    GetOrder()   const override;
    /// @note ctx.resources が nullptr の場合はスキップ（FK ポーズ後に IK 補正）
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

} /// @note namespace fbzz::scene
