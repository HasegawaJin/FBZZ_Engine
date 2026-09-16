/// @file    VFXSystem.hpp
/// @brief   VFXComponent の時刻を進め、配下の VFXElement とエンベロープへ配る Scheduler System。
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

class VFXSystem final : public ISystem {
public:
    std::string_view Name() const override { return "VFXSystem"; }
    Phase GetPhase() const override { return Phase::LateScript; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    ComponentAccess GetAccess() const override;
    OrderingHints GetOrder() const override;
    void Update(SystemContext& ctx) override;

private:
    /// VFXTimeScale の要求で Time::timeScale を書いた状態か。
    ///
    /// WHY 関数ローカルの static にしないか: Play を止めても値が残り、次の Play の
    ///     1 フレーム目に «誰も要求していないのに等速へ書き戻す» が起きる。
    ///     System はシーンと寿命を共にするので、ここに置けば Play ごとに初期化される。
    bool m_ownsTimeScale = false;
};

} // namespace fbzz::scene
