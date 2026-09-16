/// @file    RagdollSystem.hpp
/// @brief   XPBD 関節体を進め、最終ボーン階層・スキニング・描画境界を同期する。
/// @author  Hasegawa Jin
/// @date    2026-09-01
#pragma once

#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

/// SpringBoneSystem の後段で走り、スキニング行列の最終書き込み者になる。
class RagdollSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "RagdollSystem"; }
    Phase            GetPhase()   const override { return Phase::LateUpdate; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override;
    OrderingHints    GetOrder()   const override;
    void             Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
