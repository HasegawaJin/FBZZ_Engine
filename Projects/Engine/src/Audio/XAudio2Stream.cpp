/// @file    XAudio2Stream.cpp
/// @brief   音声スレッドが 3 個の PCM バッファを循環供給する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include "XAudio2Stream.hpp"
#include <Engine/Core/Logger.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace fbzz::audio {
XAudio2Stream::~XAudio2Stream()
{
    m_stop.store(true);
    if (m_wake) SetEvent(m_wake);
    if (m_thread.joinable()) m_thread.join();
    /// @note DestroyVoice はコールバック完了を待つ。PCM とイベントの解放はその後。
    if (m_voice) m_voice->DestroyVoice();
    if (m_wake) CloseHandle(m_wake);
    if (m_ready) CloseHandle(m_ready);
}

bool XAudio2Stream::Start(IXAudio2& device, IXAudio2Voice* destination, const std::string& path, bool loop)
{
    m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_wake || !m_ready) return false;
    m_thread = std::thread([this, &device, destination, path, loop] { Run(device, destination, path, loop); });
    /// @note 開始時は形式と最初のブロックのみ待つ。残りの I/O はゲームフレームから独立する。
    WaitForSingleObject(m_ready, INFINITE);
    return m_started;
}

void XAudio2Stream::OnBufferEnd(void*) { SetEvent(m_wake); }
void XAudio2Stream::OnVoiceError(void*, HRESULT)
{
    m_failed.store(true);
    m_stop.store(true);
    SetEvent(m_wake);
}

void XAudio2Stream::Run(IXAudio2& device, IXAudio2Voice* destination, const std::string& path, bool loop)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) { SetEvent(m_ready); return; }
    {
        AudioStreamReader reader;
        const auto pump = [&]() {
            if (!reader.Open(path) || !reader.Read(m_buffers[0]) || m_buffers[0].empty()) return;
            m_format = reader.Format();
            WAVEFORMATEX format{};
            format.wFormatTag = WAVE_FORMAT_PCM;
            format.nChannels = m_format.channels;
            format.nSamplesPerSec = m_format.sampleRate;
            format.wBitsPerSample = m_format.bitsPerSample;
            format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
            format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
            XAUDIO2_SEND_DESCRIPTOR descriptor{0, destination};
            XAUDIO2_VOICE_SENDS sends{1, &descriptor};
            if (FAILED(device.CreateSourceVoice(&m_voice, &format, XAUDIO2_VOICE_USEFILTER,
                       XAUDIO2_MAX_FREQ_RATIO, this, &sends))) return;
            size_t index = 0;
            while (!m_stop.load()) {
                XAUDIO2_VOICE_STATE state{};
                m_voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                if (state.BuffersQueued >= 3) {
                    WaitForSingleObject(m_wake, INFINITE);
                    continue;
                }
                auto& pcm = m_buffers[index];
                if (m_started) {
                    if (!reader.Read(pcm)) { FBZZ_LOG_ERROR("Audio stream decode failed: %s", path.c_str()); break; }
                    if (pcm.empty()) {
                        if (!loop) break;
                        if (!reader.Rewind() || !reader.Read(pcm) || pcm.empty()) break;
                    }
                }
                XAUDIO2_BUFFER buffer{};
                buffer.AudioBytes = static_cast<UINT32>(pcm.size());
                buffer.pAudioData = pcm.data();
                /// @see https://learn.microsoft.com/en-us/windows/win32/xaudio2/how-to--stream-a-sound-from-disk 消費完了後だけ循環バッファを再利用する。
                if (FAILED(m_voice->SubmitSourceBuffer(&buffer))) break;
                if (!m_started) {
                    if (FAILED(m_voice->Start())) break;
                    m_started = true;
                    SetEvent(m_ready);
                }
                index = (index + 1) % 3;
            }
        };
        pump();
        m_finished.store(true);
        if (!m_started) {
            FBZZ_LOG_ERROR("Audio stream could not start: %s", path.c_str());
            SetEvent(m_ready);
        }
    }
    CoUninitialize();
}
}
