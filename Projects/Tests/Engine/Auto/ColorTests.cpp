/// @file    ColorTests.cpp
/// @brief   色の 32bit / 16 進数 / HSV 変換と補間が往復することを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// 色は Inspector・シリアライズ・シェーダー定数の 3 経路を行き来する。
/// 往復で 1/255 ずれる、RGBA の並びが 1 つずれる、といった壊れ方は
/// «なんとなく違う色» にしか見えず、目で追い切れない。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Util/Color.hpp>

#include <Math/Vector4.hpp>

namespace fbzz::tests {

using util::Color;

class ColorTest : public testkit::EngineFixture {};

// --- 32bit 表現 -------------------------------------------------------------

TEST_F(ColorTest, PacksAndUnpacksInRgbaOrder)
{
    // 並びが 1 つずれると赤と青が入れ替わる。0xRRGGBBAA の順を固定する。
    const Color color = Color::FromRGBA32(0xFF8000C0u);

    EXPECT_NEAR(color.r, 1.0f, 1.0f / 255.0f);
    EXPECT_NEAR(color.g, 128.0f / 255.0f, 1.0f / 255.0f);
    EXPECT_NEAR(color.b, 0.0f, 1.0f / 255.0f);
    EXPECT_NEAR(color.a, 192.0f / 255.0f, 1.0f / 255.0f);
}

TEST_F(ColorTest, RoundTripsThrough32Bit)
{
    const uint32_t packed[] = { 0x00000000u, 0xFFFFFFFFu, 0x123456F0u, 0xFF8000C0u };

    for (const uint32_t value : packed)
        EXPECT_EQ(Color::FromRGBA32(value).ToRGBA32(), value);
}

TEST_F(ColorTest, ClampsOutOfRangeChannelsWhenPacking)
{
    // HDR の色をそのまま 8bit へ落とす経路がある。折り返して暗くなってはいけない。
    const Color overBright{ 4.0f, -1.0f, 0.5f, 1.0f };

    const uint32_t packed = overBright.ToRGBA32();

    EXPECT_EQ((packed >> 24) & 0xFFu, 255u);
    EXPECT_EQ((packed >> 16) & 0xFFu, 0u);
}

// --- 16 進数文字列 ----------------------------------------------------------

TEST_F(ColorTest, ParsesSixAndEightDigitHex)
{
    const Color rgb  = Color::FromHex("#FF0000");
    const Color rgba = Color::FromHex("#FF000080");

    EXPECT_NEAR(rgb.r, 1.0f, 1.0f / 255.0f);
    EXPECT_NEAR(rgb.a, 1.0f, 1.0f / 255.0f);   // 省略時は不透明
    EXPECT_NEAR(rgba.a, 128.0f / 255.0f, 1.0f / 255.0f);
}

TEST_F(ColorTest, HexParsingIgnoresCaseAndAcceptsNoHash)
{
    EXPECT_NEAR(Color::FromHex("#00ff00").g, 1.0f, 1.0f / 255.0f);
    EXPECT_NEAR(Color::FromHex("00FF00").g, 1.0f, 1.0f / 255.0f);
}

// --- Vector4 との往復 -------------------------------------------------------

TEST_F(ColorTest, RoundTripsThroughVector4)
{
    const Color original{ 0.25f, 0.5f, 0.75f, 0.125f };

    const Color back = Color::FromVector4(original.ToVector4());

    EXPECT_NEAR(back.r, original.r, testkit::kTolerance);
    EXPECT_NEAR(back.g, original.g, testkit::kTolerance);
    EXPECT_NEAR(back.b, original.b, testkit::kTolerance);
    EXPECT_NEAR(back.a, original.a, testkit::kTolerance);
}

// --- HSV --------------------------------------------------------------------

TEST_F(ColorTest, BuildsThePrimariesFromHue)
{
    EXPECT_NEAR(Color::FromHSV(0.0f, 1.0f, 1.0f).r, 1.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(Color::FromHSV(120.0f, 1.0f, 1.0f).g, 1.0f, testkit::kLooseTolerance);
    EXPECT_NEAR(Color::FromHSV(240.0f, 1.0f, 1.0f).b, 1.0f, testkit::kLooseTolerance);
}

TEST_F(ColorTest, ZeroSaturationIsGrey)
{
    const Color grey = Color::FromHSV(200.0f, 0.0f, 0.5f);

    EXPECT_NEAR(grey.r, grey.g, testkit::kLooseTolerance);
    EXPECT_NEAR(grey.g, grey.b, testkit::kLooseTolerance);
    EXPECT_NEAR(grey.r, 0.5f, testkit::kLooseTolerance);
}

TEST_F(ColorTest, RoundTripsThroughHSV)
{
    // カラーピッカーは RGB ↔ HSV を往復し続ける。ずれると触るたびに色が流れる。
    const Color original{ 0.2f, 0.6f, 0.4f, 1.0f };

    float h = 0.0f, s = 0.0f, v = 0.0f;
    original.ToHSV(h, s, v);
    const Color back = Color::FromHSV(h, s, v);

    EXPECT_NEAR(back.r, original.r, testkit::kLooseTolerance);
    EXPECT_NEAR(back.g, original.g, testkit::kLooseTolerance);
    EXPECT_NEAR(back.b, original.b, testkit::kLooseTolerance);
}

TEST_F(ColorTest, HueWrapsAroundTheColourWheel)
{
    const Color atZero = Color::FromHSV(0.0f, 1.0f, 1.0f);
    const Color atFull = Color::FromHSV(360.0f, 1.0f, 1.0f);

    EXPECT_NEAR(atZero.r, atFull.r, testkit::kLooseTolerance);
    EXPECT_NEAR(atZero.g, atFull.g, testkit::kLooseTolerance);
    EXPECT_NEAR(atZero.b, atFull.b, testkit::kLooseTolerance);
}

// --- 補間 -------------------------------------------------------------------

TEST_F(ColorTest, LerpReturnsTheEndpoints)
{
    EXPECT_NEAR(Color::Lerp(Color::Red, Color::Blue, 0.0f).r, 1.0f, testkit::kTolerance);
    EXPECT_NEAR(Color::Lerp(Color::Red, Color::Blue, 1.0f).b, 1.0f, testkit::kTolerance);
}

TEST_F(ColorTest, LerpClampsOutsideTheUnitInterval)
{
    // フェードの t は経過時間から作るので、1 を少し超えることが普通に起きる。
    const Color beyond = Color::Lerp(Color::Black, Color::White, 2.0f);

    EXPECT_NEAR(beyond.r, 1.0f, testkit::kTolerance);
}

TEST_F(ColorTest, LerpUnclampedExtrapolates)
{
    const Color beyond = Color::LerpUnclamped(Color::Black, Color::White, 2.0f);

    EXPECT_NEAR(beyond.r, 2.0f, testkit::kTolerance);
}

TEST_F(ColorTest, WithAlphaKeepsTheColourAndReplacesTheAlpha)
{
    const Color faded = Color::Red.WithAlpha(0.25f);

    EXPECT_NEAR(faded.r, 1.0f, testkit::kTolerance);
    EXPECT_NEAR(faded.a, 0.25f, testkit::kTolerance);
}

TEST_F(ColorTest, NamedConstantsAreOpaqueExceptClear)
{
    EXPECT_NEAR(Color::White.a, 1.0f, testkit::kTolerance);
    EXPECT_NEAR(Color::Black.a, 1.0f, testkit::kTolerance);
    EXPECT_NEAR(Color::Clear.a, 0.0f, testkit::kTolerance);
}

} // namespace fbzz::tests
