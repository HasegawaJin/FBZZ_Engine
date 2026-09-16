/// @file    FlipbookMotionVectorEncoding.cpp
/// @brief   モーションベクター付きフリップブックの保存値規約と Atlas 配置の実装。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/FlipbookMotionVectorEncoding.hpp>

#include <Engine/Scene/Components/ParticleColorSpace.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fbzz::asset {
namespace {

std::uint8_t ToUnorm8(float value)
{
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

} // namespace

FlipbookGrid ComputeFlipbookGrid(int frameCount, int requestedColumns)
{
    FlipbookGrid grid;
    grid.frameCount = (std::max)(frameCount, 1);
    grid.columns = requestedColumns > 0
        ? (std::min)(requestedColumns, grid.frameCount)
        : static_cast<int>(std::ceil(std::sqrt(static_cast<double>(grid.frameCount))));
    grid.rows = (grid.frameCount + grid.columns - 1) / grid.columns;
    return grid;
}

FlipbookTileOrigin TileOriginPx(int frame, const FlipbookGrid& grid,
                                std::uint32_t tileWidth, std::uint32_t tileHeight)
{
    const int columns = (std::max)(grid.columns, 1);
    return { static_cast<std::uint32_t>(frame % columns) * tileWidth,
             static_cast<std::uint32_t>(frame / columns) * tileHeight };
}

math::Vector2 TileUvToAtlasUv(math::Vector2 tileUv, const FlipbookGrid& grid)
{
    return { tileUv.x / static_cast<float>((std::max)(grid.columns, 1)),
             tileUv.y / static_cast<float>((std::max)(grid.rows, 1)) };
}

math::Vector2 PixelToAtlasUv(float dxPixels, float dyPixels,
                             std::uint32_t atlasWidth, std::uint32_t atlasHeight)
{
    return { dxPixels / static_cast<float>((std::max)(atlasWidth, 1u)),
             dyPixels / static_cast<float>((std::max)(atlasHeight, 1u)) };
}

float ComputeRecommendedStrength(std::span<const math::Vector2> displacementsUv,
                                 float minimumStrength)
{
    float strength = 0.0f;
    for (const math::Vector2& d : displacementsUv)
        strength = (std::max)(strength, (std::max)(std::fabs(d.x), std::fabs(d.y)));
    return strength > 0.0f ? strength : (std::max)(minimumStrength, 1.0e-6f);
}

EncodedMotionVector EncodeMotionVector(math::Vector2 displacementUv, float strength)
{
    const float inverse = strength > 0.0f ? 1.0f / strength : 0.0f;
    const float mx = std::clamp(-displacementUv.x * inverse, -1.0f, 1.0f);
    const float my = std::clamp(-displacementUv.y * inverse, -1.0f, 1.0f);
    return { ToUnorm8(mx * 0.5f + 0.5f), ToUnorm8(my * 0.5f + 0.5f) };
}

math::Vector2 DecodeMotionVector(std::uint8_t r, std::uint8_t g)
{
    return { static_cast<float>(r) / 255.0f * 2.0f - 1.0f,
             static_cast<float>(g) / 255.0f * 2.0f - 1.0f };
}

math::Vector2 WarpCurrentUv(math::Vector2 uv, math::Vector2 decodedMotion,
                            float blend, float strength)
{
    const float scale = blend * (std::max)(strength, 0.0f);
    return { uv.x + decodedMotion.x * scale, uv.y + decodedMotion.y * scale };
}

math::Vector2 WarpNextUv(math::Vector2 uv, math::Vector2 decodedMotion,
                         float blend, float strength)
{
    const float scale = (1.0f - blend) * (std::max)(strength, 0.0f);
    return { uv.x - decodedMotion.x * scale, uv.y - decodedMotion.y * scale };
}

void DilateMotion(std::span<math::Vector2> motion, std::span<const float> coverage,
                  std::uint32_t atlasWidth, std::uint32_t atlasHeight,
                  const FlipbookGrid& grid, std::uint32_t tileWidth, std::uint32_t tileHeight,
                  int iterations, float threshold)
{
    const std::size_t pixelCount = static_cast<std::size_t>(atlasWidth) * atlasHeight;
    if (iterations <= 0 || motion.size() < pixelCount || coverage.size() < pixelCount
        || tileWidth == 0 || tileHeight == 0)
        return;

    std::vector<std::uint8_t> filled(pixelCount);
    for (std::size_t i = 0; i < pixelCount; ++i) filled[i] = coverage[i] > threshold ? 1u : 0u;

    std::vector<math::Vector2> nextMotion(motion.begin(), motion.begin() + pixelCount);
    std::vector<std::uint8_t> nextFilled(filled);
    const std::uint32_t usedWidth = (std::min)(atlasWidth, tileWidth * static_cast<std::uint32_t>(grid.columns));
    const std::uint32_t usedHeight = (std::min)(atlasHeight, tileHeight * static_cast<std::uint32_t>(grid.rows));

    for (int pass = 0; pass < iterations; ++pass) {
        bool changed = false;
        for (std::uint32_t y = 0; y < usedHeight; ++y) {
            const std::uint32_t tileTop = (y / tileHeight) * tileHeight;
            for (std::uint32_t x = 0; x < usedWidth; ++x) {
                const std::size_t index = static_cast<std::size_t>(y) * atlasWidth + x;
                if (filled[index]) continue;
                const std::uint32_t tileLeft = (x / tileWidth) * tileWidth;

                float sumX = 0.0f;
                float sumY = 0.0f;
                int count = 0;
                const auto accumulate = [&](std::uint32_t nx, std::uint32_t ny) {
                    const std::size_t neighbour = static_cast<std::size_t>(ny) * atlasWidth + nx;
                    if (!filled[neighbour]) return;
                    sumX += motion[neighbour].x;
                    sumY += motion[neighbour].y;
                    ++count;
                };
                if (x > tileLeft) accumulate(x - 1, y);
                if (x + 1 < tileLeft + tileWidth) accumulate(x + 1, y);
                if (y > tileTop) accumulate(x, y - 1);
                if (y + 1 < tileTop + tileHeight) accumulate(x, y + 1);
                if (count == 0) continue;

                nextMotion[index] = { sumX / static_cast<float>(count), sumY / static_cast<float>(count) };
                nextFilled[index] = 1u;
                changed = true;
            }
        }
        if (!changed) break;
        std::copy(nextMotion.begin(), nextMotion.end(), motion.begin());
        filled = nextFilled;
    }
}

EncodedColorTexel EncodeColorTexel(const math::Vector4& linearPremultiplied, float exposure)
{
    const float scale = (std::max)(exposure, 0.0f);
    const float r = linearPremultiplied.x * scale;
    const float g = linearPremultiplied.y * scale;
    const float b = linearPremultiplied.z * scale;

    EncodedColorTexel texel;
    texel.clipped = r > 1.0f || g > 1.0f || b > 1.0f;
    texel.r = ToUnorm8(scene::ParticleLinearToSrgb(std::clamp(r, 0.0f, 1.0f)));
    texel.g = ToUnorm8(scene::ParticleLinearToSrgb(std::clamp(g, 0.0f, 1.0f)));
    texel.b = ToUnorm8(scene::ParticleLinearToSrgb(std::clamp(b, 0.0f, 1.0f)));
    texel.a = ToUnorm8(linearPremultiplied.w);
    return texel;
}

} // namespace fbzz::asset
