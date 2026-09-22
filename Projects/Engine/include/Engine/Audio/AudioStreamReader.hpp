/// @file    AudioStreamReader.hpp
/// @brief   音声を有界の PCM ブロックへ順次デコードする。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include "IAudioDevice.hpp"
#include <memory>
#include <string>
#include <vector>

namespace fbzz::audio {
/// @brief COM と Media Foundation が初期化された単一スレッドで使用する。
class AudioStreamReader {
public:
    static constexpr size_t BLOCK_BYTES = 65536;
    AudioStreamReader();
    ~AudioStreamReader();
    bool Open(const std::string& path);
    /// @return 失敗時 false。成功かつ空なら終端。出力は最大 BLOCK_BYTES。
    bool Read(std::vector<uint8_t>& pcm);
    bool Rewind();
    [[nodiscard]] const WaveFormat& Format() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
