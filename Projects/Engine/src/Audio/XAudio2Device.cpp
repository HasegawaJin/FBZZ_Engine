// FBZZ Engine
// XAudio2Device.cpp | fbzz::audio
// XAudio2 バックエンド実装
#include "Engine/Audio/XAudio2Device.hpp"
#include "Engine/Core/Logger.hpp"
#include <cassert>

#pragma comment(lib, "xaudio2.lib")

namespace fbzz::audio
{

bool XAudio2Device::Init()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // S_FALSE はすでに初期化済みなので問題なし
    if (FAILED(hr) && hr != S_FALSE)
    {
        FBZZ_LOG_ERROR("XAudio2Device: CoInitializeEx failed");
        return false;
    }

    UINT32 flags = 0;
#if defined(_DEBUG)
    flags |= XAUDIO2_DEBUG_ENGINE;
#endif
    hr = XAudio2Create(m_xaudio2.GetAddressOf(), flags);
    if (FAILED(hr))
    {
        FBZZ_LOG_ERROR("XAudio2Device: XAudio2Create failed");
        return false;
    }

    hr = m_xaudio2->CreateMasteringVoice(&m_masterVoice);
    if (FAILED(hr))
    {
        FBZZ_LOG_ERROR("XAudio2Device: CreateMasteringVoice failed");
        return false;
    }

    return true;
}

void XAudio2Device::Shutdown()
{
    for (auto& [id, entry] : m_voices)
    {
        if (entry.voice)
        {
            entry.voice->Stop();
            entry.voice->DestroyVoice();
        }
    }
    m_voices.clear();

    if (m_masterVoice)
    {
        m_masterVoice->DestroyVoice();
        m_masterVoice = nullptr;
    }

    m_xaudio2.Reset();
    CoUninitialize();
}

uint32_t XAudio2Device::PlayBuffer(
    const void* pcmData, size_t bytes,
    const WaveFormat& fmt, bool loop)
{
    assert(pcmData && bytes > 0);
    PurgeFinishedVoices();

    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = fmt.channels;
    wfx.nSamplesPerSec  = fmt.sampleRate;
    wfx.wBitsPerSample  = fmt.bitsPerSample;
    wfx.nBlockAlign     = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    IXAudio2SourceVoice* voice = nullptr;
    HRESULT hr = m_xaudio2->CreateSourceVoice(&voice, &wfx);
    if (FAILED(hr)) return 0;

    XAUDIO2_BUFFER buf{};
    buf.AudioBytes = static_cast<UINT32>(bytes);
    buf.pAudioData = static_cast<const BYTE*>(pcmData);
    buf.Flags      = XAUDIO2_END_OF_STREAM;
    if (loop) buf.LoopCount = XAUDIO2_LOOP_INFINITE;

    hr = voice->SubmitSourceBuffer(&buf);
    if (FAILED(hr))
    {
        voice->DestroyVoice();
        return 0;
    }

    voice->Start();

    uint32_t id = m_nextId++;
    m_voices[id] = VoiceEntry{ voice, loop };
    return id;
}

void XAudio2Device::StopBuffer(uint32_t voiceId)
{
    auto it = m_voices.find(voiceId);
    if (it == m_voices.end()) return;

    it->second.voice->Stop();
    it->second.voice->DestroyVoice();
    m_voices.erase(it);
}

void XAudio2Device::SetVolume(uint32_t voiceId, float volume)
{
    auto it = m_voices.find(voiceId);
    if (it == m_voices.end()) return;
    it->second.voice->SetVolume(volume);
}

void XAudio2Device::PurgeFinishedVoices()
{
    for (auto it = m_voices.begin(); it != m_voices.end();)
    {
        if (it->second.loop) { ++it; continue; }

        XAUDIO2_VOICE_STATE state{};
        it->second.voice->GetState(&state);
        if (state.BuffersQueued == 0)
        {
            it->second.voice->DestroyVoice();
            it = m_voices.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

} // namespace fbzz::audio
