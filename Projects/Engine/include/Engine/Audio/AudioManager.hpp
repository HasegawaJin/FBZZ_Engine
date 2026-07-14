// FBZZ Engine
// AudioManager.hpp | fbzz::audio
// BGM / SE の高レベル管理と WAV キャッシュ
// IAudioDevice に実再生を委譲し、ゲーム側はパスと音量だけを扱う。
// 読み込み済みバッファを再利用し、Shutdown で音声リソースを解放する。
#pragma once
#include "IAudioDevice.hpp"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdint>

namespace fbzz::audio
{

class AudioManager
{
public:
    // device の所有権は呼び出し元が保持。Init より前に渡す
    explicit AudioManager(IAudioDevice& device);

    bool Init();
    void Shutdown();

    void PlayBGM(const std::string& path, bool loop = true);
    void StopBGM();

    void PlaySE(const std::string& path);

    // Scene の AudioSourceComponent が所有する voice を個別に操作する低レベル API。
    // WHY: BGM 1 本の共有状態では複数の 3D 音源を同時追跡できないため、voiceId を呼び出し側へ返す。
    [[nodiscard]] uint32_t PlayVoice(const std::string& path, bool loop = false);
    void StopVoice(uint32_t voiceId);
    // EditorのPlay終了など、Scene Audioを一括停止するライフサイクル境界で使用する。
    void StopAllVoices();
    void SetVoiceVolume(uint32_t voiceId, float volume);
    void SetVoicePitch(uint32_t voiceId, float pitch);
    void SetVoicePan(uint32_t voiceId, float pan);
    [[nodiscard]] bool IsVoicePlaying(uint32_t voiceId);

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
    // Scene/SE Voiceを追跡し、AudioSystemが停止フェーズを走れない場合も確実に破棄する。
    std::unordered_set<uint32_t> m_voiceIds;
};

} // namespace fbzz::audio
