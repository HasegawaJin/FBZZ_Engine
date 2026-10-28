/// @file    GuidRefCodecTests.cpp
/// @brief   保存境界でのアセット参照 ⇄ GUID 変換が往復し、参照でない文字列を壊さないことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// ランタイムは "Assets/..." の文字列を持ち、ディスクだけが "guid:..." を持つ。
/// この変換が «参照でない文字列» まで触ると、スクリプトの設定値やタグが壊れる。
/// 逆に取りこぼすと、リネームした瞬間にその参照だけが切れる ── どちらも
/// «保存して開き直したときだけ» 出るので、手で触っている限り気づけない。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fbzz::tests {

/// 一時ディレクトリに Assets ルートを作り、そこへ実ファイルと .meta を置く。
class GuidRefCodecTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        std::filesystem::create_directories(AssetsRoot());
    }

    void TearDown() override
    {
        asset::AssetDatabase::Shutdown();
        EngineFixture::TearDown();
    }

    std::filesystem::path AssetsRoot() const { return m_temp.File("Project/Assets"); }
    std::filesystem::path SharedRoot() const { return m_temp.File("EngineAssets"); }

    /// Assets 配下へファイルを作り、その絶対パス (正規化済み) を返す。
    std::string CreateAsset(const std::string& relativePath)
    {
        const std::filesystem::path full = AssetsRoot() / relativePath;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary);
        out << "content";
        out.close();
        return full.generic_string();
    }

    /// @brief Writes a shared asset and its immutable fixture GUID metadata.
    std::string CreateSharedAsset(const std::string& relativePath,
                                  const std::string& guid,
                                  bool writeMeta = true)
    {
        const std::filesystem::path full = SharedRoot() / "Assets" / relativePath;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary);
        out << "shared content";
        out.close();
        if (writeMeta) {
            std::ofstream meta(full.string() + ".meta", std::ios::binary);
            meta << "[meta]\nguid = '" << guid << "'\n";
        }
        return util::FileSystem::PathToUtf8(full);
    }

    /// @brief Initializes a project index with an optional read-only Engine root.
    void InitDatabase(const std::filesystem::path& sharedRoot = {})
    {
        asset::AssetDatabase::Init(util::FileSystem::PathToUtf8(AssetsRoot()),
                                   sharedRoot.empty()
                                       ? std::string{}
                                       : util::FileSystem::PathToUtf8(sharedRoot));
    }

private:
    testkit::TempDir m_temp{"guidref"};
};

/// @name Encode

TEST_F(GuidRefCodecTest, EncodesAKnownAssetIntoAGuidReference)
{
    const std::string texture = CreateAsset("Textures/Player.png");
    InitDatabase();

    const std::string encoded = asset::EncodeGuidRef(texture);

    EXPECT_TRUE(asset::AssetDatabase::IsGuidRef(encoded)) << encoded;
    EXPECT_EQ(asset::AssetDatabase::GuidFromRef(encoded),
              asset::AssetDatabase::TryGetGuidFromPath(texture));
}

TEST_F(GuidRefCodecTest, WritesTheProjectRelativePathAsAHint)
{
    /// @note guid だけだと «どのファイルか» をファイルの中から辿れない。人と grep のために併記する。
    const std::string texture = CreateAsset("Textures/Player.png");
    InitDatabase();

    const std::string encoded = asset::EncodeGuidRef(texture);

    EXPECT_EQ(asset::AssetDatabase::HintFromRef(encoded), "Assets/Textures/Player.png");
}

TEST_F(GuidRefCodecTest, EncodingIsIdempotent)
{
    /// @note Save のたびに掛かる。2 度掛けて "guid:guid:..." になってはいけない。
    const std::string texture = CreateAsset("Textures/Player.png");
    InitDatabase();
    const std::string once = asset::EncodeGuidRef(texture);

    EXPECT_EQ(asset::EncodeGuidRef(once), once);
}

TEST_F(GuidRefCodecTest, LeavesValuesThatAreNotAssetPathsAlone)
{
    /// @note 文書中の全文字列に掛かるため、ここが緩いとスクリプトの設定値が書き換わる。
    InitDatabase();

    EXPECT_EQ(asset::EncodeGuidRef("Player.001"), "Player.001");
    EXPECT_EQ(asset::EncodeGuidRef("Score: 0.5"), "Score: 0.5");
    EXPECT_EQ(asset::EncodeGuidRef("Untagged"), "Untagged");
    EXPECT_EQ(asset::EncodeGuidRef(""), "");
}

TEST_F(GuidRefCodecTest, LeavesPathsThatAreNotRegisteredAlone)
{
    /// @note 実体が無い / Assets の外。データを勝手に捨てず、そのまま通す。
    InitDatabase();

    EXPECT_EQ(asset::EncodeGuidRef("Assets/Textures/Missing.png"),
              "Assets/Textures/Missing.png");
}

/// @name Decode

TEST_F(GuidRefCodecTest, DecodesBackToTheRuntimeRelativePath)
{
    /// @note ランタイムと Inspector は "Assets/..." 前提。ここだけ絶対パスを返すと
    ///       同じアセットが 2 通りの文字列で流通する。
    const std::string texture = CreateAsset("Textures/Player.png");
    InitDatabase();

    const std::string decoded = asset::DecodeGuidRef(asset::EncodeGuidRef(texture));

    EXPECT_EQ(decoded, "Assets/Textures/Player.png");
}

TEST_F(GuidRefCodecTest, RoundTripsSharedEngineAssetsByGuidWithoutCopyingThemIntoTheProject)
{
    const std::string guid = "1234567890abcdef1234567890abcdef";
    const std::string shared = CreateSharedAsset("Materials/Water/Ocean.mat", guid);
    InitDatabase(SharedRoot());

    const std::string encoded = asset::EncodeGuidRef(shared);

    EXPECT_EQ(encoded, "guid:" + guid);
    EXPECT_TRUE(std::filesystem::exists(shared));
    EXPECT_FALSE(std::filesystem::exists(AssetsRoot() / "Materials" / "Water" / "Ocean.mat"));
    EXPECT_EQ(asset::AssetDatabase::PathFromGuid(guid), shared);
    EXPECT_EQ(asset::AssetManager::ResolveAssetPath(encoded), shared);
    EXPECT_EQ(asset::DecodeGuidRef(encoded), shared);
}

TEST_F(GuidRefCodecTest, SharedGuidDecodePreservesSubassetSuffixAndSurvivesAProjectClone)
{
    const std::string guid = "abcdefabcdefabcdefabcdefabcdefab";
    const std::string shared = CreateSharedAsset("Models/Water.fbx", guid);
    InitDatabase(SharedRoot());

    const std::string encoded = asset::EncodeGuidRef(shared + ":2");
    EXPECT_EQ(encoded, "guid:" + guid + ":2");
    EXPECT_EQ(asset::DecodeGuidRef(encoded), shared + ":2");

    const std::filesystem::path cloneAssets = AssetsRoot().parent_path() / "Clone" / "Assets";
    std::filesystem::create_directories(cloneAssets);
    asset::AssetDatabase::Shutdown();
    asset::AssetDatabase::Init(util::FileSystem::PathToUtf8(cloneAssets),
                               util::FileSystem::PathToUtf8(SharedRoot()));

    EXPECT_EQ(asset::DecodeGuidRef(encoded), shared + ":2");
}

TEST_F(GuidRefCodecTest, ProjectCopyWinsOverSharedAssetWithTheSameGuid)
{
    const std::string guid = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const std::string shared = CreateSharedAsset("Materials/Water/Ocean.mat", guid);
    const std::string project = CreateAsset("Materials/Water/Ocean.mat");
    std::ofstream meta(project + ".meta", std::ios::binary);
    meta << "[meta]\nguid = '" << guid << "'\n";
    meta.close();
    InitDatabase(SharedRoot());

    EXPECT_EQ(asset::AssetDatabase::PathFromGuid(guid), project);
    EXPECT_EQ(asset::AssetDatabase::TryGetGuidFromPath(shared), guid);
    EXPECT_EQ(asset::AssetDatabase::GuidConflictCount(), 0u);
    EXPECT_EQ(asset::DecodeGuidRef("guid:" + guid + "|Assets/Materials/Water/Ocean.mat"),
              "Assets/Materials/Water/Ocean.mat");
}

TEST_F(GuidRefCodecTest, DifferentProjectAndSharedAssetsWithOneGuidAreReported)
{
    const std::string guid = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const std::string shared = CreateSharedAsset("Materials/Water/Ocean.mat", guid);
    const std::string project = CreateAsset("Materials/Local/Other.mat");
    std::ofstream meta(project + ".meta", std::ios::binary);
    meta << "[meta]\nguid = '" << guid << "'\n";
    meta.close();
    InitDatabase(SharedRoot());

    EXPECT_EQ(asset::AssetDatabase::GuidConflictCount(), 1u);
    EXPECT_EQ(asset::AssetDatabase::PathFromGuid(guid), project);
    EXPECT_EQ(asset::AssetDatabase::TryGetGuidFromPath(shared), guid);
}

TEST_F(GuidRefCodecTest, SharedAssetWithoutMatchingMetadataRemainsUnresolvedAndUnchanged)
{
    const std::string guid = "fedcba0987654321fedcba0987654321";
    const std::string shared = CreateSharedAsset("Materials/Water/Missing.mat", guid,
                                                 false);
    InitDatabase(SharedRoot());
    const std::string ref = "guid:" + guid + "|Assets/Materials/Water/Missing.mat";

    EXPECT_EQ(asset::AssetDatabase::PathFromGuid(guid), "");
    EXPECT_EQ(asset::DecodeGuidRef(ref), ref);
    EXPECT_EQ(asset::AssetManager::ResolveAssetPath(ref), "");
    EXPECT_EQ(asset::AssetDatabase::GuidFromPath(shared), "");
    EXPECT_FALSE(std::filesystem::exists(shared + ".meta"));
}

TEST_F(GuidRefCodecTest, DifferentSharedAssetsWithOneGuidRemainAmbiguous)
{
    const std::string guid = "0123456789abcdef0123456789abcdef";
    const std::string first = CreateSharedAsset("Materials/Water/First.mat", guid);
    const std::string second = CreateSharedAsset("Materials/Water/Second.mat", guid);
    InitDatabase(SharedRoot());

    EXPECT_EQ(asset::AssetDatabase::PathFromGuid(guid), "");
    EXPECT_EQ(asset::AssetDatabase::TryGetGuidFromPath(first), "");
    EXPECT_EQ(asset::AssetDatabase::TryGetGuidFromPath(second), "");
    EXPECT_EQ(asset::EncodeGuidRef(first), first);
    EXPECT_EQ(asset::AssetManager::ResolveAssetPath("guid:" + guid), "");
}

TEST_F(GuidRefCodecTest, ReassigningSharedGuidLeavesMetadataAndBakedContainerUnchanged)
{
    const std::string guid = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
    const std::string shared = CreateSharedAsset("Models/Immutable.fbx", guid);
    const auto baked = AssetsRoot().parent_path() / "Library" / "Baked" / guid;
    std::filesystem::create_directories(baked);
    {
        std::ofstream payload(baked / "payload.fzasset", std::ios::binary);
        payload << "immutable baked payload";
    }
    InitDatabase(SharedRoot());
    std::string replacement = "untouched";

    EXPECT_FALSE(asset::AssetDatabase::ReassignGuid(shared, replacement));
    const auto alias = AssetsRoot() / ".." / ".." / "EngineAssets" / "Assets" / "Models" / "Immutable.fbx";
    EXPECT_FALSE(asset::AssetDatabase::ReassignGuid(util::FileSystem::PathToUtf8(alias), replacement));
    EXPECT_EQ(replacement, "untouched");
    EXPECT_EQ(asset::EncodeGuidRef(shared), "guid:" + guid);
    EXPECT_TRUE(std::filesystem::exists(baked / "payload.fzasset"));
    std::string metadata;
    ASSERT_TRUE(util::FileSystem::ReadText(shared + ".meta", metadata));
    EXPECT_EQ(metadata, "[meta]\nguid = '" + guid + "'\n");
}

TEST_F(GuidRefCodecTest, DecodingLeavesPlainPathsAlone)
{
    InitDatabase();

    EXPECT_EQ(asset::DecodeGuidRef("Assets/Textures/Player.png"),
              "Assets/Textures/Player.png");
    EXPECT_EQ(asset::DecodeGuidRef("Untagged"), "Untagged");
}

TEST_F(GuidRefCodecTest, RecoversAnUnresolvableGuidThroughItsPathHint)
{
    /// @note .meta を作り直すと guid が変わる。ヒントの実体が残っていれば参照を生かす。
    CreateAsset("Textures/Player.png");
    InitDatabase();

    const std::string broken =
        "guid:00000000000000000000000000000000|Assets/Textures/Player.png";

    EXPECT_EQ(asset::DecodeGuidRef(broken), "Assets/Textures/Player.png");
}

TEST_F(GuidRefCodecTest, KeepsAnUnresolvableReferenceWhenNothingCanBeRecovered)
{
    /// @note 引けず、ヒントの実体も無い。空にすると «何を指していたか» が永久に失われる。
    InitDatabase();
    const std::string broken =
        "guid:00000000000000000000000000000000|Assets/Textures/Gone.png";

    EXPECT_EQ(asset::DecodeGuidRef(broken), broken);
}

TEST_F(GuidRefCodecTest, SurvivesTheFullRoundTripTwice)
{
    const std::string texture = CreateAsset("Textures/Player.png");
    InitDatabase();

    const std::string first  = asset::DecodeGuidRef(asset::EncodeGuidRef(texture));
    const std::string second = asset::DecodeGuidRef(asset::EncodeGuidRef(first));

    EXPECT_EQ(second, first);
}

/// @name TOML ツリー全体

TEST_F(GuidRefCodecTest, WalksNestedTablesAndArrays)
{
    /// @note Save 直前に文書全体へ掛ける。入れ子や配列の中の参照を取りこぼすと、
    ///       その 1 つだけリネームに弱いまま残る。
    const std::string texture = CreateAsset("Textures/Player.png");
    InitDatabase();

    toml::table root;
    root.insert("albedo", texture);
    toml::table nested;
    nested.insert("normal", texture);
    root.insert("maps", std::move(nested));
    root.insert("layers", toml::array{ texture, "Untagged" });

    asset::EncodeGuidRefs(root);

    EXPECT_TRUE(asset::AssetDatabase::IsGuidRef(root["albedo"].value_or(std::string{})));
    EXPECT_TRUE(asset::AssetDatabase::IsGuidRef(root["maps"]["normal"].value_or(std::string{})));
    EXPECT_TRUE(asset::AssetDatabase::IsGuidRef(root["layers"][0].value_or(std::string{})));
    EXPECT_EQ(root["layers"][1].value_or(std::string{}), "Untagged");
}

TEST_F(GuidRefCodecTest, DecodesTheWholeTreeBack)
{
    const std::string texture = CreateAsset("Textures/Player.png");
    InitDatabase();

    toml::table root;
    root.insert("albedo", texture);
    asset::EncodeGuidRefs(root);

    asset::DecodeGuidRefs(root);

    EXPECT_EQ(root["albedo"].value_or(std::string{}), "Assets/Textures/Player.png");
}

TEST_F(GuidRefCodecTest, LeavesNonStringValuesUntouched)
{
    InitDatabase();
    toml::table root;
    root.insert("count", 3);
    root.insert("scale", 1.5);
    root.insert("enabled", true);

    asset::EncodeGuidRefs(root);

    EXPECT_EQ(root["count"].value_or(0), 3);
    EXPECT_NEAR(root["scale"].value_or(0.0), 1.5, testkit::kTolerance);
    EXPECT_TRUE(root["enabled"].value_or(false));
}

} // namespace fbzz::tests
