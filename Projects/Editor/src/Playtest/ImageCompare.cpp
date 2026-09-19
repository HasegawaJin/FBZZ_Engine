/// @file    ImageCompare.cpp
/// @brief   基準画像との比較と差分画像の生成。PNG の解読は stb_image、符号化は miniz。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <Editor/Playtest/ImageCompare.hpp>

#include <miniz.h>
#include <stb_image.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>

namespace fbzz::editor::playtest {

bool DecodePng(const std::vector<uint8_t>& png, RgbaImage& out)
{
    if (png.empty()) return false;
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &width, &height, &channels, 4);
    if (pixels == nullptr) return false;
    out.width = static_cast<uint32_t>(width);
    out.height = static_cast<uint32_t>(height);
    out.pixels.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    stbi_image_free(pixels);
    return out.IsValid();
}

bool EncodePng(const RgbaImage& image, std::vector<uint8_t>& out)
{
    if (!image.IsValid()) return false;
    size_t length = 0;
    void* encoded = tdefl_write_image_to_png_file_in_memory_ex(
        image.pixels.data(), static_cast<int>(image.width), static_cast<int>(image.height), 4, &length,
        MZ_DEFAULT_LEVEL, MZ_FALSE);
    if (encoded == nullptr) return false;
    const auto* bytes = static_cast<const uint8_t*>(encoded);
    out.assign(bytes, bytes + length);
    mz_free(encoded);
    return true;
}

ImageDiffResult CompareImages(const RgbaImage& actual, const RgbaImage& baseline, const ImageDiffSettings& settings)
{
    ImageDiffResult result;
    if (!actual.IsValid() || !baseline.IsValid()) return result;
    if (actual.width != baseline.width || actual.height != baseline.height) return result;

    result.comparable = true;
    result.diff.width = actual.width;
    result.diff.height = actual.height;
    result.diff.pixels.resize(actual.pixels.size());

    const size_t pixelCount = static_cast<size_t>(actual.width) * actual.height;
    const int threshold = static_cast<int>(std::clamp(settings.pixelThreshold, 0.0f, 1.0f) * 255.0f + 0.5f);
    double sum = 0.0;
    for (size_t index = 0; index < pixelCount; ++index) {
        const size_t offset = index * 4;
        const int dr = std::abs(static_cast<int>(actual.pixels[offset]) - baseline.pixels[offset]);
        const int dg = std::abs(static_cast<int>(actual.pixels[offset + 1]) - baseline.pixels[offset + 1]);
        const int db = std::abs(static_cast<int>(actual.pixels[offset + 2]) - baseline.pixels[offset + 2]);
        sum += static_cast<double>(dr + dg + db) / (3.0 * 255.0);

        const bool bad = std::max({ dr, dg, db }) > threshold;
        if (bad) ++result.badPixels;

        /// @note 基準画像を 1/4 の明るさで敷く。どこが変わったかを «絵のどの部分か» と一緒に読めるように。
        const uint8_t gray = static_cast<uint8_t>((baseline.pixels[offset] + baseline.pixels[offset + 1] + baseline.pixels[offset + 2]) / 12);
        result.diff.pixels[offset]     = bad ? 255 : gray;
        result.diff.pixels[offset + 1] = bad ? 0 : gray;
        result.diff.pixels[offset + 2] = bad ? 0 : gray;
        result.diff.pixels[offset + 3] = 255;
    }
    result.meanDiff = pixelCount > 0 ? sum / static_cast<double>(pixelCount) : 0.0;
    result.badPixelRatio = pixelCount > 0 ? static_cast<double>(result.badPixels) / static_cast<double>(pixelCount) : 0.0;
    return result;
}

bool ReadBinaryFile(const std::filesystem::path& path, std::vector<uint8_t>& out)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0) return false;
    stream.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) stream.read(static_cast<char*>(static_cast<void*>(out.data())), size);
    return static_cast<bool>(stream);
}

bool WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    if (!bytes.empty()) stream.write(static_cast<const char*>(static_cast<const void*>(bytes.data())), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

} // namespace fbzz::editor::playtest
