// FBZZ Engine
// AudioSystem.hpp | fbzz::audio
// BGM / SE の高レベル管理と WAV キャッシュ
// IAudioDevice に実再生を委譲し、ゲーム側はパスと音量だけを扱う。
// 読み込み済みバッファを再利用し、Shutdown で音声リソースを解放する。
#pragma once
#include "IAudioDevice.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace fbzz::audio
{

class AudioSystem
{
public:
    // device の所有権は呼び出し元が保持。Init より前に渡す
    explicit AudioSystem(IAudioDevice& device);

    bool Init();
    void Shutdown();

    void PlayBGM(const std::string& path, bool loop = true);
    void StopBGM();

    void PlaySE(const std::string& path);

    void SetBGMVolume(float volume);  // 0.0f ~ 1.0f
    void SetSEVolume(float volume);

private:
    struct WavBuffer
    {
        WaveFormat          fmt;
        std::vector<uint8_t> pcm;
    };

    const WavBuffer* GetOrLoad(const std::string& path);
    bool LoadWav(const std::string& path, WavBuffer& out);
    bool LoadWithMediaFoundation(const std::string& path, WavBuffer& out);

    IAudioDevice& m_device;

    uint32_t m_bgmVoiceId = 0;
    float    m_bgmVolume  = 1.0f;
    float    m_seVolume   = 1.0f;

    std::unordered_map<std::string, WavBuffer> m_cache;
};

} // namespace fbzz::audio
