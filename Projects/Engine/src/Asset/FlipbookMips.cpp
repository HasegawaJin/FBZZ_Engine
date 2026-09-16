/// @file    FlipbookMips.cpp
/// @brief   コマを跨がない、アルファを考慮したフリップブックのミップ生成
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/FlipbookMips.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace fbzz::asset {
namespace {

constexpr std::uint32_t kMinTileSize = 4;

const std::array<float, 256>& SrgbToLinearTable()
{
    static const std::array<float, 256> table = [] {
        std::array<float, 256> values{};
        for (int i = 0; i < 256; ++i) {
            const float c = static_cast<float>(i) / 255.0f;
            values[static_cast<std::size_t>(i)] =
                c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return values;
    }();
    return table;
}

std::uint8_t ToUnorm8(float value)
{
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

std::uint8_t LinearToSrgb8(float linear)
{
    linear = std::clamp(linear, 0.0f, 1.0f);
    const float encoded = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return ToUnorm8(encoded);
}

void Downsample(const FlipbookMipLevel& source, FlipbookMipContent content, FlipbookMipLevel& out)
{
    out.width = source.width / 2;
    out.height = source.height / 2;
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);
    const auto& toLinear = SrgbToLinearTable();
    const bool srgb = content == FlipbookMipContent::StraightSrgb || content == FlipbookMipContent::PremultipliedSrgb;
    const bool weighted = content == FlipbookMipContent::StraightSrgb || content == FlipbookMipContent::CoverageWeighted;

    for (std::uint32_t y = 0; y < out.height; ++y) {
        for (std::uint32_t x = 0; x < out.width; ++x) {
            float color[3] = { 0.0f, 0.0f, 0.0f };
            float plainColor[3] = { 0.0f, 0.0f, 0.0f };
            float alphaSum = 0.0f;
            for (std::uint32_t dy = 0; dy < 2; ++dy) {
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const std::size_t i =
                        (static_cast<std::size_t>(y * 2 + dy) * source.width + (x * 2 + dx)) * 4;
                    const float alpha = static_cast<float>(source.rgba[i + 3]) / 255.0f;
                    for (int c = 0; c < 3; ++c) {
                        const std::uint8_t encoded = source.rgba[i + static_cast<std::size_t>(c)];
                        const float value = srgb ? toLinear[encoded] : static_cast<float>(encoded) / 255.0f;
                        color[c] += weighted ? value * alpha : value;
                        plainColor[c] += value;
                    }
                    alphaSum += alpha;
                }
            }
            const std::size_t o = (static_cast<std::size_t>(y) * out.width + x) * 4;
            for (int c = 0; c < 3; ++c) {
                // 全部が透明なら重みが 0 になる。そのときは素の平均を残す (後段の双線形で縁の色が要る)。
                const float value = weighted
                    ? (alphaSum > 1.0e-6f ? color[c] / alphaSum : plainColor[c] * 0.25f)
                    : color[c] * 0.25f;
                out.rgba[o + static_cast<std::size_t>(c)] = srgb ? LinearToSrgb8(value) : ToUnorm8(value);
            }
            out.rgba[o + 3] = ToUnorm8(alphaSum * 0.25f);
        }
    }
}

} // namespace

std::uint32_t FlipbookMipLevelCount(std::uint32_t tileWidth, std::uint32_t tileHeight)
{
    std::uint32_t count = 1;
    while (tileWidth % 2 == 0 && tileHeight % 2 == 0 && tileWidth / 2 >= kMinTileSize
           && tileHeight / 2 >= kMinTileSize) {
        tileWidth /= 2;
        tileHeight /= 2;
        ++count;
    }
    return count;
}

std::vector<FlipbookMipLevel> BuildFlipbookMips(std::span<const std::uint8_t> rgba, std::uint32_t width,
                                                std::uint32_t height, std::uint32_t tileWidth,
                                                std::uint32_t tileHeight, FlipbookMipContent content)
{
    std::vector<FlipbookMipLevel> levels;
    if (width == 0 || height == 0 || rgba.size() < static_cast<std::size_t>(width) * height * 4) return levels;
    FlipbookMipLevel base;
    base.width = width;
    base.height = height;
    base.rgba.assign(rgba.begin(), rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(width) * height * 4));
    levels.push_back(std::move(base));

    const bool tilesFit = tileWidth > 0 && tileHeight > 0 && width % tileWidth == 0 && height % tileHeight == 0;
    const std::uint32_t count = tilesFit ? FlipbookMipLevelCount(tileWidth, tileHeight) : 1;
    for (std::uint32_t level = 1; level < count; ++level) {
        FlipbookMipLevel next;
        Downsample(levels.back(), content, next);
        levels.push_back(std::move(next));
    }
    return levels;
}

} // namespace fbzz::asset
