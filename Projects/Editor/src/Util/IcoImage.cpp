/// @file    IcoImage.cpp
/// @brief   .ico の ICONDIR 解析と PNG / DIB フレームの RGBA8 展開。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#include <Editor/Util/IcoImage.hpp>

#include <Engine/Util/FileSystem.hpp>

#include <stb_image.h>

#include <algorithm>
#include <cstring>

namespace fbzz::editor {

namespace {

#pragma pack(push, 1)
struct IconDir {
    std::uint16_t reserved;
    std::uint16_t type;
    std::uint16_t count;
};
struct IconDirEntry {
    std::uint8_t  width;   ///< 0 は 256 を意味する
    std::uint8_t  height;  ///< 0 は 256 を意味する
    std::uint8_t  colorCount;
    std::uint8_t  reserved;
    std::uint16_t planes;
    std::uint16_t bitCount;
    std::uint32_t bytesInRes;
    std::uint32_t imageOffset;
};
struct DibHeader {
    std::uint32_t size;
    std::int32_t  width;
    std::int32_t  height; ///< XOR と AND を積むため、画像の 2 倍が入る
    std::uint16_t planes;
    std::uint16_t bitCount;
    std::uint32_t compression;
    std::uint32_t imageSize;
    std::int32_t  xPelsPerMeter;
    std::int32_t  yPelsPerMeter;
    std::uint32_t colorsUsed;
    std::uint32_t colorsImportant;
};
#pragma pack(pop)

constexpr std::uint8_t kPngSignature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

bool LooksLikePng(const std::uint8_t* bytes, std::size_t size)
{
    return size >= sizeof(kPngSignature) &&
           std::memcmp(bytes, kPngSignature, sizeof(kPngSignature)) == 0;
}

bool DecodePngFrame(const std::uint8_t* bytes, std::size_t size, IcoImage& out)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes, static_cast<int>(size),
                                            &width, &height, &channels, 4);
    if (!pixels) return false;
    out.width  = width;
    out.height = height;
    out.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return true;
}

/// 4 バイト境界へ切り上げた 1 行のバイト数。DIB の行はこの単位で詰まっている。
std::size_t DibStride(std::int32_t width, std::uint16_t bitCount)
{
    return ((static_cast<std::size_t>(width) * bitCount + 31u) / 32u) * 4u;
}

/// パレット参照 (1 / 4 / 8bpp) のインデックスを取り出す。
std::uint32_t PaletteIndexAt(const std::uint8_t* row, std::int32_t x, std::uint16_t bitCount)
{
    switch (bitCount) {
        case 8: return row[x];
        case 4: return (x & 1) ? (row[x / 2] & 0x0Fu) : (row[x / 2] >> 4);
        case 1: return (row[x / 8] >> (7 - (x & 7))) & 0x01u;
        default: return 0;
    }
}

bool DecodeDibFrame(const std::uint8_t* bytes, std::size_t size, IcoImage& out, std::string& outError)
{
    if (size < sizeof(DibHeader)) {
        outError = "truncated icon bitmap header";
        return false;
    }
    DibHeader header{};
    std::memcpy(&header, bytes, sizeof(header));
    if (header.compression != 0) {
        outError = "compressed icon bitmaps are not supported";
        return false;
    }
    const std::int32_t width  = header.width;
    /// @note AND マスクぶんを落とす
    const std::int32_t height = header.height / 2;
    if (width <= 0 || height <= 0) {
        outError = "invalid icon bitmap size";
        return false;
    }
    if (header.bitCount != 32 && header.bitCount != 24 &&
        header.bitCount != 8  && header.bitCount != 4 && header.bitCount != 1) {
        outError = "unsupported icon bit depth";
        return false;
    }

    const bool indexed = header.bitCount <= 8;
    const std::uint32_t paletteEntries = indexed
        ? (header.colorsUsed != 0 ? header.colorsUsed : (1u << header.bitCount))
        : 0u;
    const std::size_t paletteBytes = static_cast<std::size_t>(paletteEntries) * 4u;
    const std::size_t pixelOffset  = header.size + paletteBytes;

    const std::size_t xorStride = DibStride(width, header.bitCount);
    const std::size_t andStride = DibStride(width, 1);
    if (pixelOffset + xorStride * height > size) {
        outError = "truncated icon pixel data";
        return false;
    }
    const std::uint8_t* palette = indexed ? bytes + header.size : nullptr;
    const std::uint8_t* xorData = bytes + pixelOffset;
    /// @note AND マスクは欠けている .ico もある。無ければ «全画素不透明» とみなす。
    const std::size_t andOffset = pixelOffset + xorStride * height;
    const bool hasAndMask = andOffset + andStride * height <= size;
    const std::uint8_t* andData = hasAndMask ? bytes + andOffset : nullptr;

    out.width  = width;
    out.height = height;
    out.rgba.assign(static_cast<std::size_t>(width) * height * 4u, 0u);

    /// @note 32bpp でもアルファを一切書かない .ico がある。その場合だけ AND マスクへ倒す。
    bool hasAlphaChannel = false;
    if (header.bitCount == 32) {
        for (std::int32_t y = 0; y < height && !hasAlphaChannel; ++y) {
            const std::uint8_t* row = xorData + xorStride * y;
            for (std::int32_t x = 0; x < width; ++x) {
                if (row[x * 4 + 3] != 0) { hasAlphaChannel = true; break; }
            }
        }
    }

    for (std::int32_t y = 0; y < height; ++y) {
        const std::uint8_t* xorRow = xorData + xorStride * static_cast<std::size_t>(height - 1 - y);
        const std::uint8_t* andRow = andData
            ? andData + andStride * static_cast<std::size_t>(height - 1 - y)
            : nullptr;
        std::uint8_t* dst = out.rgba.data() + static_cast<std::size_t>(y) * width * 4u;

        for (std::int32_t x = 0; x < width; ++x) {
            std::uint8_t b = 0, g = 0, r = 0, a = 255;
            if (header.bitCount == 32) {
                b = xorRow[x * 4 + 0];
                g = xorRow[x * 4 + 1];
                r = xorRow[x * 4 + 2];
                a = hasAlphaChannel ? xorRow[x * 4 + 3] : 255;
            } else if (header.bitCount == 24) {
                b = xorRow[x * 3 + 0];
                g = xorRow[x * 3 + 1];
                r = xorRow[x * 3 + 2];
            } else {
                const std::uint32_t index = PaletteIndexAt(xorRow, x, header.bitCount);
                if (index < paletteEntries) {
                    b = palette[index * 4 + 0];
                    g = palette[index * 4 + 1];
                    r = palette[index * 4 + 2];
                }
            }
            /// @note AND マスクは «1 = 背景を透かす»。32bpp でアルファを持つ絵には掛けない。
            if (andRow && !hasAlphaChannel && ((andRow[x / 8] >> (7 - (x & 7))) & 0x01u))
                a = 0;

            dst[x * 4 + 0] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = a;
        }
    }
    return true;
}

} // namespace

bool DecodeIcoBytes(const std::uint8_t* bytes, std::size_t size, IcoImage& out, std::string& outError)
{
    out = {};
    if (!bytes || size < sizeof(IconDir)) {
        outError = "not an .ico file";
        return false;
    }

    IconDir dir{};
    std::memcpy(&dir, bytes, sizeof(dir));
    if (dir.reserved != 0 || dir.type != 1 || dir.count == 0) {
        outError = "not an .ico file";
        return false;
    }
    out.frameCount = dir.count;

    const IconDirEntry* best = nullptr;
    std::uint32_t bestScore = 0;
    std::vector<IconDirEntry> entries;
    entries.reserve(dir.count);
    for (std::uint16_t i = 0; i < dir.count; ++i) {
        const std::size_t entryOffset = sizeof(IconDir) + static_cast<std::size_t>(i) * sizeof(IconDirEntry);
        if (entryOffset + sizeof(IconDirEntry) > size) {
            outError = "truncated .ico directory";
            return false;
        }
        IconDirEntry entry{};
        std::memcpy(&entry, bytes + entryOffset, sizeof(entry));
        if (static_cast<std::size_t>(entry.imageOffset) + entry.bytesInRes > size) continue;
        entries.push_back(entry);
    }
    if (entries.empty()) {
        outError = "no readable frame in .ico";
        return false;
    }

    /// @note 面積が同じなら色深度が深い方を採る。16 色版と 32bit 版が同居する .ico で
    ///       «小さくもないのに色が潰れている» サムネイルを出さないため。
    for (const IconDirEntry& entry : entries) {
        const std::uint32_t w = entry.width  == 0 ? 256u : entry.width;
        const std::uint32_t h = entry.height == 0 ? 256u : entry.height;
        const std::uint32_t score = w * h * 64u + entry.bitCount;
        if (!best || score > bestScore) {
            best = &entry;
            bestScore = score;
        }
    }

    const std::uint8_t* frame = bytes + best->imageOffset;
    const std::size_t frameSize = best->bytesInRes;
    if (LooksLikePng(frame, frameSize)) {
        if (!DecodePngFrame(frame, frameSize, out)) {
            outError = "corrupt PNG frame in .ico";
            return false;
        }
        return true;
    }
    if (!DecodeDibFrame(frame, frameSize, out, outError)) return false;
    return out.IsValid();
}

bool DecodeIcoFile(const std::filesystem::path& path, IcoImage& out, std::string& outError)
{
    std::vector<std::uint8_t> fileBytes;
    if (!util::FileSystem::ReadBinary(path, fileBytes) || fileBytes.empty()) {
        outError = "cannot read " + util::FileSystem::PathToUtf8(path);
        return false;
    }
    return DecodeIcoBytes(fileBytes.data(), fileBytes.size(), out, outError);
}

} // namespace fbzz::editor
