/// @file    FlipbookImageIO.cpp
/// @brief   生成系アセットが共有する PNG と .meta の書き出しの実装。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#pragma comment(lib, "ole32.lib") // DirectXTex の WIC PNG エンコーダーに必要

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

} // namespace fbzz::asset::detail
