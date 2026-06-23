// FBZZ Engine
// UIAnimatorSystem.hpp | fbzz::scene
// UIAnimator の Tween を進める System
// 色・位置・スケールの補間結果を UIImage / UIText / Transform へ書き込む。
// TransformLateUpdate より前に実行し、世界行列再計算へ反映させる。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class UIAnimatorSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "UIAnimatorSystem"; }
    Phase            GetPhase()   const override { return Phase::LateUpdate; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override;
    OrderingHints    GetOrder()   const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
