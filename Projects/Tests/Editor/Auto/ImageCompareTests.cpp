/// @file    ImageCompareTests.cpp
/// @brief   絵の回帰判定 (平均差・違う画素の割合・差分画像・PNG 往復) を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <TestKit/TestKit.hpp>

#include <Editor/Playtest/ImageCompare.hpp>

#include <cstdint>
#include <vector>

namespace fbzz::tests {
namespace {

using editor::playtest::CompareImages;
using editor::playtest::DecodePng;
using editor::playtest::EncodePng;
using editor::playtest::ImageDiffSettings;
using editor::playtest::RgbaImage;

RgbaImage Solid(uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b)
{
    RgbaImage image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<size_t>(width) * height * 4);
    for (size_t index = 0; index < image.pixels.size(); index += 4) {
        image.pixels[index] = r;
        image.pixels[index + 1] = g;
        image.pixels[index + 2] = b;
        image.pixels[index + 3] = 255;
    }
    return image;
}

} // namespace

TEST(ImageCompare, IdenticalImagesHaveNoDifference)
{
    const RgbaImage image = Solid(8, 4, 40, 80, 120);

    const auto result = CompareImages(image, image, ImageDiffSettings{});

    ASSERT_TRUE(result.comparable);
    EXPECT_DOUBLE_EQ(result.meanDiff, 0.0);
    EXPECT_DOUBLE_EQ(result.badPixelRatio, 0.0);
    EXPECT_EQ(result.badPixels, 0u);
}

TEST(ImageCompare, CountsOnlyPixelsBeyondTheThreshold)
{
    const RgbaImage baseline = Solid(10, 10, 100, 100, 100);
    RgbaImage actual = baseline;
    /// @note 1 画素だけ大きく、残り全部をしきい値未満だけずらす。
    actual.pixels[0] = 255;
    for (size_t index = 4; index < actual.pixels.size(); index += 4) actual.pixels[index] = 105;

    ImageDiffSettings settings;
    settings.pixelThreshold = 0.1f;
    const auto result = CompareImages(actual, baseline, settings);

    ASSERT_TRUE(result.comparable);
    EXPECT_EQ(result.badPixels, 1u);
    EXPECT_DOUBLE_EQ(result.badPixelRatio, 0.01);
    EXPECT_GT(result.meanDiff, 0.0);
}

TEST(ImageCompare, MarksDifferentPixelsRedInTheDiffImage)
{
    const RgbaImage baseline = Solid(2, 1, 0, 0, 0);
    RgbaImage actual = baseline;
    actual.pixels[4 + 1] = 255;

    const auto result = CompareImages(actual, baseline, ImageDiffSettings{});

    ASSERT_TRUE(result.diff.IsValid());
    EXPECT_EQ(result.diff.pixels[4], 255);
    EXPECT_EQ(result.diff.pixels[5], 0);
    EXPECT_EQ(result.diff.pixels[0], 0) << "変わっていない画素を赤くしない";
}

TEST(ImageCompare, RefusesToCompareDifferentSizes)
{
    const auto result = CompareImages(Solid(4, 4, 0, 0, 0), Solid(4, 5, 0, 0, 0), ImageDiffSettings{});
    EXPECT_FALSE(result.comparable);
}

TEST(ImageCompare, IgnoresAlpha)
{
    const RgbaImage baseline = Solid(3, 3, 10, 20, 30);
    RgbaImage actual = baseline;
    for (size_t index = 3; index < actual.pixels.size(); index += 4) actual.pixels[index] = 0;

    const auto result = CompareImages(actual, baseline, ImageDiffSettings{});
    EXPECT_EQ(result.badPixels, 0u);
}

TEST(ImageCompare, PngRoundTripKeepsPixels)
{
    RgbaImage image = Solid(5, 3, 1, 2, 3);
    image.pixels[7] = 128;

    std::vector<uint8_t> png;
    ASSERT_TRUE(EncodePng(image, png));
    RgbaImage decoded;
    ASSERT_TRUE(DecodePng(png, decoded));

    EXPECT_EQ(decoded.width, image.width);
    EXPECT_EQ(decoded.height, image.height);
    EXPECT_EQ(decoded.pixels, image.pixels);
}

TEST(ImageCompare, DecodeRejectsGarbage)
{
    RgbaImage out;
    EXPECT_FALSE(DecodePng({ 1, 2, 3, 4 }, out));
    EXPECT_FALSE(DecodePng({}, out));
}

} // namespace fbzz::tests
