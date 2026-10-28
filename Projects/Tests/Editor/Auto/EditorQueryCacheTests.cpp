/// @file    EditorQueryCacheTests.cpp
/// @brief   シーンの公開値・階層・順序・寿命とアセット索引の変更を検索へ反映する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Util/AssetQueryCache.hpp>
#include <Editor/Util/SceneQueryIndex.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <array>
#include <fstream>
#include <span>
#include <string_view>
#include <system_error>

namespace fbzz::tests {
namespace {
class EditorQueryCacheTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        AttachScene();
        asset::AssetDatabase::Init(util::FileSystem::PathToUtf8(File("Assets")));
    }
    void TearDown() override
    {
        asset::AssetDatabase::Shutdown();
        EditorFixture::TearDown();
    }
    static bool WritePrefab(const std::filesystem::path& path, std::string_view guid)
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        const auto absolute = util::FileSystem::PathToUtf8(path);
        return !error && util::FileSystem::WriteText(path, "version = 1\n")
            && util::FileSystem::WriteText(absolute + ".meta",
                "[meta]\nguid = \"" + std::string(guid) + "\"\n");
    }
    editor::SceneQueryIndex m_index;
};

TEST_F(EditorQueryCacheTest, DirectRenameAndSceneOrderUpdateNameResults)
{
    auto& source = *AttachedScene();
    const auto first = source.CreateGameObject("Player A").GetID();
    const auto second = source.CreateGameObject("Player B").GetID();
    m_index.Refresh(source, 1);
    const auto revision = m_index.NameRevision();
    EXPECT_EQ(m_index.FindNames("player", 1), (std::vector<scene::EntityID>{first}));
    m_index.Refresh(source, 2);
    EXPECT_EQ(m_index.NameRevision(), revision);
    source.GetGameObject(first)->name = "Camera";
    m_index.Refresh(source, 3);
    EXPECT_EQ(m_index.FindNames("player", 100), (std::vector<scene::EntityID>{second}));
    source.GetGameObject(first)->name = "Player A";
    ASSERT_TRUE(source.MoveGameObjectToIndex(second, 0));
    m_index.Refresh(source, 4);
    EXPECT_EQ(m_index.FindNames("PLAYER", 1), (std::vector<scene::EntityID>{second}));
    EXPECT_TRUE(m_index.FindNames("player", 0).empty());
}

TEST_F(EditorQueryCacheTest, PrefabCountsMatchAuthoritativeRootsAfterLinkAndHierarchyChanges)
{
    auto& source = *AttachedScene();
    const auto root = source.CreateGameObject("Root").GetID();
    const auto child = source.CreateGameObject("Child").GetID();
    const auto other = source.CreateGameObject("Other").GetID();
    source.GetGameObject(root)->prefabAssetPath = "Assets/Tree.prefab";
    source.GetGameObject(child)->prefabAssetPath = "assets\\TREE.prefab";
    source.GetGameObject(other)->prefabAssetPath = "Assets/Other.prefab";
    ASSERT_TRUE(source.GetGameObject(child)->SetParent(source.GetGameObject(root)));
    m_index.Refresh(source, 1);
    EXPECT_EQ(m_index.CountPrefabInstances("Assets/Tree.prefab"),
        editor::PrefabSerializer::CountInstances(source, "Assets/Tree.prefab"));
    EXPECT_EQ(m_index.CountPrefabInstances("Assets/Tree.prefab"), 1);
    ASSERT_TRUE(source.GetGameObject(child)->ClearParent());
    m_index.Refresh(source, 2);
    EXPECT_EQ(m_index.CountPrefabInstances("Assets/Tree.prefab"), 2);
    source.GetGameObject(other)->prefabAssetPath = "Assets/Tree.prefab";
    m_index.Refresh(source, 3);
    EXPECT_EQ(m_index.CountPrefabInstances("Assets/Tree.prefab"), 3);
    ASSERT_TRUE(source.DestroyGameObject(child));
    m_index.Refresh(source, 4);
    EXPECT_EQ(m_index.CountPrefabInstances("Assets/Tree.prefab"), 2);
}

TEST_F(EditorQueryCacheTest, SceneClearAndReusedEntitySlotsDoNotReturnOldResults)
{
    auto& source = *AttachedScene();
    const auto old = source.CreateGameObject("Old").GetID();
    m_index.Refresh(source, 1);
    ASSERT_TRUE(source.DestroyGameObject(old));
    const auto fresh = source.CreateGameObject("Fresh").GetID();
    m_index.Refresh(source, 2);
    EXPECT_TRUE(m_index.FindNames("Old", 100).empty());
    EXPECT_EQ(m_index.FindNames("Fresh", 100), (std::vector<scene::EntityID>{fresh}));
    source.Clear();
    m_index.Refresh(source, 2);
    EXPECT_TRUE(m_index.FindNames("Fresh", 100).empty());
}

TEST_F(EditorQueryCacheTest, AssetCacheReusesOwnedResultsAndUpdatesAfterIndexRebuild)
{
    std::filesystem::create_directories(File("Assets"));
    { std::ofstream output(File("Assets/Player.mat")); output << "material"; }
    const auto root = util::FileSystem::PathToUtf8(ProjectRoot());
    editor::AssetSearch::SetProjectRoot(root);
    editor::AssetQueryCache cache;
    ASSERT_TRUE(cache.Update("Player", {}, 200));
    ASSERT_EQ(cache.Results().size(), 1u);
    EXPECT_FALSE(cache.Update("Player", {}, 200));
    const auto originalName = cache.Results()[0].entry.filename;
    { std::ofstream output(File("Assets/PlayerNew.mat")); output << "new"; }
    editor::AssetSearch::Rebuild();
    EXPECT_EQ(cache.Results()[0].entry.filename, originalName);
    ASSERT_TRUE(cache.Update("Player", {}, 200));
    EXPECT_EQ(cache.Results().size(), 2u);
    const std::array<std::string, 1> extensions{{".scene"}};
    ASSERT_TRUE(cache.Update("Player", extensions, 200));
    EXPECT_TRUE(cache.Results().empty());
}

TEST_F(EditorQueryCacheTest, AssetCacheOwnsQueryAndExtensionInputsFromItsPreviousResults)
{
    const auto path = File("Assets/PlayerWithANameLongEnoughForOwnedAllocation.mat");
    ASSERT_TRUE(util::FileSystem::WriteText(path, "material"));
    editor::AssetSearch::SetProjectRoot(util::FileSystem::PathToUtf8(ProjectRoot()));
    editor::AssetQueryCache cache;
    ASSERT_TRUE(cache.Update("Player", {}, 200));
    ASSERT_EQ(cache.Results().size(), 1u);
    const auto expectedName = cache.Results()[0].entry.filename;
    const std::string_view query = cache.Results()[0].entry.filename;
    const std::span<const std::string> extensions{&cache.Results()[0].entry.extension, 1};

    ASSERT_TRUE(cache.Update(query, extensions, 17));

    ASSERT_EQ(cache.Results().size(), 1u);
    EXPECT_EQ(cache.Results()[0].entry.filename, expectedName);
    EXPECT_EQ(cache.Results()[0].entry.extension, ".mat");
}

TEST_F(EditorQueryCacheTest, PrefabFileRegistrationRefreshesTheSameFrameWithoutRenamingObjects)
{
    constexpr std::string_view GUID = "11111111111111111111111111111111";
    const auto path = File("Assets/Tree.prefab");
    const auto absolute = util::FileSystem::PathToUtf8(path);
    auto& source = *AttachedScene();
    auto& object = source.CreateGameObject("Tree");
    object.prefabAssetPath = absolute;
    m_index.Refresh(source, 1);
    const auto nameRevision = m_index.NameRevision();
    EXPECT_EQ(m_index.CountPrefabInstances(absolute), 1);
    const auto revision = asset::AssetDatabase::Revision();

    ASSERT_TRUE(WritePrefab(path, GUID));
    ASSERT_EQ(asset::AssetDatabase::TryGetGuidFromPath(absolute), GUID);
    ASSERT_GT(asset::AssetDatabase::Revision(), revision);
    m_index.Refresh(source, 1);

    EXPECT_EQ(m_index.CountPrefabInstances("guid:" + std::string(GUID)), 1);
    EXPECT_EQ(m_index.CountPrefabInstances(absolute), 1);
    EXPECT_EQ(m_index.NameRevision(), nameRevision);
}

TEST_F(EditorQueryCacheTest, FirstPrefabCountRegistersAJustCreatedFileWithoutAnEarlierRefresh)
{
    constexpr std::string_view GUID = "22222222222222222222222222222222";
    const auto path = File("Assets/Tree.prefab");
    const auto absolute = util::FileSystem::PathToUtf8(path);
    auto& source = *AttachedScene();
    auto& object = source.CreateGameObject("Tree");
    object.prefabAssetPath = absolute;
    m_index.Refresh(source, 1);
    const auto nameRevision = m_index.NameRevision();
    const auto assetRevision = asset::AssetDatabase::Revision();
    ASSERT_TRUE(WritePrefab(path, GUID));
    ASSERT_EQ(asset::AssetDatabase::Revision(), assetRevision);

    EXPECT_EQ(m_index.CountPrefabInstances(absolute), 1);

    EXPECT_GT(asset::AssetDatabase::Revision(), assetRevision);
    EXPECT_EQ(m_index.CountPrefabInstances("guid:" + std::string(GUID)), 1);
    EXPECT_EQ(m_index.NameRevision(), nameRevision);
}

TEST_F(EditorQueryCacheTest, LazyRegistrationDuringRefreshCommitsTheFinalAssetRevision)
{
    constexpr std::string_view GUID = "33333333333333333333333333333333";
    const auto path = File("Assets/Tree.prefab");
    const auto absolute = util::FileSystem::PathToUtf8(path);
    auto& source = *AttachedScene();
    auto& object = source.CreateGameObject("Tree");
    object.prefabAssetPath = absolute;
    m_index.Refresh(source, 1);
    const auto nameRevision = m_index.NameRevision();
    const auto assetRevision = asset::AssetDatabase::Revision();
    ASSERT_TRUE(WritePrefab(path, GUID));
    asset::AssetManager::BumpAssetGeneration();

    m_index.Refresh(source, 1);

    EXPECT_GT(asset::AssetDatabase::Revision(), assetRevision);
    EXPECT_EQ(m_index.CountPrefabInstances("guid:" + std::string(GUID)), 1);
    EXPECT_EQ(m_index.NameRevision(), nameRevision);
    object.name = "Changed after refresh";
    m_index.Refresh(source, 1);
    EXPECT_EQ(m_index.FindNames("Tree", 100), (std::vector<scene::EntityID>{object.GetID()}));
    EXPECT_TRUE(m_index.FindNames("Changed", 100).empty());
    m_index.Refresh(source, 2);
    EXPECT_EQ(m_index.FindNames("Changed", 100), (std::vector<scene::EntityID>{object.GetID()}));
}

TEST_F(EditorQueryCacheTest, GuidReassignmentRekeysUnchangedPrefabPathsInTheSameFrame)
{
    constexpr std::string_view GUID = "44444444444444444444444444444444";
    const auto path = File("Assets/Tree.prefab");
    const auto absolute = util::FileSystem::PathToUtf8(path);
    ASSERT_TRUE(WritePrefab(path, GUID));
    ASSERT_EQ(asset::AssetDatabase::TryGetGuidFromPath(absolute), GUID);
    auto& source = *AttachedScene();
    source.CreateGameObject("Tree").prefabAssetPath = absolute;
    m_index.Refresh(source, 1);
    const auto nameRevision = m_index.NameRevision();
    const auto revision = asset::AssetDatabase::Revision();
    std::string replacement;

    ASSERT_TRUE(asset::AssetDatabase::ReassignGuid(absolute, replacement));
    ASSERT_NE(replacement, GUID);
    ASSERT_GT(asset::AssetDatabase::Revision(), revision);
    m_index.Refresh(source, 1);

    EXPECT_EQ(m_index.CountPrefabInstances("guid:" + replacement), 1);
    EXPECT_EQ(m_index.CountPrefabInstances("guid:" + std::string(GUID)), 0);
    EXPECT_EQ(m_index.CountPrefabInstances(absolute), 1);
    EXPECT_EQ(m_index.NameRevision(), nameRevision);
}

TEST_F(EditorQueryCacheTest, ProjectSwitchRekeysRelativePrefabPathsWithoutReplacingTheScene)
{
    constexpr std::string_view FIRST_GUID = "55555555555555555555555555555555";
    constexpr std::string_view SECOND_GUID = "66666666666666666666666666666666";
    ASSERT_TRUE(WritePrefab(File("Assets/Tree.prefab"), FIRST_GUID));
    ASSERT_TRUE(WritePrefab(File("ProjectB/Assets/Tree.prefab"), SECOND_GUID));
    auto& source = *AttachedScene();
    source.CreateGameObject("Tree").prefabAssetPath = "Assets/Tree.prefab";
    m_index.Refresh(source, 1);
    ASSERT_EQ(m_index.CountPrefabInstances("guid:" + std::string(FIRST_GUID)), 1);
    const auto nameRevision = m_index.NameRevision();
    const auto sceneGeneration = source.GetRenderSceneGeneration();

    asset::AssetDatabase::Init(util::FileSystem::PathToUtf8(File("ProjectB/Assets")));
    m_index.Refresh(source, 1);

    EXPECT_EQ(source.GetRenderSceneGeneration(), sceneGeneration);
    EXPECT_EQ(m_index.CountPrefabInstances("guid:" + std::string(SECOND_GUID)), 1);
    EXPECT_EQ(m_index.CountPrefabInstances("guid:" + std::string(FIRST_GUID)), 0);
    EXPECT_EQ(m_index.CountPrefabInstances("Assets/Tree.prefab"), 1);
    EXPECT_EQ(m_index.NameRevision(), nameRevision);
}

TEST_F(EditorQueryCacheTest, SharedRegistrationAdvancesResolutionRevisionButRepeatedReadsDoNot)
{
    constexpr std::string_view GUID = "77777777777777777777777777777777";
    const auto sharedRoot = File("Shared/Assets");
    std::error_code error;
    std::filesystem::create_directories(sharedRoot, error);
    ASSERT_FALSE(error);
    asset::AssetDatabase::Init(util::FileSystem::PathToUtf8(File("Assets")),
                              util::FileSystem::PathToUtf8(sharedRoot));
    const auto path = sharedRoot / "Tree.prefab";
    ASSERT_TRUE(WritePrefab(path, GUID));
    const auto absolute = util::FileSystem::PathToUtf8(path);
    const auto revision = asset::AssetDatabase::Revision();

    ASSERT_EQ(asset::AssetDatabase::TryGetGuidFromPath(absolute), GUID);

    EXPECT_GT(asset::AssetDatabase::Revision(), revision);
    EXPECT_EQ(asset::AssetDatabase::PathFromGuid(std::string(GUID)), absolute);
    const auto registeredRevision = asset::AssetDatabase::Revision();
    EXPECT_EQ(asset::AssetDatabase::GuidFromPath(absolute), GUID);
    EXPECT_EQ(asset::AssetDatabase::TryGetGuidFromPath(absolute), GUID);
    EXPECT_EQ(asset::AssetDatabase::Revision(), registeredRevision);
}

TEST_F(EditorQueryCacheTest, RegistrationMoveRemovalAndShutdownAdvanceOnlyWhenResolutionChanges)
{
    constexpr std::string_view GUID = "88888888888888888888888888888888";
    const auto before = File("Assets/Before.prefab");
    const auto after = File("Assets/After.prefab");
    ASSERT_TRUE(WritePrefab(before, GUID));
    const auto revision = asset::AssetDatabase::Revision();
    const auto beforePath = util::FileSystem::PathToUtf8(before);
    const auto afterPath = util::FileSystem::PathToUtf8(after);
    ASSERT_EQ(asset::AssetDatabase::GuidFromPath(beforePath), GUID);
    ASSERT_GT(asset::AssetDatabase::Revision(), revision);
    auto currentRevision = asset::AssetDatabase::Revision();
    EXPECT_EQ(asset::AssetDatabase::GuidFromPath(beforePath), GUID);
    asset::AssetDatabase::OnAssetRemoved(util::FileSystem::PathToUtf8(File("Assets/Missing.prefab")));
    EXPECT_EQ(asset::AssetDatabase::Revision(), currentRevision);
    std::error_code error;
    std::filesystem::rename(before, after, error);
    ASSERT_FALSE(error);
    std::filesystem::rename(beforePath + ".meta", afterPath + ".meta", error);
    ASSERT_FALSE(error);

    asset::AssetDatabase::OnAssetMoved(beforePath, afterPath);

    ASSERT_GT(asset::AssetDatabase::Revision(), currentRevision);
    EXPECT_EQ(asset::AssetDatabase::PathFromGuid(std::string(GUID)), afterPath);
    EXPECT_EQ(asset::AssetDatabase::TryGetGuidFromPath(beforePath), GUID);
    currentRevision = asset::AssetDatabase::Revision();
    std::filesystem::remove(after, error);
    ASSERT_FALSE(error);
    std::filesystem::remove(afterPath + ".meta", error);
    ASSERT_FALSE(error);
    asset::AssetDatabase::OnAssetRemoved(afterPath);
    EXPECT_GT(asset::AssetDatabase::Revision(), currentRevision);
    EXPECT_TRUE(asset::AssetDatabase::PathFromGuid(std::string(GUID)).empty());
    EXPECT_TRUE(asset::AssetDatabase::TryGetGuidFromPath(beforePath).empty());
    currentRevision = asset::AssetDatabase::Revision();
    asset::AssetDatabase::Shutdown();
    EXPECT_GT(asset::AssetDatabase::Revision(), currentRevision);
    currentRevision = asset::AssetDatabase::Revision();
    asset::AssetDatabase::Shutdown();
    EXPECT_EQ(asset::AssetDatabase::Revision(), currentRevision);
}
} /// @note namespace
} /// @note namespace fbzz::tests
