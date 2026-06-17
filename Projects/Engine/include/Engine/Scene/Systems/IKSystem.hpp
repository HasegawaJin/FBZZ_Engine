// FBZZ Engine
// IKSystem.hpp | fbzz::scene
// AnimatorSystem が確定した FK ポーズに解析的 2-Bone IK を後処理として適用し、
// スキニング行列を再アップロードする。
// 地面スナップレイキャストは ctx.world に委譲する。
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
    // ctx.resources が nullptr の場合はスキップ（FK ポーズ後に IK 補正）
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
