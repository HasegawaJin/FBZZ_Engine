// FBZZ Engine
// IAudioDevice.hpp | fbzz::audio
// オーディオバックエンドの抽象インターフェース
#pragma once
#include <cstddef>
#include <cstdint>

namespace fbzz::audio
{

struct WaveFormat
{
    uint32_t sampleRate    = 0;
    uint16_t channels      = 0;
    uint16_t bitsPerSample = 0;
};

class IAudioDevice
{
public:
    virtual ~IAudioDevice() = default;

    virtual bool Init()     = 0;
    virtual void Shutdown() = 0;

    // 0 は無効ハンドル。loop=true でループ再生
    [[nodiscard]] virtual uint32_t PlayBuffer(
        const void* pcmData, size_t bytes,
        const WaveFormat& fmt, bool loop) = 0;

    virtual void StopBuffer(uint32_t voiceId)              = 0;
    virtual void SetVolume(uint32_t voiceId, float volume) = 0;
};

} // namespace fbzz::audio
