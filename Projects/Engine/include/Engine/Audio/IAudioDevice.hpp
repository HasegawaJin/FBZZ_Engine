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
    // pitch は再生速度比。1.0 が原音、0.5 が 1 オクターブ下、2.0 が 1 オクターブ上。
    virtual void SetPitch(uint32_t voiceId, float pitch) = 0;
    // pan は -1(左)〜+1(右)。spatialBlend=0 の 2D 音源では 0 を渡す。
    virtual void SetPan(uint32_t voiceId, float pan) = 0;
    // normalizedCutoff=1 は無加工、0 に近いほど高域を減衰する。
    virtual void SetLowPass(uint32_t voiceId, float normalizedCutoff) = 0;
    // 終了済み voice を AudioSystem が検出してランタイム状態を戻すための問い合わせ。
    [[nodiscard]] virtual bool IsPlaying(uint32_t voiceId) = 0;
};

} // namespace fbzz::audio
