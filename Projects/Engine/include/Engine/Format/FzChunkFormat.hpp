/// @file    FzChunkFormat.hpp
/// @brief   チャンク索引付きコンテナ (.fztc 等) のオンディスクレイアウト。
/// @author  Hasegawa Jin
/// @date    2026-09-19
///
/// レイアウト:
/// FzChunkFileHeader
/// FzChunkEntry[chunkCount] (索引)
/// チャンク本体 (索引の offset が指す。各チャンクは他と独立に読める)
#pragma once
#include <cstdint>

namespace fbzz::asset {

/// v1: 無圧縮チャンク + CRC32。storedSize と rawSize を分けてあるのは、圧縮を足しても索引の形を変えないため。
constexpr uint32_t FZCHUNK_VERSION = 1;
/// チャンク本体は無圧縮。
constexpr uint32_t FZCHUNK_FLAG_STORED = 0u;

struct FzChunkFileHeader {
    char     magic[4];     ///< "FZCK"
    uint32_t version;
    uint32_t chunkCount;
    uint32_t contentKind;  ///< 中身の種類 (FourCC)。読み手は自分の種類でなければ使わない
    /// 元データの世代 (大きさ・更新時刻・展開規則の版から作る)。一致しなければキャッシュは古い。
    uint64_t sourceStamp;
    uint32_t _pad[2];
};
static_assert(sizeof(FzChunkFileHeader) == 32, "FzChunkFileHeader size mismatch");

struct FzChunkEntry {
    uint32_t id;
    uint32_t flags;        ///< FZCHUNK_FLAG_*
    uint64_t offset;       ///< ファイル先頭からのバイト位置
    uint64_t storedSize;   ///< ディスク上の大きさ
    uint64_t rawSize;      ///< 展開後の大きさ
    uint32_t checksum;     ///< 本体 (ディスク上のバイト列) の CRC32
    uint32_t _pad;
};
static_assert(sizeof(FzChunkEntry) == 40, "FzChunkEntry size mismatch");

[[nodiscard]] constexpr uint32_t MakeFourCC(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(static_cast<unsigned char>(a))
         | (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8)
         | (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16)
         | (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24);
}

} // namespace fbzz::asset
