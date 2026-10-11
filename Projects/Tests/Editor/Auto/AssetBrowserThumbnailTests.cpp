/// @file    AssetBrowserThumbnailTests.cpp
/// @brief   Asset type silhouettes and bounded CPU thumbnail decode contracts.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include <Editor/Panels/AssetBrowserThumbnail.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <TestKit/TestKit.hpp>
#include <array>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <vector>

namespace fbzz::tests {

class AssetBrowserThumbnailTest : public testkit::EngineFixture {
protected:
    testkit::TempDir m_temp{ "asset-browser-thumbnails" };
    std::filesystem::path Write(const char* name, const std::vector<uint8_t>& bytes)
    {
        const auto path = m_temp.Path() / name;
        std::ofstream output(path, std::ios::binary);
        output.write(static_cast<const char*>(static_cast<const void*>(bytes.data())), bytes.size());
        return path;
    }
};

TEST_F(AssetBrowserThumbnailTest, OriginalIconCategoriesLeaveRealPreviewTypesAvailable)
{
    using namespace editor::assetthumbnail;
    const std::array<Icon, 6> kinds{ TypeIcon(".cs"), TypeIcon(".scene"), TypeIcon(".prefab"),
        TypeIcon(".wav"), TypeIcon(".animcontroller"), TypeIcon(".hlsli") };
    for (size_t left = 0; left < kinds.size(); ++left) {
        EXPECT_NE(kinds[left], Icon::NONE);
        EXPECT_STRNE(IconName(kinds[left]), "");
        for (size_t right = left + 1; right < kinds.size(); ++right) EXPECT_NE(kinds[left], kinds[right]);
    }
    for (const char* extension : { ".fbx", ".mesh", ".mat", ".png", ".dds", ".curve", ".gradient" })
        EXPECT_EQ(TypeIcon(extension), Icon::NONE);
    EXPECT_EQ(TypeIcon(".anim"), TypeIcon(".sequence"));
    EXPECT_EQ(TypeIcon(".mp3"), TypeIcon(".flac"));
}

TEST_F(AssetBrowserThumbnailTest, ShrinksSourceWithoutLosingSourceAspectAndAlpha)
{
    /// @note A deterministic uncompressed RGBA TGA verifies the decoder without a GPU or external fixture.
    std::vector<uint8_t> bytes(18 + 512 * 128 * 4, 0);
    bytes[2] = 2; bytes[12] = 0; bytes[13] = 2; bytes[14] = 128; bytes[16] = 32; bytes[17] = 0x28;
    for (size_t index = 18; index < bytes.size(); index += 4) {
        bytes[index] = 20; bytes[index + 1] = 80; bytes[index + 2] = 200; bytes[index + 3] = 128;
    }
    const auto image = editor::assetthumbnail::Decode(Write("image.tga", bytes));
    ASSERT_EQ(image.state, editor::assetthumbnail::State::READY);
    EXPECT_EQ(image.width, 256u); EXPECT_EQ(image.height, 64u);
    EXPECT_EQ(image.sourceWidth, 512u); EXPECT_EQ(image.sourceHeight, 128u);
    ASSERT_EQ(image.rgba.size(), 256 * 64 * 4);
    EXPECT_EQ(image.rgba[0], 200); EXPECT_EQ(image.rgba[1], 80);
    EXPECT_EQ(image.rgba[2], 20); EXPECT_EQ(image.rgba[3], 128);
}

TEST_F(AssetBrowserThumbnailTest, DistinguishesMissingCorruptUnsupportedAndLargeFiles)
{
    using namespace editor::assetthumbnail;
    EXPECT_EQ(Decode(m_temp.Path() / "missing.png").state, State::FAILED);
    EXPECT_EQ(Decode(Write("corrupt.png", { 1, 2, 3 })).state, State::FAILED);
    EXPECT_EQ(Decode(Write("unknown.xyz", { 1, 2, 3 })).state, State::UNSUPPORTED);
    const auto path = m_temp.Path() / "oversize.png";
    {
        std::ofstream output(path, std::ios::binary);
        output.seekp(static_cast<std::streamoff>(MAX_FILE_BYTES));
        output.put(0);
    }
    EXPECT_EQ(Decode(path).state, State::UNSUPPORTED);
}

TEST_F(AssetBrowserThumbnailTest, RejectsHugeImageHeaderBeforePixelAllocation)
{
    std::vector<uint8_t> bytes(18, 0);
    bytes[2] = 2; bytes[12] = 0xff; bytes[13] = 0x7f;
    bytes[14] = 0xff; bytes[15] = 0x7f; bytes[16] = 32;
    const auto image = editor::assetthumbnail::Decode(Write("huge.tga", bytes));
    EXPECT_EQ(image.state, editor::assetthumbnail::State::UNSUPPORTED);
    EXPECT_TRUE(image.rgba.empty());
}

} /// @note namespace fbzz::tests
