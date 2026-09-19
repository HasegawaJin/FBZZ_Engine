/// @file    ParticleFlipbookTests.cpp
/// @brief   フリップブックのコマ評価 (CPU シミュレーションと Inspector プレビューの共通規則) を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// ParticlePass の CPU 経路と .mat Inspector のプレビューは同じ EvaluateFlipbookFrame() を呼ぶ。
/// ここが揺れると «プレビューでは合っているのにゲームでは 1 コマずれる» になり、
/// しかも両方を並べて見比べない限り気付けない。

#include <TestKit/TestKit.hpp>

#include <Engine/Asset/ParticleMaterialSettings.hpp>

namespace fbzz::tests {
namespace {

asset::ParticleFlipbookSettings Grid(int columns, int rows)
{
    asset::ParticleFlipbookSettings flipbook;
    flipbook.spriteColumns = columns;
    flipbook.spriteRows    = rows;
    return flipbook;
}

} // namespace

TEST(ParticleFlipbookTest, EndFrameZeroMeansPlayToTheLastFrame)
{
    const auto range = asset::ResolveFlipbookRange(Grid(4, 2));
    EXPECT_EQ(range.first, 0);
    EXPECT_EQ(range.last, 7);
}

TEST(ParticleFlipbookTest, RangeIsClampedIntoTheAtlas)
{
    auto flipbook = Grid(4, 2);
    flipbook.spriteStartFrame = 5;
    /// @note コマ数 8 を超える
    flipbook.spriteEndFrame   = 40;
    auto range = asset::ResolveFlipbookRange(flipbook);
    EXPECT_EQ(range.first, 5);
    EXPECT_EQ(range.last, 7);

    /// @note Start より前は Start へ詰める
    flipbook.spriteEndFrame = 2;
    range = asset::ResolveFlipbookRange(flipbook);
    EXPECT_EQ(range.last, 5);
}

TEST(ParticleFlipbookTest, NonPositiveGridIsASingleImage)
{
    const auto range = asset::ResolveFlipbookRange(Grid(0, 0));
    EXPECT_EQ(range.columns, 1);
    EXPECT_EQ(range.rows, 1);
    EXPECT_EQ(range.last, 0);
    EXPECT_EQ(Grid(0, -3).FrameCount(), 1);
}

TEST(ParticleFlipbookTest, LifetimeSpansTheRangeOverTheLifetimeWithoutWrapping)
{
    const auto flipbook = Grid(4, 1);
    EXPECT_EQ(asset::EvaluateFlipbookFrame(flipbook, 0.0f, 0.0f, 0.0f).frame, 0);
    /// @note 0.5 * 3 = 1.5
    EXPECT_EQ(asset::EvaluateFlipbookFrame(flipbook, 0.5f, 0.0f, 0.0f).frame, 1);
    const auto end = asset::EvaluateFlipbookFrame(flipbook, 1.0f, 0.0f, 0.0f);
    EXPECT_EQ(end.frame, 3);
    /// @note 寿命の終わりで先頭へ補間し始めると、消える直前に 1 コマ目が透けて見える。
    EXPECT_EQ(end.nextFrame, 3);
}

TEST(ParticleFlipbookTest, FpsLoopsAndBlendsIntoTheFirstFrame)
{
    auto flipbook = Grid(4, 1);
    flipbook.flipbookMode            = scene::ParticleFlipbookMode::FramesPerSecond;
    flipbook.flipbookFramesPerSecond = 4.0f;
    flipbook.flipbookFrameBlending   = true;

    /// @note 3.5 コマ目
    const auto last = asset::EvaluateFlipbookFrame(flipbook, 0.0f, 0.875f, 0.0f);
    EXPECT_EQ(last.frame, 3);
    EXPECT_EQ(last.nextFrame, 0);
    EXPECT_NEAR(last.blend, 0.5f, 1.0e-4f);
    /// @note 2 周目
    EXPECT_EQ(asset::EvaluateFlipbookFrame(flipbook, 0.0f, 1.25f, 0.0f).frame, 1);
}

TEST(ParticleFlipbookTest, PingPongPlaysBackwardsAfterTheLastFrame)
{
    auto flipbook = Grid(4, 1);
    flipbook.flipbookMode            = scene::ParticleFlipbookMode::PingPong;
    flipbook.flipbookFramesPerSecond = 1.0f;
    /// @note 0 1 2 3 2 1 | 0 ...
    EXPECT_EQ(asset::EvaluateFlipbookFrame(flipbook, 0.0f, 3.0f, 0.0f).frame, 3);
    EXPECT_EQ(asset::EvaluateFlipbookFrame(flipbook, 0.0f, 4.5f, 0.0f).frame, 1);
    EXPECT_EQ(asset::EvaluateFlipbookFrame(flipbook, 0.0f, 6.2f, 0.0f).frame, 0);
}

TEST(ParticleFlipbookTest, RandomRowStaysInsideTheChosenRow)
{
    auto flipbook = Grid(4, 3);
    flipbook.spriteRandomRow = true;
    for (const float seed : { 0.0f, 0.2f, 0.5f, 0.99f }) {
        const int row = static_cast<int>(seed * 3.0f);
        for (const float age : { 0.0f, 0.5f, 1.0f }) {
            const auto sample = asset::EvaluateFlipbookFrame(flipbook, age, 0.0f, seed);
            EXPECT_GE(sample.frame, row * 4);
            EXPECT_LE(sample.frame, row * 4 + 3);
            EXPECT_GE(sample.nextFrame, row * 4);
            EXPECT_LE(sample.nextFrame, row * 4 + 3);
        }
    }
}

TEST(ParticleFlipbookTest, RandomStartFrameStaysInsideTheRange)
{
    auto flipbook = Grid(8, 1);
    flipbook.spriteStartFrame       = 2;
    flipbook.spriteEndFrame         = 5;
    flipbook.spriteRandomStartFrame = true;
    for (int step = 0; step <= 20; ++step) {
        const float seed   = static_cast<float>(step) / 20.0f;
        const auto  sample = asset::EvaluateFlipbookFrame(flipbook, 0.7f, 0.0f, seed);
        EXPECT_GE(sample.frame, 2);
        EXPECT_LE(sample.frame, 5);
    }
}

TEST(ParticleFlipbookTest, RandomModeNeverBlends)
{
    auto flipbook = Grid(4, 4);
    flipbook.flipbookMode          = scene::ParticleFlipbookMode::RandomFrame;
    flipbook.flipbookFrameBlending = true;
    const auto sample = asset::EvaluateFlipbookFrame(flipbook, 0.3f, 1.7f, 0.42f);
    EXPECT_EQ(sample.blend, 0.0f);
    /// @note floor(0.42 * 15)
    EXPECT_EQ(sample.frame, 6);
}

} // namespace fbzz::tests
