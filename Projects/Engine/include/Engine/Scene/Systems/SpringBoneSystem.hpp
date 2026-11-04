/// @file    SpringBoneSystem.hpp
/// @brief   確定済みの FK / IK ポーズへ揺れもの (二次モーション) を上乗せする
/// @author  Hasegawa Jin
/// @date    2026-08-25
#pragma once
#include <Engine/Api.hpp>

#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

/// @note IKSystem の後段で走り、スキニング行列の最終書き込み者になる。
class SpringBoneSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "SpringBoneSystem"; }
    Phase            GetPhase()   const override { return Phase::LateUpdate; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    FBZZ_ENGINE_API ComponentAccess  GetAccess()  const override;
    FBZZ_ENGINE_API OrderingHints    GetOrder()   const override;
    FBZZ_ENGINE_API void             Update(SystemContext& ctx) override;
};

} /// @note namespace fbzz::scene
