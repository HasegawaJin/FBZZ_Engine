/// @file    WorldPartitionManifestTests.cpp
/// @brief   Persistent cell identity, payload confinement, atomic replacement and strict world manifests.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <TestKit/TestKit.hpp>
#include <Engine/Scene/WorldPartitionManifest.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace fbzz::tests {
class WorldPartitionManifestTest : public testkit::Fixture {
protected:
    scene::WorldPartitionManifest Example() const
    {
        scene::WorldPartitionManifest manifest;
        manifest.grid.origin = {128, -20, -256};
        manifest.grid.cellSize = 64;
        manifest.revision = (std::numeric_limits<uint64_t>::max)();
        manifest.sourceInstanceIds = {"source-z", "source-a"};
        scene::WorldPartitionCell cell;
        cell.id = {-2, 3};
        cell.bounds = {{-5, -50, -70}, {75, 120, 15}};
        cell.revision = (std::numeric_limits<uint64_t>::max)() - 1;
        cell.nearPath = "Assets/World/Cells/-2_3.scene";
        cell.hlodPath = "Assets/World/HLOD/-2_3.scene";
        cell.dependencies = {{"Model", "guid:0123456789abcdef0123456789abcdef|Assets/Models/Tree.fbx"},
            {"MaterialAsset", "Assets/Materials/Tree.mat"}};
        manifest.cells.push_back(std::move(cell));
        return manifest;
    }

    std::string Minimal(std::string_view row = {}) const
    {
        std::string text = "format = 'fbzz.worldpartition'\nversion = 1\nrevision = '1'\n"
            "[grid]\norigin = [0,0,0]\ncellSize = 64\n";
        text += row;
        return text;
    }

    std::string CellRow(std::string_view id = "[0,0]") const
    {
        return "[[cells]]\nid = " + std::string(id) + "\nboundsMin = [0,0,0]\nboundsMax = [64,10,64]\n"
            "revision = '1'\nnearPath = 'Assets/World/0_0.scene'\nhlodPath = ''\ndependencies = []\n";
    }
};

TEST_F(WorldPartitionManifestTest, RoundTripsNegativeCoordinatesFullRevisionsAndDependencies)
{
    const auto source = Example();
    std::string text, diagnostic;
    ASSERT_TRUE(scene::SerializeWorldPartitionManifest(source, text, diagnostic)) << diagnostic;
    EXPECT_TRUE(diagnostic.empty());
    scene::WorldPartitionManifest restored;
    ASSERT_TRUE(scene::DeserializeWorldPartitionManifest(text, restored, diagnostic)) << diagnostic;
    EXPECT_EQ(restored.revision, source.revision);
    EXPECT_EQ(restored.sourceInstanceIds, std::vector<std::string>({"source-a", "source-z"}));
    EXPECT_EQ(restored.grid.cellSize, 64);
    EXPECT_VEC3_NEAR(restored.grid.origin, source.grid.origin, 0.0001f);
    ASSERT_EQ(restored.cells.size(), 1u);
    const auto& cell = restored.cells.front();
    EXPECT_EQ(cell.id, source.cells.front().id);
    EXPECT_EQ(cell.revision, source.cells.front().revision);
    EXPECT_EQ(cell.nearPath, source.cells.front().nearPath);
    EXPECT_EQ(cell.hlodPath, source.cells.front().hlodPath);
    EXPECT_VEC3_NEAR(cell.bounds.min, source.cells.front().bounds.min, 0.0001f);
    EXPECT_VEC3_NEAR(cell.bounds.max, source.cells.front().bounds.max, 0.0001f);
    ASSERT_EQ(cell.dependencies.size(), 2u);
    EXPECT_EQ(cell.dependencies[0].typeName, "MaterialAsset");
    EXPECT_EQ(cell.dependencies[1].reference, source.cells.front().dependencies[0].reference);
    std::string again;
    ASSERT_TRUE(scene::SerializeWorldPartitionManifest(restored, again, diagnostic));
    EXPECT_EQ(again, text);
}

TEST_F(WorldPartitionManifestTest, CanonicalOrderingDoesNotDependOnCellOrDependencyInsertionOrder)
{
    auto first = Example();
    auto cell = first.cells.front();
    cell.id = {4, -1};
    cell.nearPath = "Assets/World/Cells/4_-1.scene";
    cell.hlodPath.clear();
    first.cells.push_back(cell);
    auto second = first;
    std::reverse(second.cells.begin(), second.cells.end());
    for (auto& entry : second.cells) std::reverse(entry.dependencies.begin(), entry.dependencies.end());
    std::string a, b, diagnostic;
    ASSERT_TRUE(scene::SerializeWorldPartitionManifest(first, a, diagnostic));
    ASSERT_TRUE(scene::SerializeWorldPartitionManifest(second, b, diagnostic));
    EXPECT_EQ(a, b);
    EXPECT_EQ(first.cells[0].id, scene::WorldCellId(-2, 3));
}

TEST_F(WorldPartitionManifestTest, RejectsTraversalAbsolutePathsAndWindowsAliasesWithoutChangingOutput)
{
    const std::vector<std::string> badPaths = {"../outside.scene", "Assets/../outside.scene", "Assets/./World/cell.scene",
        "C:/Project/Assets/cell.scene", "//server/share/cell.scene", "Assets\\World\\cell.scene", "Assets//World/cell.scene",
        "Assets/World/", "Assets/World/cell.scene.", "Assets/World/cell.scene ", "Assets/World/NUL.scene",
        "Assets/World/COM1.scene", "Assets/World/cell:alternate.scene", std::string("Assets/a\0b.scene", 16),
        std::string("Assets/World/\xc0\xaf.scene")};
    for (const auto& path : badPaths) {
        auto source = Example();
        source.cells.front().nearPath = path;
        std::string output = "previous output", diagnostic;
        EXPECT_FALSE(scene::SerializeWorldPartitionManifest(source, output, diagnostic)) << path;
        EXPECT_EQ(output, "previous output");
        EXPECT_FALSE(diagnostic.empty());
    }
}

TEST_F(WorldPartitionManifestTest, RejectsDuplicateCoordinatesAndPayloadAliases)
{
    auto source = Example();
    std::string diagnostic;
    source.cells.push_back(source.cells.front());
    EXPECT_FALSE(scene::ValidateWorldPartitionManifest(source, diagnostic));
    source.cells.back().id = {8, 2};
    source.cells.back().nearPath = "Assets/world/cells/-2_3.SCENE";
    source.cells.back().hlodPath.clear();
    EXPECT_FALSE(scene::ValidateWorldPartitionManifest(source, diagnostic));
    source = Example();
    source.cells.front().hlodPath = source.cells.front().nearPath;
    EXPECT_FALSE(scene::ValidateWorldPartitionManifest(source, diagnostic));
}

TEST_F(WorldPartitionManifestTest, SourceForestIdentitiesAreOptionalUniqueAndTransactional)
{
    auto source = Example();
    std::string diagnostic;
    source.sourceInstanceIds.push_back(source.sourceInstanceIds.front());
    EXPECT_FALSE(scene::ValidateWorldPartitionManifest(source, diagnostic));
    source = Example();
    source.sourceInstanceIds.push_back("");
    EXPECT_FALSE(scene::ValidateWorldPartitionManifest(source, diagnostic));
    source = Example();
    source.sourceInstanceIds.push_back("invalid\nidentity");
    EXPECT_FALSE(scene::ValidateWorldPartitionManifest(source, diagnostic));
    const std::string text = "sourceInstanceIds = ['duplicate','duplicate']\n" + Minimal(CellRow());
    auto output = Example();
    EXPECT_FALSE(scene::DeserializeWorldPartitionManifest(text, output, diagnostic));
    EXPECT_EQ(output.sourceInstanceIds, Example().sourceInstanceIds);
    ASSERT_TRUE(scene::DeserializeWorldPartitionManifest(Minimal(CellRow()), output, diagnostic));
    EXPECT_TRUE(output.sourceInstanceIds.empty());
}

TEST_F(WorldPartitionManifestTest, RejectsNonfiniteAndUnrepresentableGridAndBounds)
{
    auto source = Example();
    std::string diagnostic;
    source.grid.cellSize = 0;
    EXPECT_FALSE(scene::ValidateWorldPartitionManifest(source, diagnostic));
    source = Example();
    source.grid.origin.z = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(scene::ValidateWorldPartitionManifest(source, diagnostic));
    source = Example();
    source.cells.front().bounds.min.y = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(scene::ValidateWorldPartitionManifest(source, diagnostic));
    source = Example();
    source.cells.front().bounds.max.x = source.cells.front().bounds.min.x;
    EXPECT_FALSE(scene::ValidateWorldPartitionManifest(source, diagnostic));
    source = Example();
    source.cells.front().bounds.min.y = -(std::numeric_limits<float>::max)();
    source.cells.front().bounds.max.y = (std::numeric_limits<float>::max)();
    EXPECT_FALSE(scene::ValidateWorldPartitionManifest(source, diagnostic));
    source = Example();
    source.cells.front().id.x = (std::numeric_limits<int32_t>::max)();
    EXPECT_FALSE(scene::ValidateWorldPartitionManifest(source, diagnostic));
}

TEST_F(WorldPartitionManifestTest, RejectsUnsupportedMalformedAndOutOfRangeDocumentsTransactionally)
{
    const std::vector<std::string> invalid = {
        "format = 'fbzz.worldpartition'\nversion = 2\n",
        "format = 'fbzz.worldpartition'\nversion = 1\nrevision = '-1'\n",
        Minimal(CellRow("[2147483648,0]")), Minimal(CellRow("[0,2147483648]")), Minimal(CellRow("[0.5,0]")),
        Minimal(CellRow()) + "unknown = true\n", Minimal(CellRow()) + "[[cells.dependencies]]\ntypeName = 'Model'\n",
        Minimal(CellRow()) + "not valid TOML = [\n"};
    for (const auto& text : invalid) {
        auto output = Example();
        std::string diagnostic;
        EXPECT_FALSE(scene::DeserializeWorldPartitionManifest(text, output, diagnostic));
        EXPECT_EQ(output.revision, (std::numeric_limits<uint64_t>::max)());
        ASSERT_EQ(output.cells.size(), 1u);
        EXPECT_EQ(output.cells.front().nearPath, "Assets/World/Cells/-2_3.scene");
        EXPECT_FALSE(diagnostic.empty());
    }
}

TEST_F(WorldPartitionManifestTest, RejectsInvalidDependencyReferencesAndCanonicalGuidDuplicates)
{
    const std::vector<scene::WorldPartitionDependency> invalid = {{"", "Assets/tree.fzasset"},
        {"Model\ninvalid", "Assets/tree.fzasset"}, {"Model", "C:/outside/tree.fzasset"},
        {"Model", "guid:bad"}, {"Model", "guid:0123456789abcdef0123456789abcdef|../outside.fbx"}};
    std::string diagnostic;
    for (const auto& dependency : invalid) {
        auto source = Example();
        source.cells.front().dependencies.push_back(dependency);
        EXPECT_FALSE(scene::ValidateWorldPartitionManifest(source, diagnostic));
    }
    auto source = Example();
    source.cells.front().dependencies.push_back({"Model", "guid:0123456789ABCDEF0123456789ABCDEF|Assets/Renamed.fbx"});
    EXPECT_FALSE(scene::ValidateWorldPartitionManifest(source, diagnostic));
}

TEST_F(WorldPartitionManifestTest, SaveAndLoadPreserveThePriorFileWhenReplacementIsInvalid)
{
    testkit::TempDir temp("WorldPartitionManifest");
    ASSERT_TRUE(temp.IsValid());
    const auto path = util::FileSystem::PathToUtf8(temp.File("World/Island.world"));
    const auto source = Example();
    std::string diagnostic;
    ASSERT_TRUE(scene::SaveWorldPartitionManifest(path, source, diagnostic)) << diagnostic;
    std::string before;
    ASSERT_TRUE(util::FileSystem::ReadText(path, before));
    auto invalid = source;
    invalid.cells.front().revision = 0;
    EXPECT_FALSE(scene::SaveWorldPartitionManifest(path, invalid, diagnostic));
    std::string after;
    ASSERT_TRUE(util::FileSystem::ReadText(path, after));
    EXPECT_EQ(after, before);
    scene::WorldPartitionManifest restored;
    ASSERT_TRUE(scene::LoadWorldPartitionManifest(path, restored, diagnostic)) << diagnostic;
    ASSERT_EQ(restored.cells.size(), 1u);
    EXPECT_EQ(restored.cells.front().revision, source.cells.front().revision);
    auto unchanged = source;
    EXPECT_FALSE(scene::LoadWorldPartitionManifest(util::FileSystem::PathToUtf8(temp.File("missing.world")), unchanged, diagnostic));
    EXPECT_EQ(unchanged.revision, source.revision);
}

TEST_F(WorldPartitionManifestTest, SupportsAnEmptyWorldAndRejectsExcessiveCounts)
{
    scene::WorldPartitionManifest empty;
    std::string text, diagnostic;
    ASSERT_TRUE(scene::SerializeWorldPartitionManifest(empty, text, diagnostic));
    auto restored = Example();
    ASSERT_TRUE(scene::DeserializeWorldPartitionManifest(text, restored, diagnostic));
    EXPECT_TRUE(restored.cells.empty());
    auto tooMany = empty;
    tooMany.cells.resize(65537);
    EXPECT_FALSE(scene::ValidateWorldPartitionManifest(tooMany, diagnostic));
    auto source = Example();
    source.cells.front().dependencies.resize(1025);
    EXPECT_FALSE(scene::ValidateWorldPartitionManifest(source, diagnostic));
}

} /// @note namespace fbzz::tests
