/// @file    AssetBrowserThumbnail.cpp
/// @brief   CPU image decoding with fixed allocation and thumbnail bounds.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include <Editor/Panels/AssetBrowserThumbnail.hpp>
#include <Editor/Util/IcoImage.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <DirectXTex.h>
#include <stb_image.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>

namespace fbzz::editor::assetthumbnail {
namespace {

bool SafeDimensions(size_t width, size_t height)
{
    return width > 0 && height > 0 && width <= MAX_PIXELS && height <= MAX_PIXELS / width;
}

Image Resize(const uint8_t* pixels, uint32_t width, uint32_t height, size_t rowPitch)
{
    Image out;
    const float scale = std::min(1.0f, static_cast<float>(MAX_EDGE) / std::max(width, height));
    out.width = std::max(1u, static_cast<uint32_t>(std::round(width * scale)));
    out.height = std::max(1u, static_cast<uint32_t>(std::round(height * scale)));
    out.sourceWidth = width;
    out.sourceHeight = height;
    out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4);
    /// @note Area averaging in premultiplied alpha keeps transparent sprites free of dark fringes.
    for (uint32_t y = 0; y < out.height; ++y) {
        const uint32_t y0 = y * height / out.height;
        const uint32_t y1 = std::max(y0 + 1, (y + 1) * height / out.height);
        for (uint32_t x = 0; x < out.width; ++x) {
            const uint32_t x0 = x * width / out.width;
            const uint32_t x1 = std::max(x0 + 1, (x + 1) * width / out.width);
            uint64_t sums[4]{};
            for (uint32_t sy = y0; sy < y1; ++sy) {
                for (uint32_t sx = x0; sx < x1; ++sx) {
                    const uint8_t* p = pixels + sy * rowPitch + sx * 4;
                    for (int c = 0; c < 3; ++c) sums[c] += p[c] * p[3];
                    sums[3] += p[3];
                }
            }
            uint8_t* target = out.rgba.data() + (static_cast<size_t>(y) * out.width + x) * 4;
            for (int c = 0; c < 3; ++c) target[c] = sums[3] ? static_cast<uint8_t>(sums[c] / sums[3]) : 0;
            target[3] = static_cast<uint8_t>(sums[3] / ((x1 - x0) * (y1 - y0)));
        }
    }
    out.state = State::READY;
    return out;
}

} /// @note namespace

Image Decode(const std::filesystem::path& path)
{
    Image out;
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    if (error || bytes == 0) return out;
    if (bytes > MAX_FILE_BYTES) { out.state = State::UNSUPPORTED; return out; }
    std::string extension = util::FileSystem::PathToUtf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".dds") {
        DirectX::TexMetadata metadata;
        if (FAILED(DirectX::GetMetadataFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, metadata))) return out;
        if (metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || metadata.arraySize != 1 ||
            metadata.IsCubemap() || !SafeDimensions(metadata.width, metadata.height)) {
            out.state = State::UNSUPPORTED;
            return out;
        }
        DirectX::ScratchImage image, converted;
        if (FAILED(DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image))) return out;
        const DirectX::Image* top = image.GetImage(0, 0, 0);
        if (!top) return out;
        if (DirectX::IsCompressed(top->format)) {
            if (FAILED(DirectX::Decompress(*top, DXGI_FORMAT_R8G8B8A8_UNORM, converted))) return out;
        } else if (top->format != DXGI_FORMAT_R8G8B8A8_UNORM) {
            if (FAILED(DirectX::Convert(*top, DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT,
                                       DirectX::TEX_THRESHOLD_DEFAULT, converted))) return out;
        }
        if (converted.GetImageCount()) top = converted.GetImage(0, 0, 0);
        return Resize(top->pixels, static_cast<uint32_t>(top->width), static_cast<uint32_t>(top->height), top->rowPitch);
    }
    if (extension == ".ico") {
        std::ifstream input(path, std::ios::binary);
        std::vector<uint8_t> encoded(static_cast<size_t>(bytes));
        if (!input.read(static_cast<char*>(static_cast<void*>(encoded.data())), static_cast<std::streamsize>(encoded.size())) || encoded.size() < 6) return out;
        uint16_t count = 0;
        std::memcpy(&count, encoded.data() + 4, sizeof(count));
        if (count == 0 || count > 256 || encoded.size() < 6ull + count * 16ull) return out;
        for (uint16_t index = 0; index < count; ++index) {
            uint32_t frameSize = 0, offset = 0;
            std::memcpy(&frameSize, encoded.data() + 6 + index * 16 + 8, sizeof(frameSize));
            std::memcpy(&offset, encoded.data() + 6 + index * 16 + 12, sizeof(offset));
            if (offset > encoded.size() || frameSize > encoded.size() - offset || frameSize < 12) return out;
            int width = 0, height = 0, channels = 0;
            if (!stbi_info_from_memory(encoded.data() + offset, static_cast<int>(frameSize), &width, &height, &channels)) {
                std::memcpy(&width, encoded.data() + offset + 4, sizeof(width));
                std::memcpy(&height, encoded.data() + offset + 8, sizeof(height));
                height /= 2;
            }
            if (width <= 0 || height <= 0 || !SafeDimensions(width, height)) {
                out.state = State::UNSUPPORTED;
                return out;
            }
        }
        IcoImage image;
        std::string message;
        if (!DecodeIcoBytes(encoded.data(), encoded.size(), image, message) || !image.IsValid()) return out;
        if (!SafeDimensions(image.width, image.height)) { out.state = State::UNSUPPORTED; return out; }
        return Resize(image.rgba.data(), image.width, image.height, static_cast<size_t>(image.width) * 4);
    }
    if (extension != ".png" && extension != ".jpg" && extension != ".jpeg" &&
        extension != ".bmp" && extension != ".tga") { out.state = State::UNSUPPORTED; return out; }
    std::ifstream input(path, std::ios::binary);
    std::vector<uint8_t> encoded(static_cast<size_t>(bytes));
    if (!input.read(static_cast<char*>(static_cast<void*>(encoded.data())), static_cast<std::streamsize>(encoded.size()))) return out;
    int width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels)) return out;
    if (width <= 0 || height <= 0 || !SafeDimensions(width, height)) { out.state = State::UNSUPPORTED; return out; }
    uint8_t* pixels = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels, 4);
    if (!pixels) return out;
    out = Resize(pixels, width, height, static_cast<size_t>(width) * 4);
    stbi_image_free(pixels);
    return out;
}

} /// @note namespace fbzz::editor::assetthumbnail
