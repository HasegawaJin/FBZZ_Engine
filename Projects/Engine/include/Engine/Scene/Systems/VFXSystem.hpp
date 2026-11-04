/// @file    VFXSystem.hpp
/// @brief   VFXComponent の時刻を進め、配下の VFXElement とエンベロープへ配る Scheduler System。
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

class VFXSystem final : public ISystem {
public:
    std::string_view Name() const override { return "VFXSystem"; }
    Phase GetPhase() const override { return Phase::LateScript; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    FBZZ_ENGINE_API ComponentAccess GetAccess() const override;
    FBZZ_ENGINE_API OrderingHints GetOrder() const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;

private:
    /// @note VFXTimeScale の要求で Time::timeScale を書いた状態か。
    /// @note 関数ローカルの static にしない理由: Play を止めても値が残り、次の Play の 1 フレーム目に
    /// @note «誰も要求していないのに等速へ書き戻す» が起きる。System はシーンと寿命を共にするので、
    /// @note ここに置けば Play ごとに初期化される。
    bool m_ownsTimeScale = false;
};

} /// @note namespace fbzz::scene
