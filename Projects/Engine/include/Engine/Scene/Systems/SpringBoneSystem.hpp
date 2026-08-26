/// @file SpringBoneSystem.hpp
/// @brief 確定済みの FK / IK ポーズへ揺れもの (二次モーション) を上乗せする
/// @author Hasegawa Jin
/// @date 2026-08-25
#pragma once

#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

/// IKSystem の後段で走り、スキニング行列の最終書き込み者になる。
class SpringBoneSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "SpringBoneSystem"; }
    Phase            GetPhase()   const override { return Phase::LateUpdate; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override;
    OrderingHints    GetOrder()   const override;
    void             Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
