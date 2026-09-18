/// @file    FlipbookMotionVectorEncodingTests.cpp
/// @brief   MV アトラスの保存値が Particle.hlsl の warp と噛み合う規約を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// 符号や単位を取り違えても «補間がやや濁る» だけで何も落ちない (旧生成器は両方逆だった)。
/// シェーダーの計算を写した WarpCurrentUv / WarpNextUv と組み合わせ、
/// 既知の並進を 2 コマが同じ内容点で拾えることを機械的に確かめる。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/FlipbookMotionVectorEncoding.hpp>
#include <Engine/Scene/Components/ParticleColorSpace.hpp>

#include <vector>

namespace fbzz::tests {
namespace {

class FlipbookMotionVectorEncodingTest : public testkit::EngineFixture {};

math::Vector2 Scale(math::Vector2 v, float s) { return { v.x * s, v.y * s }; }

} // namespace

TEST_F(FlipbookMotionVectorEncodingTest, ZeroMotionEncodesToMidGrey)
{
    const asset::EncodedMotionVector encoded = asset::EncodeMotionVector({ 0.0f, 0.0f }, 0.02f);

    EXPECT_EQ(encoded.r, 128);
    EXPECT_EQ(encoded.g, 128);
}

TEST_F(FlipbookMotionVectorEncodingTest, PositiveDisplacementIsStoredWithNegatedSign)
{
    const asset::EncodedMotionVector encoded = asset::EncodeMotionVector({ 0.01f, -0.01f }, 0.02f);

    const math::Vector2 decoded = asset::DecodeMotionVector(encoded.r, encoded.g);
    EXPECT_LT(decoded.x, 0.0f);
    EXPECT_GT(decoded.y, 0.0f);
}

TEST_F(FlipbookMotionVectorEncodingTest, DecodedMotionTimesStrengthRestoresNegatedDisplacement)
{
    constexpr float kStrength = 0.02f;
    const std::vector<math::Vector2> displacements = {
        { 0.02f, 0.0f }, { -0.02f, 0.013f }, { 0.0071f, -0.0033f }, { 0.0f, 0.0f } };
    /// @note 8bit の量子化は [-1,1] で半ステップ 1/255。S を掛けると S/255 まで。
    const float tolerance = kStrength / 255.0f * 1.001f;

    for (const math::Vector2& d : displacements) {
        const asset::EncodedMotionVector encoded = asset::EncodeMotionVector(d, kStrength);
        const math::Vector2 restored = Scale(asset::DecodeMotionVector(encoded.r, encoded.g), kStrength);

        EXPECT_NEAR(restored.x, -d.x, tolerance);
        EXPECT_NEAR(restored.y, -d.y, tolerance);
    }
}

TEST_F(FlipbookMotionVectorEncodingTest, WarpedSamplesLandOnTheSameContentForAKnownShift)
{
    const math::Vector2 d = { 0.01f, -0.02f };
    const math::Vector2 d0[] = { d };
    const float strength = asset::ComputeRecommendedStrength(d0, 0.0f);
    const asset::EncodedMotionVector encoded = asset::EncodeMotionVector(d, strength);
    const math::Vector2 motion = asset::DecodeMotionVector(encoded.r, encoded.g);
    const math::Vector2 pixel = { 0.3f, 0.4f };
    const float tolerance = 2.0f * strength / 255.0f;

    for (const float blend : { 0.25f, 0.5f, 0.75f }) {
        const math::Vector2 current = asset::WarpCurrentUv(pixel, motion, blend, strength);
        const math::Vector2 next = asset::WarpNextUv(pixel, motion, blend, strength);

        /// @note コマ N+blend の画素 p にある内容は、コマ N では p - blend·d、コマ N+1 では p + (1-blend)·d にある。
        EXPECT_VEC2_NEAR(current, (math::Vector2{ pixel.x - d.x * blend, pixel.y - d.y * blend }), tolerance)
            << "blend=" << blend;
        EXPECT_VEC2_NEAR(next, (math::Vector2{ pixel.x + d.x * (1.0f - blend), pixel.y + d.y * (1.0f - blend) }),
                         tolerance)
            << "blend=" << blend;
    }
}

TEST_F(FlipbookMotionVectorEncodingTest, RecommendedStrengthIsTheLargestAxisMagnitude)
{
    const std::vector<math::Vector2> displacements = { { 0.01f, -0.03f }, { 0.02f, 0.0f } };
    const std::vector<math::Vector2> still = { { 0.0f, 0.0f } };

    EXPECT_NEAR(asset::ComputeRecommendedStrength(displacements, 0.001f), 0.03f, testkit::kTolerance);
    EXPECT_NEAR(asset::ComputeRecommendedStrength(still, 0.001f), 0.001f, testkit::kTolerance);
}

TEST_F(FlipbookMotionVectorEncodingTest, DisplacementConvertsPerAxisForNonSquareAtlas)
{
    const asset::FlipbookGrid grid = asset::ComputeFlipbookGrid(32, 8);

    const math::Vector2 fromPixels = asset::PixelToAtlasUv(8.0f, 8.0f, 1024, 512);
    const math::Vector2 fromTile = asset::TileUvToAtlasUv({ 0.5f, 0.5f }, grid);

    EXPECT_VEC2_NEAR(fromPixels, (math::Vector2{ 8.0f / 1024.0f, 8.0f / 512.0f }), testkit::kTolerance);
    EXPECT_EQ(grid.rows, 4);
    EXPECT_VEC2_NEAR(fromTile, (math::Vector2{ 0.5f / 8.0f, 0.5f / 4.0f }), testkit::kTolerance);
}

TEST_F(FlipbookMotionVectorEncodingTest, GridMatchesTheAtlasBakerLayoutRule)
{
    const asset::FlipbookGrid square = asset::ComputeFlipbookGrid(64, 0);
    const asset::FlipbookGrid ragged = asset::ComputeFlipbookGrid(50, 0);
    const asset::FlipbookGrid fixed = asset::ComputeFlipbookGrid(10, 4);
    const asset::FlipbookGrid clamped = asset::ComputeFlipbookGrid(3, 8);

    EXPECT_EQ(square.columns, 8);
    EXPECT_EQ(square.rows, 8);
    EXPECT_EQ(ragged.columns, 8);
    EXPECT_EQ(ragged.rows, 7);
    EXPECT_EQ(fixed.columns, 4);
    EXPECT_EQ(fixed.rows, 3);
    EXPECT_EQ(clamped.columns, 3);
    EXPECT_EQ(clamped.rows, 1);
}

TEST_F(FlipbookMotionVectorEncodingTest, TileOriginIsRowMajorFromTopLeftLikeSpriteRect)
{
    const asset::FlipbookGrid grid = asset::ComputeFlipbookGrid(16, 4);

    const asset::FlipbookTileOrigin origin = asset::TileOriginPx(5, grid, 64, 32);

    /// @note ParticlePass の SpriteRectForFrame: x = frame % columns, y = frame / columns。
    EXPECT_EQ(origin.x, 64u);
    EXPECT_EQ(origin.y, 32u);
}

TEST_F(FlipbookMotionVectorEncodingTest, DilationFillsUncoveredTexelsWithoutCrossingTiles)
{
    /// @note 2x2 のタイル 2 枚を横に並べた 4x2 の Atlas。各タイルに 1 画素だけ覆われた画素がある。
    const asset::FlipbookGrid grid = asset::ComputeFlipbookGrid(2, 2);
    std::vector<math::Vector2> motion(8, math::Vector2::ZERO);
    std::vector<float> coverage(8, 0.0f);
    motion[0] = { 1.0f, 0.0f };
    coverage[0] = 1.0f;
    motion[7] = { 0.0f, 5.0f };
    coverage[7] = 1.0f;

    asset::DilateMotion(motion, coverage, 4, 2, grid, 2, 2, /*iterations=*/4);

    for (const int index : { 0, 1, 4, 5 })
        EXPECT_VEC2_NEAR(motion[index], (math::Vector2{ 1.0f, 0.0f }), testkit::kTolerance) << "index=" << index;
    for (const int index : { 2, 3, 6, 7 })
        EXPECT_VEC2_NEAR(motion[index], (math::Vector2{ 0.0f, 5.0f }), testkit::kTolerance) << "index=" << index;
}

TEST_F(FlipbookMotionVectorEncodingTest, ColorTexelIsSrgbEncodedWithLinearAlphaAndReportsClipping)
{
    const asset::EncodedColorTexel inRange = asset::EncodeColorTexel({ 0.25f, 0.5f, 0.0f, 0.5f }, 2.0f);
    const asset::EncodedColorTexel clipped = asset::EncodeColorTexel({ 0.1f, 0.0f, 3.0f, 1.0f }, 1.0f);

    const auto expected = [](float linear) {
        return static_cast<int>(scene::ParticleLinearToSrgb(linear) * 255.0f + 0.5f);
    };
    EXPECT_EQ(inRange.r, expected(0.5f));
    EXPECT_EQ(inRange.g, expected(1.0f));
    EXPECT_EQ(inRange.b, 0);
    EXPECT_EQ(inRange.a, 128);
    EXPECT_FALSE(inRange.clipped);
    EXPECT_TRUE(clipped.clipped);
    EXPECT_EQ(clipped.b, 255);
}

} // namespace fbzz::tests
