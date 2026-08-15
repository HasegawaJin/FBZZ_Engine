// FBZZ Engine
// AudioManager.cpp | fbzz::audio
// BGM / SE の高レベル管理
// WAV をキャッシュし、実再生は IAudioDevice に委譲する。
// ゲーム側はファイルパス・ループ有無・音量だけを扱う。
#include "Engine/Audio/AudioManager.hpp"
#include "Engine/Core/Logger.hpp"
#include <Engine/Util/StringUtils.hpp>
#include <fstream>
#include <cstring>
#include <cassert>
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace fbzz::audio
{

// ---- RIFF/WAV ヘッダ構造体 (リトルエンディアン前提) ----

#pragma pack(push, 1)
struct RiffHeader  { char id[4]; uint32_t size; char type[4]; };
struct ChunkHeader { char id[4]; uint32_t size; };
struct FmtChunk
{
    uint16_t audioFormat;
    uint16_t channels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};
#pragma pack(pop)

// -------------------------------------------------------

AudioManager::AudioManager(IAudioDevice& device)
    : m_device(device)
{}

bool AudioManager::Init()
{
    if (!m_device.Init()) return false;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr))
    {
        FBZZ_LOG_ERROR("AudioManager: MFStartup failed (hr=0x%08X)", hr);
        m_device.Shutdown();
        return false;
    }

    return true;
}

void AudioManager::Shutdown()
{
    StopAllVoices();
    m_cache.clear();
    MFShutdown();
    m_device.Shutdown();
}

void AudioManager::PlayBGM(const std::string& path, bool loop)
{
    if (m_bgmVoiceId != 0) StopBGM();

    const WavBuffer* wav = GetOrLoad(path);
    if (!wav) return;

    m_bgmVoiceId = m_device.PlayBuffer(wav->pcm.data(), wav->pcm.size(), wav->fmt, loop);
    if (m_bgmVoiceId != 0) m_device.SetVolume(m_bgmVoiceId, m_bgmVolume);
}

void AudioManager::StopBGM()
{
    if (m_bgmVoiceId == 0) return;
    m_device.StopBuffer(m_bgmVoiceId);
    m_bgmVoiceId = 0;
}

void AudioManager::PlaySE(const std::string& path)
{
    const WavBuffer* wav = GetOrLoad(path);
    if (!wav) return;

    uint32_t id = m_device.PlayBuffer(wav->pcm.data(), wav->pcm.size(), wav->fmt, false);
    if (id != 0) {
        m_voiceIds.insert(id);
        m_device.SetVolume(id, m_seVolume);
    }
}

uint32_t AudioManager::PlayVoice(const std::string& path, bool loop)
{
    const WavBuffer* wav = GetOrLoad(path);
    if (!wav) return 0;
    const uint32_t voiceId = m_device.PlayBuffer(wav->pcm.data(), wav->pcm.size(), wav->fmt, loop);
    if (voiceId != 0) m_voiceIds.insert(voiceId);
    return voiceId;
}

void AudioManager::StopVoice(uint32_t voiceId)
{
    if (voiceId == 0) return;
    m_device.StopBuffer(voiceId);
    m_voiceIds.erase(voiceId);
}

void AudioManager::StopAllVoices()
{
    StopBGM();
    for (const uint32_t voiceId : m_voiceIds)
        m_device.StopBuffer(voiceId);
    m_voiceIds.clear();
}

void AudioManager::SetVoiceVolume(uint32_t voiceId, float volume)
{
    if (voiceId != 0) m_device.SetVolume(voiceId, volume);
}

void AudioManager::SetVoicePitch(uint32_t voiceId, float pitch)
{
    if (voiceId != 0) m_device.SetPitch(voiceId, pitch);
}

void AudioManager::SetVoicePan(uint32_t voiceId, float pan)
{
    if (voiceId != 0) m_device.SetPan(voiceId, pan);
}

void AudioManager::SetVoiceLowPass(uint32_t voiceId, float normalizedCutoff)
{
    if (voiceId != 0) m_device.SetLowPass(voiceId, normalizedCutoff);
}

bool AudioManager::IsVoicePlaying(uint32_t voiceId)
{
    if (voiceId == 0) return false;
    if (m_device.IsPlaying(voiceId)) return true;
    m_voiceIds.erase(voiceId);
    return false;
}

void AudioManager::SetBGMVolume(float volume)
{
    m_bgmVolume = volume;
    if (m_bgmVoiceId != 0) m_device.SetVolume(m_bgmVoiceId, m_bgmVolume);
}

void AudioManager::SetSEVolume(float volume)
{
    m_seVolume = volume;
}

const AudioManager::WavBuffer* AudioManager::GetOrLoad(const std::string& path)
{
    auto it = m_cache.find(path);
    if (it != m_cache.end()) return &it->second;

    WavBuffer buf;

    const auto dot = path.rfind('.');
    const bool isWav = (dot != std::string::npos) &&
                       (path.substr(dot) == ".wav" || path.substr(dot) == ".WAV");

    bool ok = isWav ? LoadWav(path, buf) : LoadWithMediaFoundation(path, buf);

    if (!ok)
    {
        FBZZ_LOG_ERROR("AudioManager: load failed: %s", path.c_str());
        return nullptr;
    }

    m_cache[path] = std::move(buf);
    return &m_cache[path];
}

bool AudioManager::LoadWav(const std::string& path, WavBuffer& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    RiffHeader riff{};
    file.read(reinterpret_cast<char*>(&riff), sizeof(riff));
    if (std::strncmp(riff.id,   "RIFF", 4) != 0 ||
        std::strncmp(riff.type, "WAVE", 4) != 0)
        return false;

    bool hasFmt  = false;
    bool hasData = false;

    while (file)
    {
        ChunkHeader chunk{};
        file.read(reinterpret_cast<char*>(&chunk), sizeof(chunk));
        if (!file) break;

        if (std::strncmp(chunk.id, "fmt ", 4) == 0)
        {
            FmtChunk fmt{};
            file.read(reinterpret_cast<char*>(&fmt), sizeof(fmt));
            if (chunk.size > sizeof(fmt))
                file.seekg(chunk.size - sizeof(fmt), std::ios::cur);

            if (fmt.audioFormat != 1) return false;  // PCM のみ対応

            out.fmt.sampleRate    = fmt.sampleRate;
            out.fmt.channels      = fmt.channels;
            out.fmt.bitsPerSample = fmt.bitsPerSample;
            hasFmt = true;
        }
        else if (std::strncmp(chunk.id, "data", 4) == 0)
        {
            out.pcm.resize(chunk.size);
            file.read(reinterpret_cast<char*>(out.pcm.data()), chunk.size);
            hasData = true;
        }
        else
        {
            file.seekg(chunk.size, std::ios::cur);
        }
    }

    return hasFmt && hasData;
}

bool AudioManager::LoadWithMediaFoundation(const std::string& path, WavBuffer& out)
{
    // WHY: AudioSource の path は UTF-8 として扱う。日本語フォルダへ移動した配布版でも
    //      Media Foundation が正しい Unicode パスを受け取れるようにする。
    std::wstring wpath = fbzz::util::StringUtils::ToWide(path);

    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, &reader);
    if (FAILED(hr))
    {
        FBZZ_LOG_ERROR("AudioManager: MFCreateSourceReaderFromURL failed: %s", path.c_str());
        return false;
    }

    // デコード出力を PCM に固定
    Microsoft::WRL::ComPtr<IMFMediaType> pcmType;
    MFCreateMediaType(&pcmType);
    pcmType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pcmType->SetGUID(MF_MT_SUBTYPE,    MFAudioFormat_PCM);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pcmType.Get());
    if (FAILED(hr)) return false;

    // 実際の出力フォーマットを取得
    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &outputType);
    if (FAILED(hr)) return false;

    UINT32 sampleRate = 0, channels = 0, bitsPerSample = 0;
    outputType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
    outputType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS,       &channels);
    outputType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,    &bitsPerSample);

    out.fmt.sampleRate    = sampleRate;
    out.fmt.channels      = static_cast<uint16_t>(channels);
    out.fmt.bitsPerSample = static_cast<uint16_t>(bitsPerSample);

    // 全サンプルを読み込み
    while (true)
    {
        DWORD flags = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        hr = reader->ReadSample(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
            nullptr, &flags, nullptr, &sample);

        if (FAILED(hr)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;

        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        sample->ConvertToContiguousBuffer(&buffer);

        BYTE*  data   = nullptr;
        DWORD  length = 0;
        buffer->Lock(&data, nullptr, &length);

        const size_t offset = out.pcm.size();
        out.pcm.resize(offset + length);
        std::memcpy(out.pcm.data() + offset, data, length);

        buffer->Unlock();
    }

    return !out.pcm.empty();
}

} // namespace fbzz::audio
