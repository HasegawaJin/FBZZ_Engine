// FBZZ Engine
// XAudio2Device.cpp | fbzz::audio
// XAudio2 バックエンド実装
// COM / XAudio2 の初期化と voice の寿命管理を閉じ込める。
// 上位は IAudioDevice 経由で操作し、XAudio2 型へ依存しない。
#include "Engine/Audio/XAudio2Device.hpp"
#include "Engine/Core/Logger.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>

#pragma comment(lib, "xaudio2.lib")

namespace fbzz::audio
{

bool XAudio2Device::Init()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE)
    {
        // WHY: Window が OLE ドラッグ&ドロップ (IDropTarget) のためにこのスレッドを先に STA で
        //      初期化していると、MTA 要求は RPC_E_CHANGED_MODE を返す。XAudio2 は STA でも動作するため、
        //      この COM 初期化を「所有しない」形でそのまま続行する (対の CoUninitialize は呼ばない)。
        m_comInitialized = false;
    }
    else if (FAILED(hr) && hr != S_FALSE)
    {
        // S_FALSE はすでに初期化済みなので問題なし。それ以外の失敗のみエラー扱いにする。
        FBZZ_LOG_ERROR("XAudio2Device: CoInitializeEx failed");
        return false;
    }
    else
    {
        // S_OK / S_FALSE: このスレッドの COM 参照を保持し、Shutdown で対に CoUninitialize する。
        m_comInitialized = true;
    }

    UINT32 flags = 0;
#if defined(_DEBUG)
    flags |= XAUDIO2_DEBUG_ENGINE;
#endif
    hr = XAudio2Create(m_xaudio2.GetAddressOf(), flags);
    if (FAILED(hr))
    {
        FBZZ_LOG_ERROR("XAudio2Device: XAudio2Create failed");
        Shutdown();
        return false;
    }

    hr = m_xaudio2->CreateMasteringVoice(&m_masterVoice);
    if (FAILED(hr))
    {
        FBZZ_LOG_ERROR("XAudio2Device: CreateMasteringVoice failed");
        Shutdown();
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
    if (m_comInitialized)
    {
        CoUninitialize();
        m_comInitialized = false;
    }
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
    HRESULT hr = m_xaudio2->CreateSourceVoice(
        &voice, &wfx, 0, XAUDIO2_MAX_FREQ_RATIO);
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
    m_voices[id] = VoiceEntry{ voice, loop, fmt.channels };
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

void XAudio2Device::SetPitch(uint32_t voiceId, float pitch)
{
    auto it = m_voices.find(voiceId);
    if (it == m_voices.end()) return;

    // WHY: XAudio2 の許容範囲外は HRESULT 失敗になるため、公開 API 境界で制限する。
    pitch = (std::max)(XAUDIO2_MIN_FREQ_RATIO, (std::min)(pitch, XAUDIO2_MAX_FREQ_RATIO));
    it->second.voice->SetFrequencyRatio(pitch);
}

void XAudio2Device::SetPan(uint32_t voiceId, float pan)
{
    auto it = m_voices.find(voiceId);
    if (it == m_voices.end() || it->second.channels != 1 || !m_masterVoice) return;

    XAUDIO2_VOICE_DETAILS masterDetails{};
    m_masterVoice->GetVoiceDetails(&masterDetails);
    if (masterDetails.InputChannels < 2) return;

    // WHAT: モノラル音源を等電力パンで左右へ配分し、音像移動時の音量落ちを抑える。
    pan = (std::max)(-1.0f, (std::min)(pan, 1.0f));
    const float left  = std::sqrt(0.5f * (1.0f - pan));
    const float right = std::sqrt(0.5f * (1.0f + pan));
    std::vector<float> matrix(masterDetails.InputChannels, 0.0f);
    matrix[0] = left;
    matrix[1] = right;
    it->second.voice->SetOutputMatrix(
        m_masterVoice, 1, masterDetails.InputChannels, matrix.data());
}

bool XAudio2Device::IsPlaying(uint32_t voiceId)
{
    auto it = m_voices.find(voiceId);
    if (it == m_voices.end()) return false;

    XAUDIO2_VOICE_STATE state{};
    it->second.voice->GetState(&state);
    if (state.BuffersQueued != 0) return true;

    it->second.voice->DestroyVoice();
    m_voices.erase(it);
    return false;
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
