/// @file    AudioSystem.hpp
/// @brief   AudioSourceComponent から再生命令を発行する Scene System。
/// @author  Hasegawa Jin
/// @date    2026-05-21

/// @note Scene のデータを走査し、audio::AudioManager へ高レベル操作を渡す。
/// @note デバイス API 依存は audio モジュール内に閉じ込める。
/// @note ctx.audioManager が null の場合はスキップ（SetAudioManager 未設定時）。
#pragma once
#include <Engine/Api.hpp>
#include "Engine/Core/Scheduler/ISystem.hpp"
#include <Engine/Scene/Entity.hpp>
#include <vector>

namespace fbzz::scene {

class AudioSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "AudioSystem"; }
    Phase            GetPhase()   const override { return Phase::LateScript; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    FBZZ_ENGINE_API ComponentAccess  GetAccess()  const override;
    FBZZ_ENGINE_API OrderingHints    GetOrder()   const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
private:
    /// @note Scene から消えた音源の voice を次の Update で退役させる。生存 voice だけを保持する。
    struct OwnedVoice { EntityID owner; uint32_t voiceId; bool oneShot; };
    std::vector<OwnedVoice> m_ownedVoices;
    uint64_t m_sceneGeneration = 0;
};

} /// @note namespace fbzz::scene
