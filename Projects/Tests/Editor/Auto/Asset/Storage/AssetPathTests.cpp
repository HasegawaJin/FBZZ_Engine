/// @file    AssetPathTests.cpp
/// @brief   Editor 内で共有する "Assets/..." 起点パスの正規化。
/// @author  Hasegawa Jin
/// @date    2026-09-10
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Editor/Asset/Storage/AssetPath.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Util/Files/FileSystem.hpp>

#include <filesystem>
#include <string>

namespace fbzz::tests {

using editor::NormalizeAssetPath;
using editor::ToProjectAssetDiskPath;

class AssetPathTest : public testkit::EngineFixture {};

/// @brief Keeps a path that is already relative to assets.
TEST_F(AssetPathTest, KeepsAPathThatIsAlreadyRelativeToAssets)
{
    EXPECT_EQ(NormalizeAssetPath("Assets/Models/Player.fbx"), "Assets/Models/Player.fbx");
}

/// @brief Converts backslashes to forward slashes.
TEST_F(AssetPathTest, ConvertsBackslashesToForwardSlashes)
{
    EXPECT_EQ(NormalizeAssetPath("Assets\\Models\\Player.fbx"), "Assets/Models/Player.fbx");
}

/// @brief Capitalises a lower case assets prefix.
TEST_F(AssetPathTest, CapitalisesALowerCaseAssetsPrefix)
{
    // 索引のキーは大小を吸収するが、保存される文字列は 1 通りに揃えたい。
    EXPECT_EQ(NormalizeAssetPath("assets/Models/Player.fbx"), "Assets/Models/Player.fbx");
}

/// @brief Trims an absolute path down to the assets root.
TEST_F(AssetPathTest, TrimsAnAbsolutePathDownToTheAssetsRoot)
{
    EXPECT_EQ(NormalizeAssetPath("C:/proj/GreenWare/Assets/Models/Player.fbx"),
              "Assets/Models/Player.fbx");
    EXPECT_EQ(NormalizeAssetPath("C:\\proj\\GreenWare\\Assets\\Models\\Player.fbx"),
              "Assets/Models/Player.fbx");
}

/// @brief Trims an absolute path with a lower case assets segment.
TEST_F(AssetPathTest, TrimsAnAbsolutePathWithALowerCaseAssetsSegment)
{
    EXPECT_EQ(NormalizeAssetPath("C:/proj/greenware/assets/Models/Player.fbx"),
              "Assets/Models/Player.fbx");
}

/// @brief Leaves a path with no assets segment alone.
TEST_F(AssetPathTest, LeavesAPathWithNoAssetsSegmentAlone)
{
    // Assets の外 (エンジン内蔵の素材など) は勝手に切り詰めない。
    EXPECT_EQ(NormalizeAssetPath("C:/proj/Library/Baked/abc/Player.fzasset"),
              "C:/proj/Library/Baked/abc/Player.fzasset");
    EXPECT_EQ(NormalizeAssetPath(""), "");
}

/// @brief Joins an assets path onto the project root.
TEST_F(AssetPathTest, JoinsAnAssetsPathOntoTheProjectRoot)
{
    EXPECT_EQ(ToProjectAssetDiskPath("C:/proj/GreenWare", "Assets/Models/Player.fbx"),
              "C:/proj/GreenWare/Assets/Models/Player.fbx");
}

/// @brief Normalizes separators and the Assets root before resolving the path against the project directory.
TEST_F(AssetPathTest, NormalisesBeforeJoining)
{
    EXPECT_EQ(ToProjectAssetDiskPath("C:/proj/GreenWare", "assets\\Models\\Player.fbx"),
              "C:/proj/GreenWare/Assets/Models/Player.fbx");
}

/// @brief Returns the path unchanged without a project root.
TEST_F(AssetPathTest, ReturnsThePathUnchangedWithoutAProjectRoot)
{
    EXPECT_EQ(ToProjectAssetDiskPath("", "Assets/Models/Player.fbx"),
              "Assets/Models/Player.fbx");
}

/// @brief Does not join a path outside assets.
TEST_F(AssetPathTest, DoesNotJoinAPathOutsideAssets)
{
    // "Assets/" 起点でないものにプロジェクトルートを足すと、存在しない場所を指す。
    EXPECT_EQ(ToProjectAssetDiskPath("C:/proj/GreenWare", "C:/elsewhere/Player.fbx"),
              "C:/elsewhere/Player.fbx");
}

/// @brief Never strips GUID identity because its hint contains assets.
TEST_F(AssetPathTest, NeverStripsGuidIdentityBecauseItsHintContainsAssets)
{
    const std::string reference = "guid:0123456789abcdef0123456789abcdef|Assets/Original.prefab";
    EXPECT_EQ(NormalizeAssetPath(reference), reference);
}

class AssetPathGuidTest : public testkit::EngineFixture {
protected:
    void TearDown() override
    {
        asset::AssetDatabase::Shutdown();
        EngineFixture::TearDown();
    }
};

/// @brief GUID resolves moved prefab instead of old hint.
TEST_F(AssetPathGuidTest, GuidResolvesMovedPrefabInsteadOfOldHint)
{
    testkit::TempDir temp{"editor-prefab-path"};
    ASSERT_TRUE(temp.IsValid());
    const auto assets = temp.File("Assets");
    std::filesystem::create_directories(assets);
    const std::string before = temp.File("Assets/Original.prefab").generic_string();
    const std::string after = temp.File("Assets/Renamed.prefab").generic_string();
    ASSERT_TRUE(util::FileSystem::WriteText(before, "version = 1\n"));
    asset::AssetDatabase::Init(assets.generic_string());
    const auto guid = asset::AssetDatabase::TryGetGuidFromPath(before);
    ASSERT_FALSE(guid.empty());
    const std::string reference = "guid:" + guid + "|Assets/Original.prefab";
    std::filesystem::rename(before, after);
    std::filesystem::rename(before + ".meta", after + ".meta");
    asset::AssetDatabase::OnAssetMoved(before, after);

    EXPECT_EQ(ToProjectAssetDiskPath(temp.Path().generic_string(), reference), after);
}

/// @brief Missing GUID does not redirect an operation to hint.
TEST_F(AssetPathGuidTest, MissingGuidDoesNotRedirectAnOperationToHint)
{
    EXPECT_TRUE(ToProjectAssetDiskPath("C:/proj/GreenWare",
        "guid:ffffffffffffffffffffffffffffffff|Assets/Original.prefab").empty());
}

} /// @note namespace fbzz::tests
