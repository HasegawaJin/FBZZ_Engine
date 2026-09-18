/// @file    FlipbookImageIO.cpp
/// @brief   生成系アセットが共有する PNG と .meta の書き出しの実装。
/// @author  Hasegawa Jin
/// @date    2026-09-11
/// @note DirectXTex の WIC PNG エンコーダーに必要。
#pragma comment(lib, "ole32.lib")

#include "FlipbookImageIO.hpp"

#include <Engine/Asset/TexDescSerializer.hpp>

#include <DirectXTex.h>
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <system_error>

namespace fbzz::asset::detail {

std::filesystem::path FindAvailableBase(const std::filesystem::path& directory,
                                        std::string_view baseName,
                                        std::initializer_list<std::string_view> suffixes)
{
    for (int index = 0; index <= 9999; ++index) {
        std::string candidate(baseName);
        if (index > 0) {
            char number[8]{};
            std::snprintf(number, sizeof(number), "_%03d", index);
            candidate += number;
        }
        const std::filesystem::path base = directory / candidate;
        bool occupied = false;
        for (const std::string_view suffix : suffixes) {
            std::error_code error;
            if (std::filesystem::exists(base.string() + std::string(suffix), error)) {
                occupied = true;
                break;
            }
        }
        if (!occupied) return base;
    }
    return {};
}

bool SavePngRgba8(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                  std::span<const std::uint8_t> pixels, std::string& outError,
                  std::size_t rowPitch)
{
    const std::size_t packedPitch = static_cast<std::size_t>(width) * 4;
    const std::size_t sourcePitch = rowPitch != 0 ? rowPitch : packedPitch;
    if (width == 0 || height == 0 || sourcePitch < packedPitch
        || pixels.size() < sourcePitch * (height - 1) + packedPitch) {
        outError = "PNG の画素数が寸法と一致しません: " + path.string();
        return false;
    }

    DirectX::ScratchImage image;
    HRESULT hr = image.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1);
    if (FAILED(hr)) {
        outError = "PNG 出力バッファを確保できません";
        return false;
    }
    const DirectX::Image* destination = image.GetImage(0, 0, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        std::memcpy(destination->pixels + static_cast<std::size_t>(y) * destination->rowPitch,
                    pixels.data() + static_cast<std::size_t>(y) * sourcePitch, packedPitch);
    }

    std::error_code directoryError;
    if (const std::filesystem::path parent = path.parent_path(); !parent.empty())
        std::filesystem::create_directories(parent, directoryError);
    if (directoryError) {
        outError = "出力ディレクトリを作成できません: " + directoryError.message();
        return false;
    }

    hr = DirectX::SaveToWICFile(*destination, DirectX::WIC_FLAGS_NONE,
                                DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
                                path.wstring().c_str());
    if (FAILED(hr)) {
        outError = "PNG を書き出せません: " + path.string();
        return false;
    }
    return true;
}

bool SaveTextureMeta(const std::filesystem::path& sourcePath, TextureType type,
                     TextureCompression compression, AlphaMode alphaMode,
                     bool mipmaps, std::string& outError)
{
    TextureAsset texture;
    texture.sourcePath = sourcePath.string();
    texture.settings = DefaultSettingsForType(type);
    texture.settings.compression = compression;
    texture.settings.alphaMode = alphaMode;
    texture.settings.mipmaps = mipmaps;
    texture.settings.maxSize = 16384;
    if (!mipmaps) {
        texture.settings.wrapU = TextureWrap::Clamp;
        texture.settings.wrapV = TextureWrap::Clamp;
        texture.settings.filter = TextureFilter::Bilinear;
    }
    if (type == TextureType::Data || type == TextureType::Normal)
        texture.settings.srgb = false;

    const std::string metaPath = sourcePath.string() + ".meta";
    TexDescSerializer serializer;
    if (!serializer.Save(texture, metaPath)) {
        outError = "テクスチャ .meta を書き出せません: " + metaPath;
        return false;
    }
    return true;
}

bool SaveFlipbookDds(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                     std::span<const std::uint8_t> pixels, std::uint32_t tileWidth, std::uint32_t tileHeight,
                     FlipbookMipContent content, FlipbookDdsCompression compression, std::string& outError)
{
    const std::vector<FlipbookMipLevel> levels =
        BuildFlipbookMips(pixels, width, height, tileWidth, tileHeight, content);
    if (levels.empty()) {
        outError = "DDS へ書く画素が足りません: " + path.string();
        return false;
    }
    DirectX::ScratchImage chain;
    if (FAILED(chain.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, levels.size()))) {
        outError = "DDS の出力バッファを確保できません";
        return false;
    }
    for (std::size_t level = 0; level < levels.size(); ++level) {
        const DirectX::Image* image = chain.GetImage(level, 0, 0);
        const FlipbookMipLevel& source = levels[level];
        const std::size_t sourcePitch = static_cast<std::size_t>(source.width) * 4;
        for (std::uint32_t y = 0; y < source.height; ++y)
            std::memcpy(image->pixels + static_cast<std::size_t>(y) * image->rowPitch,
                        source.rgba.data() + static_cast<std::size_t>(y) * sourcePitch, sourcePitch);
    }

    const DirectX::ScratchImage* output = &chain;
    DirectX::ScratchImage compressed;
    if (width % 4 != 0 || height % 4 != 0) compression = FlipbookDdsCompression::None;
    if (compression != FlipbookDdsCompression::None) {
        const DXGI_FORMAT format =
            compression == FlipbookDdsCompression::BC5 ? DXGI_FORMAT_BC5_UNORM : DXGI_FORMAT_BC7_UNORM;
        const DirectX::TEX_COMPRESS_FLAGS flags = compression == FlipbookDdsCompression::BC7
            ? DirectX::TEX_COMPRESS_BC7_QUICK : DirectX::TEX_COMPRESS_DEFAULT;
        if (FAILED(DirectX::Compress(chain.GetImages(), chain.GetImageCount(), chain.GetMetadata(), format, flags,
                                     DirectX::TEX_THRESHOLD_DEFAULT, compressed))) {
            outError = "DDS を圧縮できません: " + path.string();
            return false;
        }
        output = &compressed;
    }
    if (FAILED(DirectX::SaveToDDSFile(output->GetImages(), output->GetImageCount(), output->GetMetadata(),
                                      DirectX::DDS_FLAGS_NONE, path.wstring().c_str()))) {
        outError = "DDS を書き出せません: " + path.string();
        return false;
    }
    return true;
}

bool SaveFlipbookMeta(const std::filesystem::path& sourcePath, TextureType type, TextureCompression compression,
                      AlphaMode alphaMode, std::string& outError)
{
    TextureAsset texture;
    texture.sourcePath = sourcePath.string();
    texture.settings = DefaultSettingsForType(type);
    texture.settings.compression = compression;
    texture.settings.alphaMode = alphaMode;
    texture.settings.mipmaps = true;
    texture.settings.maxSize = 16384;
    texture.settings.wrapU = TextureWrap::Clamp;
    texture.settings.wrapV = TextureWrap::Clamp;
    texture.settings.filter = TextureFilter::Trilinear;
    if (type == TextureType::Data || type == TextureType::Normal)
        texture.settings.srgb = false;

    const std::string metaPath = sourcePath.string() + ".meta";
    TexDescSerializer serializer;
    if (!serializer.Save(texture, metaPath)) {
        outError = "テクスチャ .meta を書き出せません: " + metaPath;
        return false;
    }
    return true;
}

} // namespace fbzz::asset::detail
