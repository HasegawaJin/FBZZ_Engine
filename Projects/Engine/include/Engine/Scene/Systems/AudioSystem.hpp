/// @file    AudioSystem.hpp
/// @brief   AudioSourceComponent から再生命令を発行する Scene System。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// Scene のデータを走査し、audio::AudioManager へ高レベル操作を渡す。
/// デバイス API 依存は audio モジュール内に閉じ込める。
/// ctx.audioManager が null の場合はスキップ（SetAudioManager 未設定時）。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class AudioSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "AudioSystem"; }
    Phase            GetPhase()   const override { return Phase::LateScript; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override;
    OrderingHints    GetOrder()   const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
