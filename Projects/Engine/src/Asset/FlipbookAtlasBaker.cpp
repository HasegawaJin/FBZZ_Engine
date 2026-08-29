/// @file    FlipbookAtlasBaker.cpp
/// @brief   PNG等の画像列をRGBA8 Flipbook Atlasへ結合する実装。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma comment(lib, "ole32.lib")

#include <Engine/Asset/FlipbookAtlasBaker.hpp>

#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Asset/TextureAsset.hpp>

#include <DirectXTex.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <utility>

namespace fbzz::asset {
namespace {

constexpr std::uint32_t kMaximumAtlasDimension = 16384;

FlipbookAtlasBakeResult Fail(std::string message)
{
    FlipbookAtlasBakeResult result;
    result.message = std::move(message);
    return result;
}

bool HasExtension(const std::filesystem::path& path, const wchar_t* extension)
{
    std::wstring value = path.extension().wstring();
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return value == extension;
}

bool LoadFrame(const std::filesystem::path& path, DirectX::ScratchImage& rgba,
               std::string& outError)
{
    DirectX::ScratchImage loaded;
    const std::wstring widePath = path.wstring();
    HRESULT hr = E_FAIL;
    if (HasExtension(path, L".dds"))
        hr = DirectX::LoadFromDDSFile(widePath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, loaded);
    else if (HasExtension(path, L".tga"))
        hr = DirectX::LoadFromTGAFile(widePath.c_str(), nullptr, loaded);
    else
        hr = DirectX::LoadFromWICFile(widePath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, loaded);
    if (FAILED(hr) || loaded.GetImageCount() == 0) {
        outError = "画像を読み込めません: " + path.string();
        return false;
    }

    const DirectX::Image* source = loaded.GetImage(0, 0, 0);
    hr = DirectX::Convert(*source, DXGI_FORMAT_R8G8B8A8_UNORM,
                          DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, rgba);
    if (FAILED(hr)) {
        outError = "RGBA8へ変換できません: " + path.string();
        return false;
    }
    return true;
}

bool SaveTextureMeta(const std::filesystem::path& outputPath)
{
    TextureAsset texture;
    texture.sourcePath = outputPath.string();
    texture.settings = DefaultSettingsForType(TextureType::Color);
    // WHY: AtlasのMip生成は隣接フレームを混ぜて境界を汚すため、明示的に無効化する。
    texture.settings.mipmaps = false;
    texture.settings.maxSize = static_cast<int>(kMaximumAtlasDimension);
    texture.settings.wrapU = TextureWrap::Clamp;
    texture.settings.wrapV = TextureWrap::Clamp;
    texture.settings.filter = TextureFilter::Bilinear;
    TexDescSerializer serializer;
    return serializer.Save(texture, outputPath.string() + ".meta");
}

} // namespace

FlipbookAtlasBakeResult BakeFlipbookAtlas(const FlipbookAtlasBakeSettings& settings)
{
    if (settings.framePaths.empty()) return Fail("入力フレームがありません");
    if (settings.outputPath.empty()) return Fail("Atlasの出力先がありません");

    const std::filesystem::path outputPath(settings.outputPath);
    if (!HasExtension(outputPath, L".png"))
        return Fail("Atlasの出力形式はPNGにしてください: " + settings.outputPath);

    std::error_code fileError;
    if (std::filesystem::exists(outputPath, fileError) && !settings.overwrite)
        return Fail("出力先が既に存在します: " + settings.outputPath);
    if (fileError) return Fail("出力先を確認できません: " + fileError.message());

    const int frameCount = static_cast<int>((std::min)(
        settings.framePaths.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    const int columns = settings.columns > 0
        ? (std::min)(settings.columns, frameCount)
        : static_cast<int>(std::ceil(std::sqrt(static_cast<double>(frameCount))));
    const int rows = (frameCount + columns - 1) / columns;

    DirectX::ScratchImage firstFrame;
    std::string loadError;
    if (!LoadFrame(settings.framePaths.front(), firstFrame, loadError)) return Fail(loadError);
    const DirectX::Image* firstImage = firstFrame.GetImage(0, 0, 0);
    if (firstImage == nullptr || firstImage->width == 0 || firstImage->height == 0)
        return Fail("先頭フレームの寸法が無効です");
    const std::size_t frameWidth = firstImage->width;
    const std::size_t frameHeight = firstImage->height;

    if (frameWidth > kMaximumAtlasDimension / static_cast<std::uint32_t>(columns)
        || frameHeight > kMaximumAtlasDimension / static_cast<std::uint32_t>(rows)) {
        return Fail("Atlasが最大寸法16384pxを超えます: "
                    + std::to_string(frameWidth * static_cast<std::size_t>(columns))
                    + "x" + std::to_string(frameHeight * static_cast<std::size_t>(rows)));
    }

    const std::size_t atlasWidth = frameWidth * static_cast<std::size_t>(columns);
    const std::size_t atlasHeight = frameHeight * static_cast<std::size_t>(rows);
    DirectX::ScratchImage atlas;
    HRESULT hr = atlas.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM,
                                    atlasWidth, atlasHeight, 1, 1);
    if (FAILED(hr)) return Fail("Atlasの出力バッファを確保できません");

    const DirectX::Image* atlasImage = atlas.GetImage(0, 0, 0);
    std::memset(atlasImage->pixels, 0, atlasImage->slicePitch);
    for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        DirectX::ScratchImage current;
        const DirectX::Image* source = firstImage;
        if (frameIndex > 0) {
            if (!LoadFrame(settings.framePaths[static_cast<std::size_t>(frameIndex)],
                           current, loadError))
                return Fail(loadError);
            source = current.GetImage(0, 0, 0);
        }
        if (source == nullptr || source->width != frameWidth
            || source->height != frameHeight) {
            return Fail("フレーム寸法が先頭と一致しません: "
                        + settings.framePaths[static_cast<std::size_t>(frameIndex)]);
        }

        const std::size_t originX =
            static_cast<std::size_t>(frameIndex % columns) * source->width;
        const std::size_t originY =
            static_cast<std::size_t>(frameIndex / columns) * source->height;
        const std::size_t copyBytes = source->width * 4;
        for (std::size_t y = 0; y < source->height; ++y) {
            std::memcpy(atlasImage->pixels + (originY + y) * atlasImage->rowPitch + originX * 4,
                        source->pixels + y * source->rowPitch, copyBytes);
        }
    }

    const std::filesystem::path parent = outputPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, fileError);
        if (fileError) return Fail("出力ディレクトリを作成できません: " + fileError.message());
    }

    hr = DirectX::SaveToWICFile(*atlasImage, DirectX::WIC_FLAGS_NONE,
                                DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
                                outputPath.wstring().c_str());
    if (FAILED(hr)) return Fail("Atlasを書き出せません: " + outputPath.string());
    if (settings.generateTextureMeta && !SaveTextureMeta(outputPath))
        return Fail("Atlasは生成しましたが.metaを書き出せません: " + outputPath.string() + ".meta");

    FlipbookAtlasBakeResult result;
    result.success = true;
    result.outputPath = outputPath.string();
    result.frameCount = frameCount;
    result.columns = columns;
    result.rows = rows;
    result.frameWidth = static_cast<std::uint32_t>(frameWidth);
    result.frameHeight = static_cast<std::uint32_t>(frameHeight);
    result.atlasWidth = static_cast<std::uint32_t>(atlasWidth);
    result.atlasHeight = static_cast<std::uint32_t>(atlasHeight);
    result.message = "Atlas生成: " + std::to_string(frameCount) + " frames / "
        + std::to_string(columns) + "x" + std::to_string(rows) + " / "
        + std::to_string(atlasWidth) + "x" + std::to_string(atlasHeight);
    return result;
}

} // namespace fbzz::asset
