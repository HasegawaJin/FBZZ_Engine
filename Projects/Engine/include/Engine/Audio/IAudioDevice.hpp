// FBZZ Engine
// IAudioDevice.hpp | fbzz::audio
// オーディオバックエンドの抽象インターフェース
// XAudio2 などの具体 API 依存を audio モジュールの境界で隠蔽する。
// 再生失敗は戻り値や voiceId で表し、例外は使わない。
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

    // 0 は無効ハンドル。loop が true ならループ再生
    [[nodiscard]] virtual uint32_t PlayBuffer(
        const void* pcmData, size_t bytes,
        const WaveFormat& fmt, bool loop) = 0;

    virtual void StopBuffer(uint32_t voiceId)              = 0;
    virtual void SetVolume(uint32_t voiceId, float volume) = 0;
};

} // namespace fbzz::audio
