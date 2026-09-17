/// @file    WaterNoiseBakeTests.cpp
/// @brief   水面のさざ波タイル (タイラブル勾配ノイズ + ミップ連鎖) のベイクを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-16
///
/// このタイルは «勾配を格納してミップで平均する» ことで遠景のさざ波を平坦へ収束させる。
/// 平均が崩れると、水面が遠くで総毛立つ / 逆に一様に傾いた鏡になる、という形でしか表に出ない。
/// @see Docs/design/water-waves.md
#include <TestKit/TestKit.hpp>

#include "Scene/Systems/RenderPasses/Geometry/WaterNoiseBake.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fbzz::tests {
namespace {

namespace wn = scene::waternoise;

/// タイルは決定的で、焼くのに 512^2 の評価が要る。1 度だけ焼いて全テストで共有する。
const wn::Tile& SharedTile()
{
    static const wn::Tile tile = wn::BakeDetailTile();
    return tile;
}

/// 格納された RG を ∂h/∂q へ戻す。シェーダーの WaterNoiseTile と同じ式。
float DecodeDerivative(std::uint8_t encoded, float scale)
{
    return (static_cast<float>(encoded) / 255.0f * 2.0f - 1.0f) * scale;
}

class WaterNoiseBakeTest : public testkit::Fixture {};

} // namespace

/// @name ノイズそのもの

TEST_F(WaterNoiseBakeTest, GradientNoiseRepeatsExactlyAtThePeriod)
{
    /// @note タイルは WRAP でサンプルされる。周期でぴったり折り返さないと、水面に格子状の継ぎ目が出る。
    constexpr std::int32_t period = 16;
    for (int i = 0; i < 12; ++i) {
        const float x = -3.7f + static_cast<float>(i) * 1.31f;
        const float y = 0.9f + static_cast<float>(i) * 0.77f;

        float dx0 = 0.0f, dy0 = 0.0f, dx1 = 0.0f, dy1 = 0.0f;
        const float base   = wn::detail::GradientNoise(x, y, period, dx0, dy0);
        const float wrappedX = wn::detail::GradientNoise(x + period, y, period, dx1, dy1);
        EXPECT_NEAR(base, wrappedX, 1.0e-5f);
        EXPECT_NEAR(dx0, dx1, 1.0e-5f);
        EXPECT_NEAR(dy0, dy1, 1.0e-5f);

        const float wrappedY = wn::detail::GradientNoise(x, y + period, period, dx1, dy1);
        EXPECT_NEAR(base, wrappedY, 1.0e-5f);
    }
}

TEST_F(WaterNoiseBakeTest, GradientNoiseReturnsItsOwnAnalyticDerivative)
{
    /// @note 勾配は «値を 3 回引いて差分を取る» 代わりに解析で返している。ここがずれると
    ///       法線だけが静かに間違い、絵では «光り方がおかしい» としか分からない。
    constexpr std::int32_t period = 16;
    constexpr float h = 1.0e-3f;
    for (int i = 0; i < 12; ++i) {
        const float x = 0.31f + static_cast<float>(i) * 1.07f;
        const float y = -2.4f + static_cast<float>(i) * 0.53f;

        float dx = 0.0f, dy = 0.0f, ignored = 0.0f;
        wn::detail::GradientNoise(x, y, period, dx, dy);

        const float px = wn::detail::GradientNoise(x + h, y, period, ignored, ignored);
        const float mx = wn::detail::GradientNoise(x - h, y, period, ignored, ignored);
        const float py = wn::detail::GradientNoise(x, y + h, period, ignored, ignored);
        const float my = wn::detail::GradientNoise(x, y - h, period, ignored, ignored);

        EXPECT_NEAR(dx, (px - mx) / (2.0f * h), 2.0e-3f);
        EXPECT_NEAR(dy, (py - my) / (2.0f * h), 2.0e-3f);
    }
}

/// @name 焼き上がったタイル

TEST_F(WaterNoiseBakeTest, MipChainHalvesDownToASingleTexel)
{
    const wn::Tile& tile = SharedTile();
    ASSERT_FALSE(tile.mips.empty());
    EXPECT_EQ(tile.sizes.front(), wn::kTileSize);
    EXPECT_EQ(tile.sizes.back(), 1u);

    for (size_t level = 0; level < tile.mips.size(); ++level) {
        const std::uint32_t side = tile.sizes[level];
        EXPECT_EQ(tile.mips[level].size(), static_cast<size_t>(side) * side * 4u);
        if (level > 0) EXPECT_EQ(side, tile.sizes[level - 1] / 2u);
    }
}

TEST_F(WaterNoiseBakeTest, EachMipIsTheBoxAverageOfThePreviousLevel)
{
    /// @note «縮小 = 勾配の平均» が成り立つことが LOD の土台。ここが別のフィルタになると、
    ///       遠景の水面が «平坦» ではなく «別の傾き» へ収束する。
    const wn::Tile& tile = SharedTile();
    ASSERT_GE(tile.mips.size(), 2u);

    for (size_t level = 1; level < tile.mips.size(); ++level) {
        const std::uint32_t side = tile.sizes[level];
        const std::uint32_t srcSide = tile.sizes[level - 1];
        const auto& dst = tile.mips[level];
        const auto& src = tile.mips[level - 1];

        for (std::uint32_t y = 0; y < side; ++y) {
            for (std::uint32_t x = 0; x < side; ++x) {
                for (std::uint32_t c = 0; c < 4u; ++c) {
                    const size_t s0 = ((static_cast<size_t>(y) * 2u) * srcSide + x * 2u) * 4u + c;
                    const std::uint32_t sum = src[s0] + src[s0 + 4u]
                                            + src[s0 + static_cast<size_t>(srcSide) * 4u]
                                            + src[s0 + static_cast<size_t>(srcSide) * 4u + 4u];
                    ASSERT_EQ(dst[(static_cast<size_t>(y) * side + x) * 4u + c],
                              static_cast<std::uint8_t>((sum + 2u) / 4u));
                }
            }
        }
    }
}

TEST_F(WaterNoiseBakeTest, TheSmallestMipDecodesToAFlatSurface)
{
    /// @note 周期関数の勾配は 1 周期で積分するとゼロ。最小ミップ = タイル全体の平均なので、
    ///       «一番遠い水面» はどの向きにも傾いていない = 鏡になる。
    const wn::Tile& tile = SharedTile();
    const auto& last = tile.mips.back();
    ASSERT_EQ(last.size(), 4u);

    /// @note 許容は «典型的な勾配の 1/4»。厳密にゼロにならないのは、各段の 8bit 丸めが 9 回積もるため。
    const float tolerance = wn::kGradientRms * 0.25f;
    EXPECT_LT(std::abs(DecodeDerivative(last[0], tile.derivativeScale)), tolerance);
    EXPECT_LT(std::abs(DecodeDerivative(last[1], tile.derivativeScale)), tolerance);
}

TEST_F(WaterNoiseBakeTest, GradientRmsMatchesTheTargetSoDetailStrengthKeepsItsMeaning)
{
    /// @note 旧・値ノイズと同じ RMS へ正規化してある。ずれると、ノイズを差し替えただけで
    ///       既存 .mat の detailStrength が別の強さを意味してしまう。
    const wn::Tile& tile = SharedTile();
    const auto& mip0 = tile.mips.front();

    double squareSum = 0.0;
    const size_t texels = static_cast<size_t>(wn::kTileSize) * wn::kTileSize;
    for (size_t i = 0; i < texels; ++i) {
        const float dx = DecodeDerivative(mip0[i * 4u + 0u], tile.derivativeScale);
        const float dy = DecodeDerivative(mip0[i * 4u + 1u], tile.derivativeScale);
        squareSum += static_cast<double>(dx) * dx + static_cast<double>(dy) * dy;
    }
    const float rms = static_cast<float>(std::sqrt(squareSum / (2.0 * static_cast<double>(texels))));
    EXPECT_NEAR(rms, wn::kGradientRms, wn::kGradientRms * 0.05f);
}

TEST_F(WaterNoiseBakeTest, DerivativeScaleIsExactlyTheLargestBakedGradient)
{
    /// @note 復号係数は «実際に出た最大» でなければならない。大きすぎると 8bit の分解能を捨て、
    ///       小さいと山と谷が飽和して «平らな筋» になる。最大の成分がちょうど端へ張り付く。
    const wn::Tile& tile = SharedTile();
    const auto& mip0 = tile.mips.front();

    int lowest = 255;
    int highest = 0;
    const size_t texels = static_cast<size_t>(wn::kTileSize) * wn::kTileSize;
    for (size_t i = 0; i < texels; ++i) {
        for (std::uint32_t c = 0; c < 2u; ++c) {
            const int value = static_cast<int>(mip0[i * 4u + c]);
            lowest  = (std::min)(lowest,  value);
            highest = (std::max)(highest, value);
        }
    }
    /// @note 最大絶対値を取った成分の符号によって、張り付く端が変わる。
    EXPECT_TRUE(highest == 255 || lowest == 0) << "lowest=" << lowest << " highest=" << highest;
}

} // namespace fbzz::tests
