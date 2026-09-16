/// @file    FlipbookMipsTests.cpp
/// @brief   フリップブックのミップ (コマを跨がない・アルファで重み付け) を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// 滲みも黒い縁も «遠くで小さく見えたとき» にしか出ないので、画面を見て気付くのが遅れる。

#include <TestKit/TestKit.hpp>

#include <Engine/Asset/FlipbookMips.hpp>

#include <cstdint>
#include <vector>

namespace fbzz::tests {
namespace {

void Fill(std::vector<std::uint8_t>& rgba, std::uint32_t width, std::uint32_t x0, std::uint32_t y0,
          std::uint32_t w, std::uint32_t h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
{
    for (std::uint32_t y = y0; y < y0 + h; ++y)
        for (std::uint32_t x = x0; x < x0 + w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
            rgba[i + 0] = r;
            rgba[i + 1] = g;
            rgba[i + 2] = b;
            rgba[i + 3] = a;
        }
}

} // namespace

TEST(FlipbookMipsTest, LevelCountStopsBeforeTilesGetTooSmallOrOdd)
{
    EXPECT_EQ(asset::FlipbookMipLevelCount(256, 256), 7u); // 256 → 4
    EXPECT_EQ(asset::FlipbookMipLevelCount(8, 8), 2u);
    EXPECT_EQ(asset::FlipbookMipLevelCount(24, 24), 3u);   // 24 → 12 → 6 (次は 3 で奇数)
    EXPECT_EQ(asset::FlipbookMipLevelCount(6, 6), 1u);
}

TEST(FlipbookMipsTest, NeighbouringTilesDoNotBleed)
{
    constexpr std::uint32_t kWidth = 16, kHeight = 8;
    std::vector<std::uint8_t> rgba(kWidth * kHeight * 4, 0);
    Fill(rgba, kWidth, 0, 0, 8, 8, 255, 0, 0, 255);   // 左のコマ = 赤
    Fill(rgba, kWidth, 8, 0, 8, 8, 0, 0, 255, 255);   // 右のコマ = 青
    const auto levels = asset::BuildFlipbookMips(rgba, kWidth, kHeight, 8, 8, asset::FlipbookMipContent::Plain);
    ASSERT_EQ(levels.size(), 2u);
    const auto& mip = levels[1];
    ASSERT_EQ(mip.width, 8u);
    ASSERT_EQ(mip.height, 4u);
    for (std::uint32_t y = 0; y < mip.height; ++y) {
        const std::size_t leftEdge = (static_cast<std::size_t>(y) * mip.width + 3) * 4;
        const std::size_t rightEdge = (static_cast<std::size_t>(y) * mip.width + 4) * 4;
        EXPECT_EQ(mip.rgba[leftEdge + 0], 255);
        EXPECT_EQ(mip.rgba[leftEdge + 2], 0);
        EXPECT_EQ(mip.rgba[rightEdge + 0], 0);
        EXPECT_EQ(mip.rgba[rightEdge + 2], 255);
    }
}

TEST(FlipbookMipsTest, StraightAlphaIgnoresTheColourOfTransparentTexels)
{
    // 2x2 のうち 1 画素だけ不透明な白、残りは透明な黒。素直に平均すると灰色の縁になる。
    std::vector<std::uint8_t> rgba(8 * 8 * 4, 0);
    Fill(rgba, 8, 0, 0, 1, 1, 255, 255, 255, 255);
    const auto levels = asset::BuildFlipbookMips(rgba, 8, 8, 8, 8, asset::FlipbookMipContent::StraightSrgb);
    ASSERT_EQ(levels.size(), 2u);
    EXPECT_EQ(levels[1].rgba[0], 255);
    EXPECT_EQ(levels[1].rgba[3], 64);
}

TEST(FlipbookMipsTest, PremultipliedColourIsAveragedInLinearSpace)
{
    std::vector<std::uint8_t> rgba(8 * 8 * 4, 0);
    Fill(rgba, 8, 0, 0, 8, 8, 0, 0, 0, 255);
    Fill(rgba, 8, 0, 0, 1, 2, 255, 255, 255, 255);   // 2x2 の左半分が白
    const auto levels =
        asset::BuildFlipbookMips(rgba, 8, 8, 8, 8, asset::FlipbookMipContent::PremultipliedSrgb);
    ASSERT_EQ(levels.size(), 2u);
    // リニアで 0.5 → sRGB で 188。sRGB のまま平均すると 128 になり、縮めるほど暗く沈む。
    EXPECT_NEAR(static_cast<int>(levels[1].rgba[0]), 188, 1);
    EXPECT_EQ(levels[1].rgba[3], 255);
}

TEST(FlipbookMipsTest, GridThatDoesNotDivideTheAtlasKeepsOnlyTheBaseLevel)
{
    std::vector<std::uint8_t> rgba(12 * 8 * 4, 128);
    const auto levels = asset::BuildFlipbookMips(rgba, 12, 8, 8, 8, asset::FlipbookMipContent::Plain);
    EXPECT_EQ(levels.size(), 1u);
}

} // namespace fbzz::tests
