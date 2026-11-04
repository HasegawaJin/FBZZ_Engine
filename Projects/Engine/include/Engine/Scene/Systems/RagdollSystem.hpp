/// @file    RagdollSystem.hpp
/// @brief   XPBD 関節体を進め、最終ボーン階層・スキニング・描画境界を同期する。
/// @author  Hasegawa Jin
/// @date    2026-09-01
#pragma once
#include <Engine/Api.hpp>

#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

/// @note SpringBoneSystem の後段で走り、スキニング行列の最終書き込み者になる。
class RagdollSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "RagdollSystem"; }
    Phase            GetPhase()   const override { return Phase::LateUpdate; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    FBZZ_ENGINE_API ComponentAccess  GetAccess()  const override;
    FBZZ_ENGINE_API OrderingHints    GetOrder()   const override;
    FBZZ_ENGINE_API void             Update(SystemContext& ctx) override;
};

} /// @note namespace fbzz::scene
