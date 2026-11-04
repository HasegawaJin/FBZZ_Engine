/// @file    AssetFileOperatorTests.cpp
/// @brief   ファイル操作のGUID・サイドカー・境界・上書き拒否を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <fstream>

namespace fbzz::tests {
class AssetFileOperatorTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp(); Context().projectRoot = ProjectRoot().generic_string();
        std::ofstream(File("Assets/Color.png")) << "source";
        asset::AssetDatabase::Init(File("Assets").generic_string()); editor::RegisterAssetFileOperators(m_registry);
    }
    void TearDown() override
    {
        for (const auto& path : m_dirtyPaths) editor::AssetDirtyRegistry::MarkClean(path);
        m_registry.Clear(); asset::AssetDatabase::Shutdown(); EditorFixture::TearDown();
    }
    editor::OpResult Invoke(const char* id, const std::string& path, const std::string& destination = {})
    {
        editor::OpArgs args; args.Set("path", path); if (!destination.empty()) args.Set("destination", destination);
        editor::OpContext context{Context(), m_undo}; return m_registry.Invoke(id, context, args);
    }
    editor::OperatorRegistry m_registry; editor::UndoStack m_undo; std::vector<std::string> m_dirtyPaths;
};
TEST_F(AssetFileOperatorTest, MoveKeepsGuidAndItsImportSettings)
{
    const std::string original = File("Assets/Color.png").generic_string();
    const std::string guid = asset::AssetDatabase::GuidFromPath(original);
    asset::TextureAsset texture; texture.settings.maxSize = 512;
    ASSERT_TRUE(asset::TexDescSerializer{}.Save(texture, original + ".meta"));
    const auto result = Invoke("asset.move", "guid:" + guid + "|Assets/Color.png", "Assets/Moved.png");
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_EQ(asset::AssetDatabase::PathFromGuid(guid), File("Assets/Moved.png").generic_string());
    EXPECT_FALSE(std::filesystem::exists(File("Assets/Color.png.meta")));
    asset::TextureAsset moved; ASSERT_TRUE(asset::TexDescSerializer{}.Load(File("Assets/Moved.png.meta").generic_string(), moved));
    EXPECT_EQ(moved.settings.maxSize, 512u);
}
TEST_F(AssetFileOperatorTest, DuplicateAssignsAnotherGuidAndPreservesImportSettings)
{
    const std::string original = File("Assets/Color.png").generic_string();
    const std::string guid = asset::AssetDatabase::GuidFromPath(original);
    asset::TextureAsset texture; texture.settings.maxSize = 512;
    ASSERT_TRUE(asset::TexDescSerializer{}.Save(texture, original + ".meta"));
    const auto result = Invoke("asset.duplicate", "Assets/Color.png", "Assets/Copy.png"); ASSERT_TRUE(result.ok);
    const std::string copy = File("Assets/Copy.png").generic_string();
    EXPECT_NE(asset::AssetDatabase::GuidFromPath(copy), guid);
    asset::TextureAsset loaded; ASSERT_TRUE(asset::TexDescSerializer{}.Load(copy + ".meta", loaded));
    EXPECT_EQ(loaded.settings.maxSize, 512u);
}
TEST_F(AssetFileOperatorTest, ExistingDestinationSidecarPreventsPartialMove)
{
    std::ofstream(File("Assets/Destination.png.meta")) << "reserved";
    EXPECT_FALSE(Invoke("asset.move", "Assets/Color.png", "Assets/Destination.png").ok);
    EXPECT_TRUE(std::filesystem::exists(File("Assets/Color.png")));
    EXPECT_FALSE(std::filesystem::exists(File("Assets/Destination.png")));
}
TEST_F(AssetFileOperatorTest, TrashKeepsBodyAndGuidTogetherAndRemovesTheLiveIndex)
{
    const std::string guid = asset::AssetDatabase::GuidFromPath(File("Assets/Color.png").generic_string());
    const auto result = Invoke("asset.trash", "Assets/Color.png"); ASSERT_TRUE(result.ok);
    const auto trash = util::FileSystem::PathFromUtf8(result.data.Find("trashRoot")->AsString());
    EXPECT_TRUE(std::filesystem::exists(trash / "0_Color.png"));
    EXPECT_TRUE(std::filesystem::exists(trash / "0_Color.png.meta"));
    EXPECT_TRUE(asset::AssetDatabase::PathFromGuid(guid).empty());
    EXPECT_FALSE(std::filesystem::exists(File("Assets/Color.png")));
}
TEST_F(AssetFileOperatorTest, DirtyAssetsAndProjectRootCannotBeMovedOrTrashed)
{
    const std::string original = File("Assets/Color.png").generic_string();
    editor::AssetDirtyRegistry::Register(original, "Assets/Color.png", "Texture", [] { return false; }); m_dirtyPaths.push_back(original);
    EXPECT_FALSE(Invoke("asset.move", "Assets/Color.png", "Assets/Moved.png").ok);
    EXPECT_FALSE(Invoke("asset.trash", "Assets").ok);
    EXPECT_FALSE(Invoke("asset.move", "Assets/Color.png", "Assets/../Outside.png").ok);
    EXPECT_TRUE(std::filesystem::exists(File("Assets/Color.png")));
}
TEST_F(AssetFileOperatorTest, CreationDoesNotOverwriteExistingAssetsOrCreateWrongFormats)
{
    editor::OpArgs args; args.Set("path", std::string("Assets/Test.sequence")); args.Set("kind", std::string("sequence"));
    editor::OpContext context{Context(), m_undo}; ASSERT_TRUE(m_registry.Invoke("asset.create", context, args).ok);
    EXPECT_FALSE(m_registry.Invoke("asset.create", context, args).ok);
    args.Set("path", std::string("Assets/Wrong.png")); EXPECT_FALSE(m_registry.Invoke("asset.create", context, args).ok);
    EXPECT_FALSE(std::filesystem::exists(File("Assets/Wrong.png")));
}
TEST_F(AssetFileOperatorTest, HlslCreationRejectsTraversalAndKeepsExistingSource)
{
    editor::OpContext context{Context(), m_undo}; editor::OpArgs args; args.Set("name", std::string("SmokeCompute")); args.Set("kind", std::string("compute"));
    const auto created = m_registry.Invoke("asset.hlsl.create", context, args); ASSERT_TRUE(created.ok);
    EXPECT_TRUE(std::filesystem::exists(File("Assets/Shaders/PostProcess/Custom/SmokeCompute.cs.hlsl")));
    EXPECT_FALSE(m_registry.Invoke("asset.hlsl.create", context, args).ok);
    args.Set("name", std::string("../Outside")); EXPECT_FALSE(m_registry.Invoke("asset.hlsl.create", context, args).ok);
}
} /// @note namespace fbzz::tests
