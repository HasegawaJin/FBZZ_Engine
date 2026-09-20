/// @file    ChunkFile.hpp
/// @brief   チャンク索引付きコンテナの読み書き。要るチャンクだけをディスクから読む。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#pragma once
#include <Engine/Asset/BinaryReader.hpp>
#include <Engine/Format/FzChunkFormat.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fbzz::asset {

/// @brief CRC32 (IEEE 802.3、反転多項式 0xEDB88320)。
/// @see https://www.w3.org/TR/png-3/#5CRC-algorithm PNG «5.5 CRC algorithm»
[[nodiscard]] uint32_t Crc32(std::span<const uint8_t> bytes);

/// @brief チャンクを並べて 1 ファイルへ書く。
class ChunkFileWriter {
public:
    void AddChunk(uint32_t id, std::vector<uint8_t> bytes);

    /// @brief 一時ファイルへ書き切ってから置き換える。読み手が書きかけのファイルを掴まない。
    /// @return 書けなければ false (既存のファイルは残る)。どのスレッドから呼んでもよい。
    [[nodiscard]] bool WriteAtomically(const std::string& path, uint32_t contentKind, uint64_t sourceStamp) const;

private:
    struct Pending {
        uint32_t             id = 0;
        std::vector<uint8_t> bytes;
    };
    std::vector<Pending> m_chunks;
};

/// @brief 索引だけを読んで開き、チャンクは頼まれたものだけを読む。
class ChunkFileReader {
public:
    /// @return 開けない・形式が違う・索引が壊れていれば false。
    [[nodiscard]] bool Open(const std::string& path);

    [[nodiscard]] const FzChunkFileHeader& Header() const { return m_header; }
    [[nodiscard]] const FzChunkEntry* Find(uint32_t id) const;

    /// @brief 1 チャンクを読んで CRC32 を照合する。
    /// @return 無い・読めない・照合が合わなければ false (out は未定義)。
    [[nodiscard]] bool ReadChunk(uint32_t id, std::vector<uint8_t>& out);

    /// @brief ここまでにディスクから読んだバイト数 (索引を含む)。
    [[nodiscard]] uint64_t BytesRead() const { return m_reader.bytesRead; }

private:
    BinaryReader              m_reader;
    FzChunkFileHeader         m_header{};
    std::vector<FzChunkEntry> m_entries;
};

} // namespace fbzz::asset
