/// @file    AssetReferenceScanTests.cpp
/// @brief   実際の保存参照・不明な来歴・範囲外入力と明示更新の境界を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include "Panels/AssetBrowser/AssetReferenceScan.hpp"
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TestKit.hpp>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fbzz::tests {

class AssetReferenceScanTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        std::error_code error;
        std::filesystem::create_directories(m_temp.Path() / "Assets", error);
        ASSERT_FALSE(error);
    }

    void TearDown() override
    {
        asset::AssetDatabase::Shutdown();
        EngineFixture::TearDown();
    }

    std::string Root() const { return util::FileSystem::PathToUtf8(m_temp.Path()); }
    std::string Path(const std::string& relative) const
    {
        return util::FileSystem::PathToUtf8(m_temp.Path() / util::FileSystem::PathFromUtf8(relative));
    }
    bool Write(const std::string& relative, const std::string& text)
    {
        const auto path = util::FileSystem::PathFromUtf8(Path(relative));
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        return !error && util::FileSystem::WriteText(path, text);
    }
    editor::AssetSearchEntry Entry(const std::string& relative) const
    {
        return {Path(relative), relative, util::FileSystem::GetFilename(relative),
            util::FileSystem::GetExtension(relative)};
    }
    editor::assetrefs::Request Request(const std::string& target, std::vector<editor::AssetSearchEntry> entries) const
    {
        return {Root(), Path(target), 17, asset::AssetDatabase::Revision(), std::move(entries)};
    }
    void Finish(editor::assetrefs::Scan& scan)
    {
        for (size_t step = 0; step < 32 && !scan.Result().finished; ++step) scan.Advance(1);
        ASSERT_TRUE(scan.Result().finished);
    }

    testkit::TempDir m_temp{"asset-reference-scan"};
};

TEST_F(AssetReferenceScanTest, FindsCodecGuidAndNestedPathValuesWithOwnerAndFieldEvidence)
{
    ASSERT_TRUE(Write("Assets/Textures/船 Boat.png", "source"));
    asset::AssetDatabase::Init(Path("Assets"));
    toml::table material;
    material.insert("textures", toml::table{{"albedo", Path("Assets/Textures/船 Boat.png")}});
    asset::EncodeGuidRefs(material);
    std::ostringstream saved;
    saved << material;
    ASSERT_NE(saved.str().find("guid:"), std::string::npos);
    ASSERT_TRUE(Write("Assets/Materials/Hull.mat", saved.str()));
    ASSERT_TRUE(Write("Assets/Scenes/Harbor.scene",
        "[[entities]]\n[entities.renderer]\ntextures = ['Assets/Textures/船 Boat.png']\n"));
    editor::assetrefs::Scan scan;

    scan.Begin(Request("Assets/Textures/船 Boat.png", {Entry("Assets/Scenes/Harbor.scene"), Entry("Assets/Materials/Hull.mat")}));
    Finish(scan);

    const auto& report = scan.Result();
    ASSERT_EQ(report.owners.size(), 2u);
    EXPECT_EQ(report.owners[0].kind, "Material");
    ASSERT_EQ(report.owners[0].evidence.size(), 1u);
    EXPECT_EQ(report.owners[0].evidence[0].kind, "GUID value");
    EXPECT_EQ(report.owners[0].evidence[0].field, "textures.albedo");
    EXPECT_GT(report.owners[0].evidence[0].line, 0u);
    EXPECT_EQ(report.owners[1].kind, "Scene");
    EXPECT_EQ(report.owners[1].evidence[0].field, "entities[0].renderer.textures[0]");
}

TEST_F(AssetReferenceScanTest, FindsDependenciesInDedicatedPaintAndPlacementFormats)
{
    ASSERT_TRUE(Write("Assets/Pine.fbx", "mesh"));
    asset::AssetDatabase::Init(Path("Assets"));
    const auto guid = asset::AssetDatabase::TryGetGuidFromPath(Path("Assets/Pine.fbx"));
    ASSERT_FALSE(guid.empty());
    const auto reference = "guid:" + guid;
    ASSERT_TRUE(Write("Assets/Pine.terrainpaint", "[[mesh.parts]]\nmesh='" + reference + "'\n"));
    ASSERT_TRUE(Write("Assets/Pine.meshinstances", "[[mesh.parts]]\nmesh='" + reference + "'\n"));
    editor::assetrefs::Scan scan;
    scan.Begin(Request("Assets/Pine.fbx", {Entry("Assets/Pine.terrainpaint"), Entry("Assets/Pine.meshinstances")}));
    Finish(scan);
    ASSERT_EQ(scan.Result().owners.size(), 2u);
    EXPECT_EQ(scan.Result().unsupported, 0u);
    for (const auto& owner : scan.Result().owners) {
        EXPECT_TRUE(owner.kind == "Terrain paint" || owner.kind == "Mesh instances");
        ASSERT_EQ(owner.evidence.size(), 1u);
        EXPECT_EQ(owner.evidence[0].field, "mesh.parts[0].mesh");
    }
}

TEST_F(AssetReferenceScanTest, IgnoresCommentsSubstringsScriptLoadsAndUnknownGuidSuffixes)
{
    ASSERT_TRUE(Write("Assets/Boat.png", "source"));
    ASSERT_TRUE(Write("Assets/Boat.png.meta", "[meta]\nguid = '11111111111111111111111111111111'\n"));
    ASSERT_TRUE(Write("Assets/Test.scene",
        "# texture = 'Assets/Boat.png'\n"
        "note = 'Use Assets/Boat.png during testing'\n"
        "near = 'Assets/Boat.png.backup'\n"
        "bad = 'guid:11111111111111111111111111111111extra'\n"));
    ASSERT_TRUE(Write("Assets/Load.cs", "var value = Load(\"Assets/Boat.png\");"));
    editor::assetrefs::Scan scan;

    scan.Begin(Request("Assets/Boat.png", {Entry("Assets/Test.scene"), Entry("Assets/Load.cs")}));
    Finish(scan);

    EXPECT_TRUE(scan.Result().owners.empty());
    EXPECT_EQ(scan.Result().unsupported, 1u);
    EXPECT_EQ(scan.Result().parsed, 1u);
}

TEST_F(AssetReferenceScanTest, KeepsGuidAuthoritySeparateFromAnUnresolvedHint)
{
    ASSERT_TRUE(Write("Assets/Boat.png", "source"));
    ASSERT_TRUE(Write("Assets/Other.png", "source"));
    asset::AssetDatabase::Init(Path("Assets"));
    const std::string otherGuid = asset::AssetDatabase::TryGetGuidFromPath(Path("Assets/Other.png"));
    ASSERT_FALSE(otherGuid.empty());
    ASSERT_TRUE(Write("Assets/Stale.mat", "texture = 'guid:" + otherGuid + "|Assets/Boat.png'\n"));
    ASSERT_TRUE(Write("Assets/Unresolved.mat",
        "texture = 'guid:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee|Assets/Boat.png'\n"));
    editor::assetrefs::Scan scan;

    scan.Begin(Request("Assets/Boat.png", {Entry("Assets/Stale.mat"), Entry("Assets/Unresolved.mat")}));
    Finish(scan);

    ASSERT_EQ(scan.Result().owners.size(), 1u);
    EXPECT_EQ(scan.Result().owners[0].path, Path("Assets/Unresolved.mat"));
    EXPECT_EQ(scan.Result().owners[0].evidence[0].kind, "Unresolved GUID hint (unverified)");
    EXPECT_EQ(scan.Result().unresolvedHints, 1u);
}

TEST_F(AssetReferenceScanTest, ResolvesTextureSidecarAliasesAndKeepsSelectedSpriteSuffixExact)
{
    ASSERT_TRUE(Write("Assets/Atlas.png", "source"));
    ASSERT_TRUE(Write("Assets/A.scene",
        "image = 'Assets\\Atlas.png.meta'\n"
        "first = 'Assets/Atlas.png.meta::sprite::sprite-one'\n"
        "second = 'Assets/Atlas.png::sprite::sprite-two'\n"));
    const auto request = Request("Assets/Atlas.png", {Entry("Assets/A.scene")});
    editor::assetrefs::Scan scan;

    scan.Begin(request);
    Finish(scan);
    ASSERT_EQ(scan.Result().owners.size(), 1u);
    EXPECT_EQ(scan.Result().owners[0].evidence.size(), 3u);
    auto selectedSprite = request;
    selectedSprite.targetPath += "::sprite::sprite-one";
    scan.Begin(std::move(selectedSprite));
    Finish(scan);

    ASSERT_EQ(scan.Result().owners.size(), 1u);
    ASSERT_EQ(scan.Result().owners[0].evidence.size(), 1u);
    EXPECT_EQ(scan.Result().owners[0].evidence[0].field, "first");
    EXPECT_FALSE(util::FileSystem::Exists(Path("Assets/Atlas.png.meta")));
}

TEST_F(AssetReferenceScanTest, ReportsOnlyRecordedImportMetadataWithoutFilenameClassification)
{
    ASSERT_TRUE(Write("Assets/Imported_Verification_Blender.fbx", "source"));
    editor::assetrefs::Scan scan;

    scan.Begin(Request("Assets/Imported_Verification_Blender.fbx", {}));

    EXPECT_TRUE(scan.Result().metadata.sourceDcc.empty());
    EXPECT_TRUE(scan.Result().metadata.importer.empty());
    EXPECT_EQ(scan.Result().metadata.importerVersion, -1);
    ASSERT_TRUE(Write("Assets/Imported_Verification_Blender.fbx.meta",
        "[model]\nsource_dcc = 'Auto'\nimporter = 'ModelImporter'\nimporter_version = 4\n"));
    scan.Begin(Request("Assets/Imported_Verification_Blender.fbx", {}));

    EXPECT_EQ(scan.Result().metadata.sourceDcc, "Auto");
    EXPECT_EQ(scan.Result().metadata.importer, "ModelImporter");
    EXPECT_EQ(scan.Result().metadata.importerVersion, 4);
}

TEST_F(AssetReferenceScanTest, RefreshReplacesTheOwnedSnapshotAndCompletedScansDoNotReadAgain)
{
    ASSERT_TRUE(Write("Assets/Boat.png", "source"));
    ASSERT_TRUE(Write("Assets/A.scene", "texture = 'Assets/Boat.png'\n"));
    ASSERT_TRUE(Write("Assets/B.mat", "name = 'empty'\n"));
    const auto request = Request("Assets/Boat.png", {Entry("Assets/A.scene"), Entry("Assets/B.mat")});
    editor::assetrefs::Scan scan;
    scan.Begin(request);

    scan.Advance(1);
    EXPECT_EQ(scan.Result().processed, 1u);
    EXPECT_FALSE(scan.Result().finished);
    Finish(scan);
    ASSERT_EQ(scan.Result().owners.size(), 1u);
    const auto firstBytes = scan.Result().bytesRead;
    ASSERT_TRUE(Write("Assets/A.scene", "name = 'changed'\n"));
    scan.Advance();

    EXPECT_EQ(scan.Result().bytesRead, firstBytes);
    EXPECT_EQ(scan.Result().owners.size(), 1u);
    EXPECT_TRUE(scan.MatchesSnapshot(Root(), 17, request.databaseRevision));
    EXPECT_FALSE(scan.MatchesSnapshot(Root(), 18, request.databaseRevision));
    EXPECT_FALSE(scan.MatchesSnapshot(Root(), 17, request.databaseRevision + 1));
    EXPECT_FALSE(scan.MatchesSnapshot(Path("OtherProject"), 17, request.databaseRevision));
    scan.Begin(request);
    Finish(scan);

    EXPECT_EQ(scan.Result().generation, 2u);
    EXPECT_TRUE(scan.Result().owners.empty());
}

TEST_F(AssetReferenceScanTest, ReportsMissingMalformedAndOversizedFilesWithoutCreatingMetadata)
{
    ASSERT_TRUE(Write("Assets/Boat.png", "source"));
    ASSERT_TRUE(Write("Assets/Bad.scene", "value = ["));
    ASSERT_TRUE(Write("Assets/Huge.scene", std::string(2 * 1024 * 1024 + 1, 'x')));
    editor::assetrefs::Scan scan;

    scan.Begin(Request("Assets/Boat.png",
        {Entry("Assets/Bad.scene"), Entry("Assets/Huge.scene"), Entry("Assets/Missing.scene")}));
    Finish(scan);

    EXPECT_EQ(scan.Result().malformed, 1u);
    EXPECT_EQ(scan.Result().oversized, 1u);
    EXPECT_TRUE(scan.Result().limited);
    EXPECT_EQ(scan.Result().unreadable, 1u);
    EXPECT_TRUE(scan.Result().owners.empty());
    EXPECT_FALSE(util::FileSystem::Exists(Path("Assets/Boat.png.meta")));
}

TEST_F(AssetReferenceScanTest, UsesExistingDerivedGuidMappingForADirectGeneratedTarget)
{
    constexpr const char* SOURCE_GUID = "1234567890abcdef1234567890abcdef";
    const std::string generated = std::string("Library/Baked/") + SOURCE_GUID + "/materials/Hull.mat";
    ASSERT_TRUE(Write(generated, "name = 'Hull'\n"));
    asset::AssetDatabase::Init(Path("Assets"));
    const std::string guid = asset::AssetDatabase::TryGetGuidFromPath(Path(generated));
    ASSERT_EQ(guid, asset::AssetDatabase::DeriveGuid(SOURCE_GUID, "materials/Hull.mat"));
    ASSERT_FALSE(guid.empty());
    ASSERT_TRUE(Write("Assets/Harbor.scene", "material = 'guid:" + guid + "|" + generated + "'\n"));
    editor::assetrefs::Scan scan;

    scan.Begin(Request(generated, {Entry("Assets/Harbor.scene")}));
    Finish(scan);

    ASSERT_EQ(scan.Result().owners.size(), 1u);
    EXPECT_EQ(scan.Result().owners[0].evidence[0].kind, "GUID value");
    EXPECT_FALSE(util::FileSystem::Exists(Path(generated + ".meta")));
}

} /// @note namespace fbzz::tests
