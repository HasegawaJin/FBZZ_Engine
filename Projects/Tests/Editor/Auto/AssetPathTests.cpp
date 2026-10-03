/// @file    AssetPathTests.cpp
/// @brief   Editor 内で共有する "Assets/..." 起点パスの正規化。
/// @author  Hasegawa Jin
/// @date    2026-09-10
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Editor/Util/AssetPath.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <filesystem>
#include <string>

namespace fbzz::tests {

using editor::NormalizeAssetPath;
using editor::ToProjectAssetDiskPath;

class AssetPathTest : public testkit::EngineFixture {};

TEST_F(AssetPathTest, KeepsAPathThatIsAlreadyRelativeToAssets)
{
    EXPECT_EQ(NormalizeAssetPath("Assets/Models/Player.fbx"), "Assets/Models/Player.fbx");
}

TEST_F(AssetPathTest, ConvertsBackslashesToForwardSlashes)
{
    EXPECT_EQ(NormalizeAssetPath("Assets\\Models\\Player.fbx"), "Assets/Models/Player.fbx");
}

TEST_F(AssetPathTest, CapitalisesALowerCaseAssetsPrefix)
{
    /// @note 索引のキーは大小を吸収するが、保存される文字列は 1 通りに揃えたい。
    EXPECT_EQ(NormalizeAssetPath("assets/Models/Player.fbx"), "Assets/Models/Player.fbx");
}

TEST_F(AssetPathTest, TrimsAnAbsolutePathDownToTheAssetsRoot)
{
    EXPECT_EQ(NormalizeAssetPath("C:/proj/GreenWare/Assets/Models/Player.fbx"),
              "Assets/Models/Player.fbx");
    EXPECT_EQ(NormalizeAssetPath("C:\\proj\\GreenWare\\Assets\\Models\\Player.fbx"),
              "Assets/Models/Player.fbx");
}

TEST_F(AssetPathTest, TrimsAnAbsolutePathWithALowerCaseAssetsSegment)
{
    EXPECT_EQ(NormalizeAssetPath("C:/proj/greenware/assets/Models/Player.fbx"),
              "Assets/Models/Player.fbx");
}

TEST_F(AssetPathTest, LeavesAPathWithNoAssetsSegmentAlone)
{
    /// @note Assets の外 (エンジン内蔵の素材など) は勝手に切り詰めない。
    EXPECT_EQ(NormalizeAssetPath("C:/proj/Library/Baked/abc/Player.fzasset"),
              "C:/proj/Library/Baked/abc/Player.fzasset");
    EXPECT_EQ(NormalizeAssetPath(""), "");
}

TEST_F(AssetPathTest, JoinsAnAssetsPathOntoTheProjectRoot)
{
    EXPECT_EQ(ToProjectAssetDiskPath("C:/proj/GreenWare", "Assets/Models/Player.fbx"),
              "C:/proj/GreenWare/Assets/Models/Player.fbx");
}

TEST_F(AssetPathTest, NormalisesBeforeJoining)
{
    EXPECT_EQ(ToProjectAssetDiskPath("C:/proj/GreenWare", "assets\\Models\\Player.fbx"),
              "C:/proj/GreenWare/Assets/Models/Player.fbx");
}

TEST_F(AssetPathTest, ReturnsThePathUnchangedWithoutAProjectRoot)
{
    EXPECT_EQ(ToProjectAssetDiskPath("", "Assets/Models/Player.fbx"),
              "Assets/Models/Player.fbx");
}

TEST_F(AssetPathTest, DoesNotJoinAPathOutsideAssets)
{
    /// @note "Assets/" 起点でないものにプロジェクトルートを足すと、存在しない場所を指す。
    EXPECT_EQ(ToProjectAssetDiskPath("C:/proj/GreenWare", "C:/elsewhere/Player.fbx"),
              "C:/elsewhere/Player.fbx");
}

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

TEST_F(AssetPathGuidTest, MissingGuidDoesNotRedirectAnOperationToHint)
{
    EXPECT_TRUE(ToProjectAssetDiskPath("C:/proj/GreenWare",
        "guid:ffffffffffffffffffffffffffffffff|Assets/Original.prefab").empty());
}

} /// @note namespace fbzz::tests
