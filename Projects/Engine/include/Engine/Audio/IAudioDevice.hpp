/// @file    IAudioDevice.hpp
/// @brief   XAudio2 などの具体 API 依存を audio モジュールの境界で隠蔽する。
/// @author  Hasegawa Jin
/// @date    2025-01-01
#pragma once
#include "AudioBus.hpp"
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

/// 再生失敗は戻り値や voiceId で表し、例外は使わない。
class IAudioDevice
{
public:
    virtual ~IAudioDevice() = default;

    virtual bool Init()     = 0;
    virtual void Shutdown() = 0;

    /// バスグラフを構築し直す。descs は親が子より前に並んでいること
    /// (NormalizeBusLayout が保証する)。既存の voice はすべて破棄される。
    virtual bool RebuildBuses(const BusDesc* descs, size_t count) = 0;
    virtual void SetBusVolume(BusIndex bus, float volume) = 0;
    virtual void SetBusLowPass(BusIndex bus, float normalizedCutoff) = 0;

    /// BusDesc::reverb が true のバスの残響を更新する。false のバスでは何も起きない。
    /// WHY wet=0 を特別扱いするか: 残響 DSP は鳴っていなくても回り続けるため、
    ///     ゾーンの外にいる間は明示的に止めないと常時 CPU を食う。
    /// @param wet 0-1。0 で残響を止める。
    /// @param decaySeconds 残響が -60dB まで落ちるまでの秒数。
    /// @param highFrequencyRatio 1 で高域も同じだけ残り、0 に近いほど高域が早く減る。
    virtual void SetBusReverb(BusIndex bus, float wet,
                              float decaySeconds, float highFrequencyRatio) = 0;

    /// pcmData は voice が終わるまで呼び出し側が生かしておくこと。
    /// WHY: XAudio2 はサブミットされたバッファをコピーせずポインタで参照し続ける。
    /// @param bus 出力先。範囲外なら Master へ落とす。
    /// @ret 0 は無効ハンドル。loop が true ならループ再生。
    [[nodiscard]] virtual uint32_t PlayBuffer(
        const void* pcmData, size_t bytes,
        const WaveFormat& fmt, bool loop, BusIndex bus) = 0;

    virtual void StopBuffer(uint32_t voiceId)              = 0;
    /// 再生位置を保ったまま止める。ResumeBuffer で続きから鳴る。
    /// WHY StopBuffer と分けるか: あちらは voice を破棄するため位置が残らない。
    virtual void PauseBuffer(uint32_t voiceId)             = 0;
    virtual void ResumeBuffer(uint32_t voiceId)            = 0;
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
