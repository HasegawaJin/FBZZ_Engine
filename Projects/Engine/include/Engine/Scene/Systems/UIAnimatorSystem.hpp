/// @file    UIAnimatorSystem.hpp
/// @brief   UIAnimator の Tween を進める System。
/// @author  Hasegawa Jin
/// @date    2026-05-23

/// @note 色・位置・スケールの補間結果を UIImage / UIText / Transform へ書き込む。
/// @note TransformLateUpdate より前に実行し、世界行列再計算へ反映させる。
#pragma once
#include <Engine/Api.hpp>
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class UIAnimatorSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "UIAnimatorSystem"; }
    Phase            GetPhase()   const override { return Phase::LateUpdate; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    FBZZ_ENGINE_API ComponentAccess  GetAccess()  const override;
    FBZZ_ENGINE_API OrderingHints    GetOrder()   const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

} /// @note namespace fbzz::scene
