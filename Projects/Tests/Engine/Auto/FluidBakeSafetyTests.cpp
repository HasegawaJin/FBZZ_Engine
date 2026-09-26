/// @file    FluidBakeSafetyTests.cpp
/// @brief   Bake の容量検査と DDS 圧縮キャンセルを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-25
#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>

#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/FluidBakeBudget.hpp>
#include <Engine/Asset/FluidBaker.hpp>
#include <Engine/Asset/FluidRecipeCodec.hpp>
#include <Engine/Asset/FlipbookMips.hpp>
#include <Engine/Util/FileSystem.hpp>

#include "Asset/FlipbookImageIO.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace fbzz::tests {
namespace {

class FluidBakeSafetyTest : public testkit::Fixture {};

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
}

} /// @note namespace

TEST_F(FluidBakeSafetyTest, RejectsAtlasWhoseTotalPixelsExceedBudget)
{
    fluid::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Fire);
    recipe.output.frameSize = 1024;
    recipe.output.columns = 8;
    recipe.output.rows = 8;
    const asset::FluidBakeBudget budget = asset::EstimateFluidBakeBudget(recipe);
    EXPECT_GT(budget.atlasPixels, 16ull * 1024ull * 1024ull);
    std::string error;
    EXPECT_FALSE(asset::ValidateFluidBakeBudget(budget, std::filesystem::current_path(), error));
    EXPECT_NE(error.find("16M"), std::string::npos);
}

TEST_F(FluidBakeSafetyTest, CancelledFlatBakeDoesNotWriteOutput)
{
    testkit::TempDir temp{ "fluid-cancel" };
    ASSERT_TRUE(temp.IsValid());
    fluid::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Fire);
    std::atomic<bool> cancel{ true };
    const std::string base = util::FileSystem::PathToUtf8(temp.File("Cancelled"));
    const asset::FluidBakeResult result = asset::BakeFluid(recipe, base, nullptr, nullptr, &cancel);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(std::filesystem::exists(temp.File("Cancelled_Flipbook.png")));
}

TEST_F(FluidBakeSafetyTest, StagedGuidIsIndexedOnlyAfterPublication)
{
    testkit::TempDir temp{ "fluid-staged-guid" };
    ASSERT_TRUE(temp.IsValid());
    const auto staged = temp.File("Staged.png");
    const auto published = temp.File("Published.png");
    ASSERT_TRUE(util::FileSystem::WriteText(util::FileSystem::PathToUtf8(staged), "test"));
    const std::string stagedPath = util::FileSystem::PathToUtf8(staged);
    const std::string guid = asset::AssetDatabase::EnsureGuidMetaUnindexed(stagedPath);
    ASSERT_FALSE(guid.empty());
    EXPECT_TRUE(asset::AssetDatabase::PathFromGuid(guid).empty());
    std::error_code error;
    std::filesystem::rename(staged, published, error);
    ASSERT_FALSE(error) << error.message();
    std::filesystem::rename(stagedPath + ".meta", util::FileSystem::PathToUtf8(published) + ".meta", error);
    ASSERT_FALSE(error) << error.message();
    EXPECT_EQ(asset::AssetDatabase::GuidFromPath(util::FileSystem::PathToUtf8(published)), guid);
    EXPECT_EQ(asset::AssetDatabase::PathFromGuid(guid), util::FileSystem::PathToUtf8(published));
    asset::AssetDatabase::OnAssetRemoved(util::FileSystem::PathToUtf8(published));
}

TEST_F(FluidBakeSafetyTest, CancellableCompressionKeepsDdsBlocksIdentical)
{
    testkit::TempDir temp{ "fluid-dds" };
    ASSERT_TRUE(temp.IsValid());
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 64;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
        const auto i = (static_cast<std::size_t>(y) * width + x) * 4;
        pixels[i] = static_cast<std::uint8_t>((x * 7 + y * 3) & 255u);
        pixels[i + 1] = static_cast<std::uint8_t>((x * 11 + y * 5) & 255u);
        pixels[i + 2] = static_cast<std::uint8_t>((x * 13 + y * 17) & 255u);
        pixels[i + 3] = static_cast<std::uint8_t>((x + y) & 255u);
    }
    const auto whole = temp.File("whole.dds");
    const auto tiled = temp.File("tiled.dds");
    std::string error;
    ASSERT_TRUE(asset::detail::SaveFlipbookDds(whole, width, height, pixels, 32, 32,
        asset::FlipbookMipContent::PremultipliedSrgb, asset::detail::FlipbookDdsCompression::BC7, error)) << error;
    std::atomic<bool> cancel{ false };
    ASSERT_TRUE(asset::detail::SaveFlipbookDds(tiled, width, height, pixels, 32, 32,
        asset::FlipbookMipContent::PremultipliedSrgb, asset::detail::FlipbookDdsCompression::BC7, error,
        &cancel)) << error;
    const auto wholeBytes = ReadBytes(whole);
    ASSERT_FALSE(wholeBytes.empty());
    EXPECT_EQ(ReadBytes(tiled), wholeBytes);

    cancel.store(true, std::memory_order_relaxed);
    const auto stopped = temp.File("stopped.dds");
    EXPECT_FALSE(asset::detail::SaveFlipbookDds(stopped, width, height, pixels, 32, 32,
        asset::FlipbookMipContent::PremultipliedSrgb, asset::detail::FlipbookDdsCompression::BC7, error,
        &cancel));
    EXPECT_FALSE(std::filesystem::exists(stopped));
}

} /// @note namespace fbzz::tests
