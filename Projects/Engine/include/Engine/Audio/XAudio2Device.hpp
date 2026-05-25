// FBZZ Engine
// XAudio2Device.hpp | fbzz::audio
// XAudio2 を使った IAudioDevice 実装
// COM / XAudio2 の寿命管理をこのクラスに閉じ込め、上位は IAudioDevice だけを見る。
// 終了済み voice は PurgeFinishedVoices で回収する。
#pragma once
#include "IAudioDevice.hpp"
#include <xaudio2.h>
#include <wrl/client.h>
#include <unordered_map>
#include <vector>

namespace fbzz::audio
{

class XAudio2Device : public IAudioDevice
{
public:
    bool Init()     override;
    void Shutdown() override;

    [[nodiscard]] uint32_t PlayBuffer(
        const void* pcmData, size_t bytes,
        const WaveFormat& fmt, bool loop) override;

    void StopBuffer(uint32_t voiceId)             override;
    void SetVolume(uint32_t voiceId, float volume) override;

private:
    struct VoiceEntry
    {
        IXAudio2SourceVoice* voice = nullptr;
        bool                 loop  = false;
    };

    void PurgeFinishedVoices();

    Microsoft::WRL::ComPtr<IXAudio2> m_xaudio2;
    IXAudio2MasteringVoice*          m_masterVoice = nullptr;

    uint32_t                              m_nextId = 1;
    std::unordered_map<uint32_t, VoiceEntry> m_voices;
};

} // namespace fbzz::audio
