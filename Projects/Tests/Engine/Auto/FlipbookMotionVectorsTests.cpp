/// @file    FlipbookMotionVectorsTests.cpp
/// @brief   画像から推定する MV が、保存値の規約 (−移動量 / S, Atlas UV) で出てくることを固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// 旧実装は «+移動量 / 探索半径 [px]» を書いており、シェーダーとは符号も単位も逆だった。
/// 既知の量だけ動かした 2 コマを解析させ、S と符号の両方を確かめる。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/FlipbookMotionVectorEncoding.hpp>
#include <Engine/Asset/FlipbookMotionVectors.hpp>

#include <vector>

namespace fbzz::tests {
namespace {

class FlipbookMotionVectorsTest : public testkit::EngineFixture {};

constexpr std::uint32_t kTile = 32;
constexpr std::uint32_t kAtlasWidth = kTile * 2;
constexpr std::uint32_t kAtlasHeight = kTile;
constexpr int kShift = 4;

/// 模様付きの 8x8 の四角。平坦だとブロックマッチングの同点が «静止» 側へ倒れてしまう。
void PaintSquare(std::vector<float>& rgba, std::uint32_t originX, std::uint32_t originY)
{
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            const std::size_t index = (static_cast<std::size_t>(originY + y) * kAtlasWidth + originX + x) * 4;
            const float value = 0.2f + 0.08f * static_cast<float>(x) + 0.03f * static_cast<float>(y);
            rgba[index + 0] = value;
            rgba[index + 1] = value;
            rgba[index + 2] = value;
        }
    }
}

std::vector<float> MakeShiftedPair()
{
    std::vector<float> rgba(static_cast<std::size_t>(kAtlasWidth) * kAtlasHeight * 4, 0.0f);
    for (std::size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 1.0f;
    PaintSquare(rgba, 8, 12);
    PaintSquare(rgba, kTile + 8 + kShift, 12);
    return rgba;
}

asset::FlipbookMotionVectorSettings PairSettings()
{
    asset::FlipbookMotionVectorSettings settings;
    settings.columns = 2;
    settings.rows = 1;
    settings.blockRadius = 4;
    settings.searchRadius = 8;
    settings.smoothIterations = 0;
    settings.loop = false;
    return settings;
}

} // namespace

TEST_F(FlipbookMotionVectorsTest, RecommendedStrengthIsTheShiftInAtlasUv)
{
    const std::vector<float> rgba = MakeShiftedPair();

    const asset::FlipbookMotionAnalysis analysis =
        asset::AnalyzeFlipbookMotion(rgba, kAtlasWidth, kAtlasHeight, PairSettings());

    ASSERT_TRUE(analysis.success) << analysis.message;
    EXPECT_NEAR(analysis.recommendedStrength, static_cast<float>(kShift) / kAtlasWidth, testkit::kTolerance);
    EXPECT_NEAR(analysis.maxObservedFlow, static_cast<float>(kShift), testkit::kTolerance);
}

TEST_F(FlipbookMotionVectorsTest, EncodedMotionPointsAgainstTheShift)
{
    const std::vector<float> rgba = MakeShiftedPair();

    const asset::FlipbookMotionAnalysis analysis =
        asset::AnalyzeFlipbookMotion(rgba, kAtlasWidth, kAtlasHeight, PairSettings());

    ASSERT_TRUE(analysis.success) << analysis.message;
    const std::size_t inside = static_cast<std::size_t>(16) * kAtlasWidth + 12;
    const asset::EncodedMotionVector encoded =
        asset::EncodeMotionVector(analysis.displacementUv[inside], analysis.recommendedStrength);
    const math::Vector2 decoded = asset::DecodeMotionVector(encoded.r, encoded.g);
    EXPECT_NEAR(decoded.x, -1.0f, 2.0f / 255.0f);
    EXPECT_NEAR(decoded.y, 0.0f, 2.0f / 255.0f);
}

TEST_F(FlipbookMotionVectorsTest, LastFrameWithoutLoopHasNoMotion)
{
    const std::vector<float> rgba = MakeShiftedPair();

    const asset::FlipbookMotionAnalysis analysis =
        asset::AnalyzeFlipbookMotion(rgba, kAtlasWidth, kAtlasHeight, PairSettings());

    ASSERT_TRUE(analysis.success) << analysis.message;
    const std::size_t lastFrame = static_cast<std::size_t>(16) * kAtlasWidth + kTile + 12;
    EXPECT_VEC2_NEAR(analysis.displacementUv[lastFrame], math::Vector2::ZERO, testkit::kTolerance);
}

} // namespace fbzz::tests
