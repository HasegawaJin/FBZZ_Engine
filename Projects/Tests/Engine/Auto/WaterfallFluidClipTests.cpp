/// @file    WaterfallFluidClipTests.cpp
/// @brief   Bake output, frame streaming, and rejection of stale or canceled clips.
/// @author  Hasegawa Jin
/// @date    2026-11-11
#include <Engine/Asset/WaterfallFluidClip.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <TestKit/TempDir.hpp>
#include <TestKit/TestKit.hpp>
#include <algorithm>

namespace fbzz::tests {
namespace {
class WaterfallFluidClipTest : public ::testing::Test {
protected:
    testkit::TempDir m_temp{"waterfall-fluid"};

    asset::WaterfallFluidBakeInput MakeInput() const
    {
        asset::WaterfallFluidBakeInput input;
        input.grid = 24;
        input.frameCount = 8;
        input.inlet = {0.0f, 0.50f, 0.0f};
        input.inletVelocity = {0.0f, -3.0f, 0.15f};
        input.inletAxis = math::Vector3::RIGHT;
        input.inletHalfWidth = 0.22f;
        input.receiverHeight = -0.42f;
        input.gravity = 2.5f;
        input.particlesPerSecond = 480;
        input.stamp = 42;
        return input;
    }
};
} /// @note namespace

TEST_F(WaterfallFluidClipTest, BakeStreamsAnAnimatedThreeDimensionalSurface)
{
    auto input = MakeInput();
    input.obstacles.push_back({{0.0f, -0.05f, 0.0f}, {}, {}, 0.15f});
    const auto path = util::FileSystem::PathToUtf8(m_temp.File("impact.wfall"));
    std::atomic<bool> cancel{false};
    std::atomic<int> progress{0};
    ASSERT_EQ(asset::BakeWaterfallFluidClip(input, path, cancel, progress), "");
    EXPECT_EQ(progress.load(), 100);

    asset::WaterfallFluidFrame first, next, loop;
    ASSERT_TRUE(asset::LoadWaterfallFluidFrame(path, input.stamp, 0, first));
    ASSERT_TRUE(asset::LoadWaterfallFluidFrame(path, input.stamp, 2, next));
    ASSERT_TRUE(asset::LoadWaterfallFluidFrame(path, input.stamp, input.frameCount, loop));
    ASSERT_FALSE(first.indices.empty());
    EXPECT_EQ(first.indices, loop.indices);
    EXPECT_EQ(next.frame, 2);
    EXPECT_EQ(first.frameCount, input.frameCount);
    EXPECT_EQ(first.framesPerSecond, input.framesPerSecond);
    const auto [minZ, maxZ] = std::minmax_element(first.vertices.begin(), first.vertices.end(),
        [](const auto& a, const auto& b) { return a.position.z < b.position.z; });
    EXPECT_GT(maxZ->position.z - minZ->position.z, 0.15f);
    EXPECT_TRUE(std::any_of(first.vertices.begin(), first.vertices.end(),
        [](const auto& v) { return v.coverage > 0.5f; }));
    EXPECT_FALSE(std::equal(first.vertices.begin(), first.vertices.end(), next.vertices.begin(), next.vertices.end(),
        [](const auto& a, const auto& b) { return a.position == b.position; }));
    EXPECT_FALSE(asset::LoadWaterfallFluidFrame(path, input.stamp + 1, 0, first));
    EXPECT_EQ(first.frame, 0);
}

TEST_F(WaterfallFluidClipTest, CancelLeavesAnExistingAssetUntouched)
{
    const auto file = m_temp.File("impact.wfall");
    const auto path = util::FileSystem::PathToUtf8(file);
    ASSERT_TRUE(util::FileSystem::WriteTextAtomic(path, "existing"));
    std::atomic<bool> cancel{true};
    std::atomic<int> progress{0};
    EXPECT_FALSE(asset::BakeWaterfallFluidClip(MakeInput(), path, cancel, progress).empty());
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(util::FileSystem::ReadBinary(file, bytes));
    EXPECT_EQ(std::string(bytes.begin(), bytes.end()), "existing");
}
} /// @note namespace fbzz::tests
