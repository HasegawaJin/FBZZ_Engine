/// @file    FluidJetFlamePlaybackTests.cpp
/// @brief   JetFlame の 2D/3D 焼き画像で上昇とループの継ぎ目を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-25
#include <TestKit/TestKit.hpp>

#include <Editor/Playtest/ImageCompare.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

class FluidJetFlamePlaybackTest : public testkit::Fixture {};

using editor::playtest::RgbaImage;

std::filesystem::path JetFlameAsset(const char* name)
{
    return std::filesystem::path(FBZZ_SOURCE_DIR) / "GreenWare/Assets/VFX/Fluid" / name;
}

bool LoadAtlas(const char* name, RgbaImage& image)
{
    std::vector<std::uint8_t> bytes;
    return editor::playtest::ReadBinaryFile(JetFlameAsset(name), bytes)
        && editor::playtest::DecodePng(bytes, image)
        && image.IsValid() && image.width % 8 == 0 && image.height % 8 == 0;
}

double CentroidFromTop(const RgbaImage& image, int frame)
{
    const int width = static_cast<int>(image.width) / 8;
    const int height = static_cast<int>(image.height) / 8;
    const int originX = frame % 8 * width;
    const int originY = frame / 8 * height;
    double mass = 0.0;
    double weightedY = 0.0;
    for (int y = 0; y < height; y += 2) for (int x = 0; x < width; x += 2) {
        const auto offset = (static_cast<std::size_t>(originY + y) * image.width + originX + x) * 4;
        const double luminance = (0.2126 * image.pixels[offset] + 0.7152 * image.pixels[offset + 1]
                                  + 0.0722 * image.pixels[offset + 2]) / 255.0;
        const double alpha = image.pixels[offset + 3] / 255.0;
        const double visibility = (std::max)(luminance, alpha);
        mass += visibility;
        weightedY += visibility * y;
    }
    return mass > 0.0 ? weightedY / mass / height : 1.0;
}

double FrameDifference(const RgbaImage& image, int first, int second)
{
    const int width = static_cast<int>(image.width) / 8;
    const int height = static_cast<int>(image.height) / 8;
    double difference = 0.0;
    std::size_t samples = 0;
    for (int y = 0; y < height; y += 2) for (int x = 0; x < width; x += 2) {
        const auto a = (static_cast<std::size_t>(first / 8 * height + y) * image.width
            + first % 8 * width + x) * 4;
        const auto b = (static_cast<std::size_t>(second / 8 * height + y) * image.width
            + second % 8 * width + x) * 4;
        for (int channel = 0; channel < 4; ++channel)
            difference += std::abs(static_cast<int>(image.pixels[a + channel])
                                 - static_cast<int>(image.pixels[b + channel])) / (4.0 * 255.0);
        ++samples;
    }
    return difference / static_cast<double>(samples);
}

} /// @note namespace

TEST_F(FluidJetFlamePlaybackTest, TwoDimensionalBakeRisesAndClosesAtTheSeam)
{
    RgbaImage image;
    ASSERT_TRUE(LoadAtlas("JetFlame_Flipbook.png", image));
    EXPECT_GT(CentroidFromTop(image, 0) - CentroidFromTop(image, 12), 0.06);
    double typical = 0.0;
    for (int frame = 0; frame < 63; ++frame) typical += FrameDifference(image, frame, frame + 1);
    typical /= 63.0;
    EXPECT_LT(FrameDifference(image, 63, 0), (std::max)(0.015, typical * 2.5));
}

TEST_F(FluidJetFlamePlaybackTest, ThreeDimensionalSixWayBakeRisesAndClosesAtTheSeam)
{
    RgbaImage positive;
    RgbaImage negative;
    RgbaImage color;
    RgbaImage emission;
    ASSERT_TRUE(LoadAtlas("JetFlame_6wayP.png", positive));
    ASSERT_TRUE(LoadAtlas("JetFlame_6wayN.png", negative));
    ASSERT_TRUE(LoadAtlas("JetFlame_6wayC.png", color));
    ASSERT_TRUE(LoadAtlas("JetFlame_6wayE.png", emission));
    EXPECT_GT(CentroidFromTop(positive, 16) - CentroidFromTop(positive, 40), 0.06);
    for (const RgbaImage* map : { &positive, &negative, &color, &emission }) {
        double typical = 0.0;
        for (int frame = 0; frame < 63; ++frame) typical += FrameDifference(*map, frame, frame + 1);
        typical /= 63.0;
        EXPECT_LT(FrameDifference(*map, 63, 0), (std::max)(0.015, typical * 2.5));
    }
}

} /// @note namespace fbzz::tests
