/// @file    XAudio2Stream.hpp
/// @brief   XAudio2 の消費完了に合わせてデコードするストリーミング音声。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Engine/Audio/AudioStreamReader.hpp>
#include <xaudio2.h>
#include <atomic>
#include <thread>

namespace fbzz::audio {
/// @brief voice を破棄するまで PCM とコールバックを保持する。
class XAudio2Stream final : public IXAudio2VoiceCallback {
public:
    ~XAudio2Stream();
    bool Start(IXAudio2& device, IXAudio2Voice* destination, const std::string& path, bool loop);
    [[nodiscard]] IXAudio2SourceVoice* Voice() const { return m_voice; }
    [[nodiscard]] const WaveFormat& Format() const { return m_format; }
    [[nodiscard]] bool Finished() const { return m_finished.load(); }
    [[nodiscard]] bool Failed() const { return m_failed.load(); }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void*) override;
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override;
private:
    void Run(IXAudio2& device, IXAudio2Voice* destination, const std::string& path, bool loop);
    IXAudio2SourceVoice* m_voice = nullptr;
    WaveFormat m_format{};
    void* m_wake = nullptr;
    void* m_ready = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_finished{false};
    std::atomic<bool> m_failed{false};
    bool m_started = false;
    std::vector<uint8_t> m_buffers[3];
};
}
