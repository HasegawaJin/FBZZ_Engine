/// @file    TextureStreamCache.cpp
/// @brief   テクスチャの品質段キャッシュ (.fztc)。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#include <Engine/Asset/TextureStreamCache.hpp>
#include <Engine/Asset/ChunkFile.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace fbzz::asset::texturecache {

namespace {

/// @brief 付帯情報チャンクの中身。
struct CacheMeta {
    uint32_t rulesVersion = kDecodeRulesVersion;
    uint32_t sourceMipCount = 0;
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    uint32_t chunkCount = 0;
    uint32_t _pad = 0;
};
static_assert(sizeof(CacheMeta) == 24, "CacheMeta size mismatch");

/// @brief FNV-1a 64bit。
/// @see http://www.isthe.com/chongo/tech/comp/fnv/index.html «FNV-1a»
uint64_t Fnv1a(const void* bytes, std::size_t size, uint64_t hash = 0xcbf29ce484222325ull)
{
    const auto* p = static_cast<const uint8_t*>(bytes);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

std::vector<uint8_t> EncodeMip(const renderer::DecodedTextureRGBA8::Mip& mip)
{
    std::vector<uint8_t> bytes(8 + mip.rgba.size());
    std::memcpy(bytes.data(), &mip.width, 4);
    std::memcpy(bytes.data() + 4, &mip.height, 4);
    if (!mip.rgba.empty()) std::memcpy(bytes.data() + 8, mip.rgba.data(), mip.rgba.size());
    return bytes;
}

bool DecodeMip(const std::vector<uint8_t>& bytes, renderer::DecodedTextureRGBA8::Mip& mip)
{
    if (bytes.size() < 8) return false;
    std::memcpy(&mip.width, bytes.data(), 4);
    std::memcpy(&mip.height, bytes.data() + 4, 4);
    const std::size_t expected = static_cast<std::size_t>(mip.width) * mip.height * 4u;
    if (mip.width == 0 || mip.height == 0 || bytes.size() != 8 + expected) return false;
    mip.rgba.assign(bytes.begin() + 8, bytes.end());
    return true;
}

} // namespace

uint64_t MakeSourceStamp(uint64_t sourceSize, int64_t sourceWriteTime)
{
    uint64_t hash = Fnv1a(&sourceSize, sizeof(sourceSize));
    hash = Fnv1a(&sourceWriteTime, sizeof(sourceWriteTime), hash);
    return Fnv1a(&kDecodeRulesVersion, sizeof(kDecodeRulesVersion), hash);
}

std::string CacheFileName(const std::string& sourcePath)
{
    std::string normalized = sourcePath;
    for (char& c : normalized)
        c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    char name[32]{};
    std::snprintf(name, sizeof(name), "%016llx.fztc",
                  static_cast<unsigned long long>(Fnv1a(normalized.data(), normalized.size())));
    return name;
}

renderer::DecodedTextureRGBA8 SelectQuality(const renderer::DecodedTextureRGBA8& full, AssetQuality quality)
{
    renderer::DecodedTextureRGBA8 result;
    result.sourceWidth = full.sourceWidth;
    result.sourceHeight = full.sourceHeight;
    if (full.mips.empty()) return result;
    if (full.mips.size() > 1) {
        const std::size_t first = (std::min)(static_cast<std::size_t>(quality), full.mips.size() - 1);
        result.mips.assign(full.mips.begin() + static_cast<std::ptrdiff_t>(first), full.mips.end());
        return result;
    }
    renderer::DecodedTextureRGBA8::Mip level = full.mips[0];
    for (AssetQuality i = 0; i < quality && (level.width > 1 || level.height > 1); ++i)
        level = renderer::DownsampleRGBA8Box2x(level);
    result.mips.push_back(std::move(level));
    return result;
}

bool Write(const std::string& cachePath, uint64_t sourceStamp,
           const renderer::DecodedTextureRGBA8& full, AssetQuality lowest)
{
    if (full.mips.empty()) return false;
    ChunkFileWriter writer;
    CacheMeta meta;
    meta.sourceMipCount = static_cast<uint32_t>(full.mips.size());
    meta.sourceWidth = full.sourceWidth;
    meta.sourceHeight = full.sourceHeight;
    if (full.mips.size() > 1) {
        /// @note ミップ付き: チャンク i = ミップ i。品質段 q は q 番以降を読む。
        meta.chunkCount = static_cast<uint32_t>(full.mips.size());
        for (std::size_t i = 0; i < full.mips.size(); ++i)
            writer.AddChunk(static_cast<uint32_t>(i), EncodeMip(full.mips[i]));
    } else {
        /// @note 1 段: チャンク q = q 回縮小した 1 段。品質段 q はそのチャンクだけを読む。
        meta.chunkCount = static_cast<uint32_t>(lowest) + 1u;
        renderer::DecodedTextureRGBA8::Mip level = full.mips[0];
        for (uint32_t q = 0; q < meta.chunkCount; ++q) {
            writer.AddChunk(q, EncodeMip(level));
            if (level.width > 1 || level.height > 1) level = renderer::DownsampleRGBA8Box2x(level);
        }
    }
    std::vector<uint8_t> metaBytes(sizeof(CacheMeta));
    std::memcpy(metaBytes.data(), &meta, sizeof(CacheMeta));
    writer.AddChunk(kMetaChunkId, std::move(metaBytes));
    return writer.WriteAtomically(cachePath, kContentKind, sourceStamp);
}

bool ReadQuality(const std::string& cachePath, uint64_t sourceStamp, AssetQuality quality,
                 renderer::DecodedTextureRGBA8& out, uint64_t* outBytesRead)
{
    ChunkFileReader reader;
    const auto finish = [&](bool ok) {
        if (outBytesRead) *outBytesRead = reader.BytesRead();
        return ok;
    };
    if (!reader.Open(cachePath)) return finish(false);
    if (reader.Header().contentKind != kContentKind || reader.Header().sourceStamp != sourceStamp)
        return finish(false);

    std::vector<uint8_t> bytes;
    CacheMeta meta;
    if (!reader.ReadChunk(kMetaChunkId, bytes) || bytes.size() != sizeof(CacheMeta)) return finish(false);
    std::memcpy(&meta, bytes.data(), sizeof(CacheMeta));
    if (meta.rulesVersion != kDecodeRulesVersion || meta.chunkCount == 0) return finish(false);

    out.mips.clear();
    out.sourceWidth = meta.sourceWidth;
    out.sourceHeight = meta.sourceHeight;
    uint32_t first = 0;
    uint32_t last = 0;
    if (meta.sourceMipCount > 1) {
        first = (std::min)(static_cast<uint32_t>(quality), meta.chunkCount - 1);
        last = meta.chunkCount - 1;
    } else {
        /// @note キャッシュが持つ段より低い品質を求められたら、そのときは元画像から作り直させる。
        if (quality >= meta.chunkCount) return finish(false);
        first = last = quality;
    }
    for (uint32_t id = first; id <= last; ++id) {
        renderer::DecodedTextureRGBA8::Mip mip;
        if (!reader.ReadChunk(id, bytes) || !DecodeMip(bytes, mip)) {
            out.mips.clear();
            return finish(false);
        }
        out.mips.push_back(std::move(mip));
    }
    return finish(true);
}

} // namespace fbzz::asset::texturecache
