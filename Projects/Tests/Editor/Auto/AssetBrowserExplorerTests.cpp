/// @file    AssetBrowserExplorerTests.cpp
/// @brief   Explorer の対象がプロジェクト・GUID・仮想アセットの実体へ解決されることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include "Panels/AssetBrowser/AssetBrowserExplorer.hpp"
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <TestKit/TestKit.hpp>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>

namespace fbzz::tests {

class AssetBrowserExplorerTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        m_root = m_temp.Path() / util::FileSystem::PathFromUtf8("日本語 Project");
        std::error_code error;
        std::filesystem::create_directories(m_root / "Assets", error);
        ASSERT_FALSE(error);
    }

    void TearDown() override
    {
        asset::AssetDatabase::Shutdown();
        EngineFixture::TearDown();
    }

    std::string Root() const { return util::FileSystem::PathToUtf8(m_root); }
    std::string Path(const std::string& relative) const
    {
        return util::FileSystem::PathToUtf8(m_root / util::FileSystem::PathFromUtf8(relative));
    }

    bool WriteFile(const std::string& relative)
    {
        const auto path = util::FileSystem::PathFromUtf8(Path(relative));
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        return !error && util::FileSystem::WriteText(path, "source\n");
    }

    testkit::TempDir m_temp{"asset-browser-explorer"};
    std::filesystem::path m_root;
};

TEST_F(AssetBrowserExplorerTest, ResolvesUnicodeSpacesAndCommasRelativeToProject)
{
    const std::string relative = "Assets/日本語 Folder/対象 File,1.png";
    ASSERT_TRUE(WriteFile(relative));

    const auto target = editor::ResolveAssetBrowserExplorerTarget(Root(), relative);

    ASSERT_FALSE(target.path.empty());
    EXPECT_FALSE(target.isDirectory);
    EXPECT_TRUE(util::FileSystem::SamePathText(target.path, Path(relative)));
    EXPECT_TRUE(util::FileSystem::PathFromUtf8(target.path).is_absolute());
}

TEST_F(AssetBrowserExplorerTest, ResolvesBackslashesAndDotSegmentsFromProject)
{
    ASSERT_TRUE(WriteFile("Assets/日本語 Folder/対象 File.png"));

    const auto target = editor::ResolveAssetBrowserExplorerTarget(
        Root(), ".\\Assets\\日本語 Folder\\..\\日本語 Folder\\対象 File.png");

    EXPECT_TRUE(util::FileSystem::SamePathText(target.path, Path("Assets/日本語 Folder/対象 File.png")));
    EXPECT_FALSE(target.isDirectory);
}

TEST_F(AssetBrowserExplorerTest, OpensSelectedFolderIncludingAssetsRootAndTrailingSeparator)
{
    ASSERT_TRUE(WriteFile("Assets/日本語 Folder/Child.txt"));

    for (const std::string relative : { "Assets", "Assets/日本語 Folder/" }) {
        const auto target = editor::ResolveAssetBrowserExplorerTarget(Root(), relative);
        ASSERT_FALSE(target.path.empty());
        EXPECT_TRUE(target.isDirectory);
        EXPECT_TRUE(util::FileSystem::SamePathText(target.path, Path(relative)));
    }
}

TEST_F(AssetBrowserExplorerTest, PreservesExternalAbsoluteMountInsteadOfMatchingProjectAssets)
{
    ASSERT_TRUE(WriteFile("Assets/日本語 Folder/File.txt"));
    const std::string external = Path("../Other Project/Assets/日本語 Folder/File.txt");
    const auto externalPath = util::FileSystem::PathFromUtf8(external);
    std::error_code error;
    std::filesystem::create_directories(externalPath.parent_path(), error);
    ASSERT_FALSE(error);
    ASSERT_TRUE(util::FileSystem::WriteText(externalPath, "external\n"));

    const auto target = editor::ResolveAssetBrowserExplorerTarget(Root(), external);

    EXPECT_TRUE(util::FileSystem::SamePathText(target.path,
        util::FileSystem::PathToUtf8(externalPath.lexically_normal())));
    EXPECT_FALSE(util::FileSystem::SamePathText(target.path, Path("Assets/日本語 Folder/File.txt")));
}

TEST_F(AssetBrowserExplorerTest, PreservesImportedCacheFileForAbsoluteAndProjectRelativeSelections)
{
    const std::string relative = "Library/Baked/0123456789abcdef0123456789abcdef/anims/待機 Idle.anim";
    ASSERT_TRUE(WriteFile(relative));

    const auto relativeTarget = editor::ResolveAssetBrowserExplorerTarget(Root(), relative);
    const auto absoluteTarget = editor::ResolveAssetBrowserExplorerTarget(Root(), Path(relative));

    EXPECT_TRUE(util::FileSystem::SamePathText(relativeTarget.path, Path(relative)));
    EXPECT_EQ(relativeTarget.path, absoluteTarget.path);
    EXPECT_FALSE(relativeTarget.isDirectory);
}

TEST_F(AssetBrowserExplorerTest, ResolvesVirtualMeshAndSpriteToTheirSourceFiles)
{
    ASSERT_TRUE(WriteFile("Assets/Models/日本語 Model.fbx"));
    ASSERT_TRUE(WriteFile("Assets/Images/日本語 Image.png"));

    const auto mesh = editor::ResolveAssetBrowserExplorerTarget(
        Root(), Path("Assets/Models/日本語 Model.fbx") + "::mesh::2");
    const auto sprite = editor::ResolveAssetBrowserExplorerTarget(
        Root(), "Assets/Images/日本語 Image.png::sprite::Sprite 1");

    EXPECT_TRUE(util::FileSystem::SamePathText(mesh.path, Path("Assets/Models/日本語 Model.fbx")));
    EXPECT_TRUE(util::FileSystem::SamePathText(sprite.path, Path("Assets/Images/日本語 Image.png")));
    EXPECT_FALSE(mesh.isDirectory);
    EXPECT_FALSE(sprite.isDirectory);
}

TEST_F(AssetBrowserExplorerTest, UsesIndexedGuidAfterMoveAndIgnoresExistingStaleHint)
{
    ASSERT_TRUE(WriteFile("Assets/Original.prefab"));
    asset::AssetDatabase::Init(Path("Assets"));
    const std::string guid = asset::AssetDatabase::TryGetGuidFromPath(Path("Assets/Original.prefab"));
    ASSERT_FALSE(guid.empty());
    const auto before = util::FileSystem::PathFromUtf8(Path("Assets/Original.prefab"));
    const auto after = util::FileSystem::PathFromUtf8(Path("Assets/移動 Renamed.prefab"));
    ASSERT_TRUE(util::FileSystem::Rename(before, after));
    ASSERT_TRUE(util::FileSystem::Rename(
        util::FileSystem::PathFromUtf8(Path("Assets/Original.prefab.meta")),
        util::FileSystem::PathFromUtf8(Path("Assets/移動 Renamed.prefab.meta"))));
    asset::AssetDatabase::OnAssetMoved(
        util::FileSystem::PathToUtf8(before), util::FileSystem::PathToUtf8(after));
    ASSERT_TRUE(WriteFile("Assets/Original.prefab"));

    const auto target = editor::ResolveAssetBrowserExplorerTarget(
        Root(), "guid:" + guid + "|Assets/Original.prefab");

    EXPECT_TRUE(util::FileSystem::SamePathText(target.path, util::FileSystem::PathToUtf8(after)));
    EXPECT_TRUE(editor::ResolveAssetBrowserExplorerTarget(
        Root(), "guid:ffffffffffffffffffffffffffffffff|Assets/Original.prefab").path.empty());
}

TEST_F(AssetBrowserExplorerTest, RejectsMissingEmptyAndUnanchoredTargets)
{
    ASSERT_TRUE(WriteFile("Assets/Existing.txt"));
    std::string embeddedNull = "Assets/Existing.txt";
    embeddedNull.push_back('\0');
    embeddedNull += "ignored";

    EXPECT_TRUE(editor::ResolveAssetBrowserExplorerTarget(Root(), "Assets/Missing.txt").path.empty());
    EXPECT_TRUE(editor::ResolveAssetBrowserExplorerTarget(Root(), "").path.empty());
    EXPECT_TRUE(editor::ResolveAssetBrowserExplorerTarget("", "Assets/Existing.txt").path.empty());
    EXPECT_TRUE(editor::ResolveAssetBrowserExplorerTarget(Root(), "C:Assets/Existing.txt").path.empty());
    EXPECT_TRUE(editor::ResolveAssetBrowserExplorerTarget(Root(), "\\Assets\\Existing.txt").path.empty());
    EXPECT_TRUE(editor::ResolveAssetBrowserExplorerTarget(Root(), embeddedNull).path.empty());
    EXPECT_TRUE(util::FileSystem::Exists(Path("Assets/Existing.txt")));
    EXPECT_FALSE(util::FileSystem::Exists(Path("Assets/Existing.txt.meta")));
}

} /// @note namespace fbzz::tests
