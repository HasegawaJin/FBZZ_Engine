/// @file    XAudio2Device.hpp
/// @brief   XAudio2 を使った IAudioDevice 実装。
/// @author  Hasegawa Jin
/// @date    2025-01-01
#pragma once
#include "IAudioDevice.hpp"
#include <xaudio2.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fbzz::audio
{

/// COM / XAudio2 の寿命管理をこのクラスに閉じ込め、上位は IAudioDevice だけを見る。
/// 終了済み voice は PurgeFinishedVoices で回収する。
class XAudio2Device : public IAudioDevice
{
public:
    bool Init()     override;
    void Shutdown() override;

    bool RebuildBuses(const BusDesc* descs, size_t count) override;
    void SetBusVolume(BusIndex bus, float volume) override;
    void SetBusLowPass(BusIndex bus, float normalizedCutoff) override;
    void SetBusReverb(BusIndex bus, float wet,
                      float decaySeconds, float highFrequencyRatio) override;

    [[nodiscard]] uint32_t PlayBuffer(
        const void* pcmData, size_t bytes,
        const WaveFormat& fmt, bool loop, BusIndex bus) override;

    void StopBuffer(uint32_t voiceId)             override;
    void PauseBuffer(uint32_t voiceId)            override;
    void ResumeBuffer(uint32_t voiceId)           override;
    void SetVolume(uint32_t voiceId, float volume) override;
    void SetPitch(uint32_t voiceId, float pitch) override;
    void SetPan(uint32_t voiceId, float pan) override;
    void SetLowPass(uint32_t voiceId, float normalizedCutoff) override;
    [[nodiscard]] bool IsPlaying(uint32_t voiceId) override;

private:
    /// 鳴り終わった voice を «畳まずに» プールへ戻すための鍵。
    ///
    /// WHY 送り先まで鍵に入れるか: 送り先 (バス) は CreateSourceVoice の引数で、
    ///     後から変えるには SetOutputVoices が要る。あれも DestroyVoice と同じく
    ///     オーディオ処理の合間を待つ呼び出しなので、«同じバス宛» だけを再利用する。
    struct VoiceKey
    {
        uint16_t       channels      = 0;
        uint32_t       sampleRate    = 0;
        uint16_t       bitsPerSample = 0;
        IXAudio2Voice* destination   = nullptr;

        bool operator==(const VoiceKey& rhs) const
        {
            return channels == rhs.channels && sampleRate == rhs.sampleRate
                && bitsPerSample == rhs.bitsPerSample && destination == rhs.destination;
        }
    };
    struct VoiceKeyHash
    {
        std::size_t operator()(const VoiceKey& key) const noexcept
        {
            std::size_t hash = key.sampleRate;
            hash = hash * 131u + key.channels;
            hash = hash * 131u + key.bitsPerSample;
            hash = hash * 131u + static_cast<std::size_t>(
                reinterpret_cast<std::uintptr_t>(key.destination) >> 4);
            return hash;
        }
    };

    struct VoiceEntry
    {
        IXAudio2SourceVoice* voice    = nullptr;
        bool                 loop     = false;
        uint16_t             channels = 0;
        // パンの出力行列はこの voice の実際の送り先に対して設定する。
        // WHY: バス導入後もマスターへ行列を書くと、送り先が違うため何も起きない。
        IXAudio2Voice*       destination = nullptr;
        uint32_t             destinationChannels = 0;
        /// 鳴り終わったときにどのプールへ返すか。
        VoiceKey             key{};
    };

    /// 鳴り終わった voice をプールへ戻す (溢れていれば畳む)。
    void RecycleVoice(const VoiceEntry& entry);
    /// 同じ形式・同じ送り先の voice をプールから取る。無ければ nullptr。
    [[nodiscard]] IXAudio2SourceVoice* TakePooledVoice(const VoiceKey& key);

    void PurgeFinishedVoices();
    void DestroyAllSourceVoices();
    void DestroyBuses();
    [[nodiscard]] IXAudio2Voice* BusVoice(BusIndex bus) const;

    Microsoft::WRL::ComPtr<IXAudio2> m_xaudio2;
    IXAudio2MasteringVoice*          m_masterVoice = nullptr;
    uint32_t                         m_masterChannels   = 0;
    uint32_t                         m_masterSampleRate = 0;
    // CoInitializeEx成功時だけCoUninitializeを対にし、初期化途中の失敗でも安全に後始末する。
    bool                             m_comInitialized = false;

    /// 1 本のバス。effect スロット 0 が残響で、持たないバスでは hasReverb が false。
    struct BusEntry
    {
        IXAudio2SubmixVoice* voice         = nullptr;
        bool                 hasReverb     = false;
        bool                 reverbEnabled = false;
    };

    // 添字は BusIndex。先頭が Master。
    std::vector<BusEntry> m_buses;

    uint32_t                                 m_nextId = 1;
    std::unordered_map<uint32_t, VoiceEntry> m_voices;

    // 鳴り終わった voice の置き場。形式と送り先が同じものだけを積む。
    // 1 鍵あたりの上限は «同じ音が同時に何本鳴るか» で足りる。
    static constexpr std::size_t kMaxPooledPerKey = 16;
    std::unordered_map<VoiceKey, std::vector<IXAudio2SourceVoice*>, VoiceKeyHash> m_voicePool;
};

} // namespace fbzz::audio
