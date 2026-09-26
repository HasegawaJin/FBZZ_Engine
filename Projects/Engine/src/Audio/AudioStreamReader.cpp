/// @file    AudioStreamReader.cpp
/// @brief   Media Foundation のサンプルを固定上限の PCM ブロックへ分割する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <Engine/Audio/AudioStreamReader.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <algorithm>
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

namespace fbzz::audio {
struct AudioStreamReader::Impl {
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    WaveFormat format{};
    std::vector<uint8_t> pending;
    size_t offset = 0;
    bool ended = false;
};
AudioStreamReader::AudioStreamReader() : m_impl(std::make_unique<Impl>()) {}
AudioStreamReader::~AudioStreamReader() = default;
const WaveFormat& AudioStreamReader::Format() const { return m_impl->format; }

bool AudioStreamReader::Open(const std::string& path)
{
    m_impl = std::make_unique<Impl>();
    auto& state = *m_impl;
    const auto wide = util::StringUtils::ToWide(path);
    if (FAILED(MFCreateSourceReaderFromURL(wide.c_str(), nullptr, &state.reader))) return false;
    if (FAILED(state.reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE))
        || FAILED(state.reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE))) return false;
    Microsoft::WRL::ComPtr<IMFMediaType> type;
    if (FAILED(MFCreateMediaType(&type))
        || FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio))
        || FAILED(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM))
        || FAILED(state.reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type.Get()))) return false;
    type.Reset();
    if (FAILED(state.reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &type))) return false;
    UINT32 channels = 0, bits = 0, rate = 0;
    if (FAILED(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels))
        || FAILED(type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits))
        || FAILED(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate))) return false;
    if (channels == 0 || channels > 8 || rate == 0 || (bits != 8 && bits != 16 && bits != 24 && bits != 32)) return false;
    state.format = {rate, static_cast<uint16_t>(channels), static_cast<uint16_t>(bits)};
    return true;
}

bool AudioStreamReader::Read(std::vector<uint8_t>& pcm)
{
    pcm.clear();
    auto& state = *m_impl;
    if (!state.reader || state.format.channels == 0) return false;
    const size_t alignment = state.format.channels * (state.format.bitsPerSample / 8);
    const size_t capacity = BLOCK_BYTES / alignment * alignment;
    unsigned emptySamples = 0;
    while (pcm.size() < capacity) {
        if (state.offset < state.pending.size()) {
            const size_t count = (std::min)(capacity - pcm.size(), state.pending.size() - state.offset);
            pcm.insert(pcm.end(), state.pending.data() + state.offset, state.pending.data() + state.offset + count);
            state.offset += count;
            continue;
        }
        if (state.ended) break;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        DWORD flags = 0;
        /// @see https://learn.microsoft.com/en-us/windows/win32/medfound/processing-media-data-with-the-source-reader ReadSample と終端フラグ。
        if (FAILED(state.reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample))) return false;
        if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)) return false;
        state.ended = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
        if (!sample) {
            if (++emptySamples > 1024) return false;
            continue;
        }
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) return false;
        /// @note デコーダーの異常な単一サンプルで無制限確保しない。通常の音声パケットより十分大きい上限。
        const bool valid = length <= 1024u * 1024u && length % alignment == 0;
        if (valid) state.pending.assign(data, data + length);
        buffer->Unlock();
        if (!valid || (length == 0 && ++emptySamples > 1024)) return false;
        state.offset = 0;
    }
    return true;
}

bool AudioStreamReader::Rewind()
{
    auto& state = *m_impl;
    if (!state.reader) return false;
    PROPVARIANT position{};
    position.vt = VT_I8;
    position.hVal.QuadPart = 0;
    if (FAILED(state.reader->SetCurrentPosition(GUID_NULL, position))) return false;
    state.pending.clear();
    state.offset = 0;
    state.ended = false;
    return true;
}
}
