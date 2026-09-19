/// @file    AssetDatabaseTests.cpp
/// @brief   GUID 参照の解析・導出 GUID の決定性・.meta 索引の移動と削除の追従を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// 参照は «パスではなく guid» で持つ、というのがこのエンジンのアセット参照の芯。
/// 導出 GUID が環境で変わったり、移動で索引が付け替わらなかったりすると、
/// «手元では開けるのに clone した先では参照が全部切れている» という形で出る。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/AssetDatabase.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace fbzz::tests {
namespace {

using asset::AssetDatabase;

bool IsLowerHex32(const std::string& value)
{
    if (value.size() != 32) return false;
    for (const char c : value) {
        const bool digit = c >= '0' && c <= '9';
        const bool hex   = c >= 'a' && c <= 'f';
        if (!digit && !hex) return false;
    }
    return true;
}

void WriteFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

} // namespace

/// @name 参照文字列の解析 (索引に依存しない純ロジック)

class AssetRefTest : public testkit::EngineFixture {};

TEST_F(AssetRefTest, RecognisesOnlyThePrefixedForm)
{
    EXPECT_TRUE(AssetDatabase::IsGuidRef("guid:0123456789abcdef0123456789abcdef"));
    EXPECT_FALSE(AssetDatabase::IsGuidRef("Assets/Textures/Player.png"));
    EXPECT_FALSE(AssetDatabase::IsGuidRef(""));
}

TEST_F(AssetRefTest, ExtractsTheGuidWithoutThePathHint)
{
    const std::string guid = AssetDatabase::GuidFromRef(
        "guid:0123456789abcdef0123456789abcdef|Assets/Textures/Player.png");

    EXPECT_EQ(guid, "0123456789abcdef0123456789abcdef");
}

TEST_F(AssetRefTest, ExtractsTheGuidWithoutASubAssetSuffix)
{
    /// @note 接尾辞の区切り文字に依存しないよう、hex が続く限りで切る契約。
    const std::string guid =
        AssetDatabase::GuidFromRef("guid:0123456789abcdef0123456789abcdef@clip3");

    EXPECT_EQ(guid, "0123456789abcdef0123456789abcdef");
}

TEST_F(AssetRefTest, NormalisesTheGuidToLowerCase)
{
    EXPECT_EQ(AssetDatabase::GuidFromRef("guid:0123456789ABCDEF0123456789ABCDEF"),
              "0123456789abcdef0123456789abcdef");
}

TEST_F(AssetRefTest, ReturnsNoGuidForAPlainPath)
{
    EXPECT_EQ(AssetDatabase::GuidFromRef("Assets/Textures/Player.png"), "");
}

TEST_F(AssetRefTest, ExtractsThePathHintAfterTheSeparator)
{
    EXPECT_EQ(AssetDatabase::HintFromRef(
                  "guid:0123456789abcdef0123456789abcdef|Assets/Textures/Player.png"),
              "Assets/Textures/Player.png");
}

TEST_F(AssetRefTest, HasNoHintWhenNoneIsWritten)
{
    EXPECT_EQ(AssetDatabase::HintFromRef("guid:0123456789abcdef0123456789abcdef"), "");
    EXPECT_EQ(AssetDatabase::HintFromRef("Assets/Textures/Player.png"), "");
}

/// @name GUID の生成と導出

TEST_F(AssetRefTest, GeneratesA32DigitHexGuid)
{
    EXPECT_TRUE(IsLowerHex32(AssetDatabase::GenerateGuid()));
}

TEST_F(AssetRefTest, GeneratesADifferentGuidEachTime)
{
    EXPECT_NE(AssetDatabase::GenerateGuid(), AssetDatabase::GenerateGuid());
}

TEST_F(AssetRefTest, DerivesTheSameGuidForTheSameInputs)
{
    /// @note Library は .gitignore 済み。再インポートで値が変われば参照が全部切れる。
    const std::string a = AssetDatabase::DeriveGuid("0123456789abcdef0123456789abcdef",
                                                    "anims/MiniBot@Idle.anim");
    const std::string b = AssetDatabase::DeriveGuid("0123456789abcdef0123456789abcdef",
                                                    "anims/MiniBot@Idle.anim");

    EXPECT_EQ(a, b);
    EXPECT_TRUE(IsLowerHex32(a));
}

TEST_F(AssetRefTest, DerivesADifferentGuidPerSubKey)
{
    const std::string idle = AssetDatabase::DeriveGuid("0123456789abcdef0123456789abcdef",
                                                       "anims/Idle.anim");
    const std::string run  = AssetDatabase::DeriveGuid("0123456789abcdef0123456789abcdef",
                                                       "anims/Run.anim");

    EXPECT_NE(idle, run);
}

TEST_F(AssetRefTest, DerivesADifferentGuidPerSource)
{
    const std::string fromA = AssetDatabase::DeriveGuid("0123456789abcdef0123456789abcdef",
                                                        "anims/Idle.anim");
    const std::string fromB = AssetDatabase::DeriveGuid("fedcba9876543210fedcba9876543210",
                                                        "anims/Idle.anim");

    EXPECT_NE(fromA, fromB);
}

TEST_F(AssetRefTest, DerivesNothingFromEmptyInputs)
{
    EXPECT_EQ(AssetDatabase::DeriveGuid("", "anims/Idle.anim"), "");
    EXPECT_EQ(AssetDatabase::DeriveGuid("0123456789abcdef0123456789abcdef", ""), "");
}

/// @name .meta を持つ対象の判定

TEST_F(AssetRefTest, SourceAssetsAndDocumentsGetMeta)
{
    EXPECT_TRUE(AssetDatabase::ShouldHaveMeta("player.png"));
    EXPECT_TRUE(AssetDatabase::ShouldHaveMeta("player.fbx"));
    EXPECT_TRUE(AssetDatabase::ShouldHaveMeta("readme.md"));
    EXPECT_TRUE(AssetDatabase::ShouldHaveMeta("stage_01.scene"));
}

TEST_F(AssetRefTest, DotLeadingFilesWithARealExtensionStillGetMeta)
{
    /// @note ".playmode_snapshot.scene" は実アセット。«ドット始まりは全除外» にできない。
    EXPECT_TRUE(AssetDatabase::ShouldHaveMeta(".playmode_snapshot.scene"));
}

TEST_F(AssetRefTest, GeneratedArtefactsDoNotGetMeta)
{
    /// @note 再生成できる派生物の GUID は原本から導出する。独自の .meta を持たせない。
    EXPECT_FALSE(AssetDatabase::ShouldHaveMeta("player.meta"));
    EXPECT_FALSE(AssetDatabase::ShouldHaveMeta("player.fzasset"));
    EXPECT_FALSE(AssetDatabase::ShouldHaveMeta("player.mesh"));
    EXPECT_FALSE(AssetDatabase::ShouldHaveMeta("player.skel"));
    EXPECT_FALSE(AssetDatabase::ShouldHaveMeta("blur.cso"));
    EXPECT_FALSE(AssetDatabase::ShouldHaveMeta("scripts.dll"));
    EXPECT_FALSE(AssetDatabase::ShouldHaveMeta("playercomponent.generated.hpp"));
}

TEST_F(AssetRefTest, AtomicSaveTempFilesDoNotGetMeta)
{
    /// @note 原子的な保存の途中経過。rename で消える相手に .meta を発行すると孤児が残る。
    ///       一時名は .tmp の «後ろ» に pid とハッシュが付くので、末尾拡張子だけでは弾けない。
    EXPECT_FALSE(AssetDatabase::ShouldHaveMeta("stage_01.scene.tmp.3588.b6c904700d2b"));
    EXPECT_FALSE(AssetDatabase::ShouldHaveMeta("keyicons.hpp.tmp.3588.44fddd02650a"));
    EXPECT_FALSE(AssetDatabase::ShouldHaveMeta("player.tmp"));

    /// @note 名前の一部として ".tmp." を含まない通常のアセットは巻き込まない。
    EXPECT_TRUE(AssetDatabase::ShouldHaveMeta("tmp_backup.scene"));
    EXPECT_TRUE(AssetDatabase::ShouldHaveMeta("effect.tmpl.mat"));
}

TEST_F(AssetRefTest, NonFileLikeNamesDoNotGetMeta)
{
    /// @note EncodeGuidRefs は文書中の全文字列にこの判定を掛ける。
    ///       «拡張子に見えるだけの値» をディスクへ問い合わせに行かせない。
    EXPECT_FALSE(AssetDatabase::ShouldHaveMeta("license"));
    EXPECT_FALSE(AssetDatabase::ShouldHaveMeta(".gitignore"));
    EXPECT_FALSE(AssetDatabase::ShouldHaveMeta("player.001"));
    EXPECT_FALSE(AssetDatabase::ShouldHaveMeta("value.with_underscore"));
}

TEST_F(AssetRefTest, AuthoredFoldersGetMetaAndGeneratedOnesDoNot)
{
    EXPECT_TRUE(AssetDatabase::ShouldHaveFolderMeta("Textures"));
    EXPECT_TRUE(AssetDatabase::ShouldHaveFolderMeta("Scenes"));

    EXPECT_FALSE(AssetDatabase::ShouldHaveFolderMeta(".git"));
    EXPECT_FALSE(AssetDatabase::ShouldHaveFolderMeta("Library"));
    EXPECT_FALSE(AssetDatabase::ShouldHaveFolderMeta("build"));
    EXPECT_FALSE(AssetDatabase::ShouldHaveFolderMeta("compiled"));
    EXPECT_FALSE(AssetDatabase::ShouldHaveFolderMeta(""));
}

TEST_F(AssetRefTest, FolderExclusionIgnoresCase)
{
    EXPECT_FALSE(AssetDatabase::ShouldHaveFolderMeta("LIBRARY"));
    EXPECT_FALSE(AssetDatabase::ShouldHaveFolderMeta("Build"));
}

/// @name 索引 (実ファイルを伴う)

/// Assets ルートを一時ディレクトリに作り、テストごとに索引を作り直す。
/// AssetDatabase はプロセス全体で 1 つの静的な索引を持つため、
/// TearDown で必ず Shutdown して次のテストへ持ち越さない。
class AssetDatabaseTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        std::filesystem::create_directories(AssetsRoot());
    }

    void TearDown() override
    {
        AssetDatabase::Shutdown();
        EngineFixture::TearDown();
    }

    std::filesystem::path AssetsRoot() const { return m_temp.File("Assets"); }

    std::string Utf8(const std::filesystem::path& path) const { return path.generic_string(); }

    void InitDatabase() { AssetDatabase::Init(Utf8(AssetsRoot())); }

private:
    testkit::TempDir m_temp{"assetdb"};
};

TEST_F(AssetDatabaseTest, GeneratesMetaForAssetsFoundDuringInit)
{
    const std::filesystem::path texture = AssetsRoot() / "Textures" / "Player.png";
    WriteFile(texture, "not really a png");

    InitDatabase();

    EXPECT_TRUE(std::filesystem::exists(texture.string() + ".meta"));
    EXPECT_TRUE(IsLowerHex32(AssetDatabase::TryGetGuidFromPath(Utf8(texture))));
}

TEST_F(AssetDatabaseTest, ResolvesTheGuidBackToItsPath)
{
    const std::filesystem::path texture = AssetsRoot() / "Textures" / "Player.png";
    WriteFile(texture, "x");
    InitDatabase();

    const std::string guid = AssetDatabase::TryGetGuidFromPath(Utf8(texture));
    ASSERT_FALSE(guid.empty());

    EXPECT_EQ(AssetDatabase::PathFromGuid(guid), Utf8(texture));
}

TEST_F(AssetDatabaseTest, ReusesTheGuidWrittenInAnExistingMeta)
{
    /// @note .meta は git 管理下。索引を作り直しても値を振り直してはいけない。
    const std::filesystem::path texture = AssetsRoot() / "Player.png";
    WriteFile(texture, "x");
    WriteFile(std::filesystem::path(texture.string() + ".meta"),
              "[meta]\nguid = \"0123456789abcdef0123456789abcdef\"\n");

    InitDatabase();

    EXPECT_EQ(AssetDatabase::TryGetGuidFromPath(Utf8(texture)),
              "0123456789abcdef0123456789abcdef");
}

TEST_F(AssetDatabaseTest, TryGetDoesNotCreateMetaForUnknownAssets)
{
    /// @note Import 前の FBX に .meta を発行させないための契約。
    InitDatabase();
    const std::filesystem::path model = AssetsRoot() / "Later.fbx";
    WriteFile(model, "x");

    const std::string guid = AssetDatabase::TryGetGuidFromPath(Utf8(model));

    EXPECT_TRUE(guid.empty());
    EXPECT_FALSE(std::filesystem::exists(model.string() + ".meta"));
}

TEST_F(AssetDatabaseTest, GuidFromPathHealsAnAssetAddedAfterInit)
{
    InitDatabase();
    const std::filesystem::path texture = AssetsRoot() / "Added.png";
    WriteFile(texture, "x");

    const std::string guid = AssetDatabase::GuidFromPath(Utf8(texture));

    EXPECT_TRUE(IsLowerHex32(guid));
    EXPECT_TRUE(std::filesystem::exists(texture.string() + ".meta"));
    EXPECT_EQ(AssetDatabase::PathFromGuid(guid), Utf8(texture));
}

TEST_F(AssetDatabaseTest, RefusesToInventAGuidForAMissingFile)
{
    InitDatabase();

    EXPECT_EQ(AssetDatabase::GuidFromPath(Utf8(AssetsRoot() / "Nope.png")), "");
}

TEST_F(AssetDatabaseTest, KeepsTheGuidWhenAnAssetIsRenamed)
{
    const std::filesystem::path before = AssetsRoot() / "Old.png";
    WriteFile(before, "x");
    InitDatabase();
    const std::string guid = AssetDatabase::TryGetGuidFromPath(Utf8(before));
    ASSERT_FALSE(guid.empty());

    const std::filesystem::path after = AssetsRoot() / "New.png";
    std::filesystem::rename(before, after);
    std::filesystem::rename(before.string() + ".meta", after.string() + ".meta");
    AssetDatabase::OnAssetMoved(Utf8(before), Utf8(after));

    EXPECT_EQ(AssetDatabase::PathFromGuid(guid), Utf8(after));
    EXPECT_EQ(AssetDatabase::TryGetGuidFromPath(Utf8(after)), guid);
}

TEST_F(AssetDatabaseTest, MovingAFolderRepointsEverythingUnderIt)
{
    const std::filesystem::path texture = AssetsRoot() / "Old" / "Player.png";
    WriteFile(texture, "x");
    InitDatabase();
    const std::string guid = AssetDatabase::TryGetGuidFromPath(Utf8(texture));
    ASSERT_FALSE(guid.empty());

    std::filesystem::rename(AssetsRoot() / "Old", AssetsRoot() / "New");
    AssetDatabase::OnAssetMoved(Utf8(AssetsRoot() / "Old"), Utf8(AssetsRoot() / "New"));

    EXPECT_EQ(AssetDatabase::PathFromGuid(guid), Utf8(AssetsRoot() / "New" / "Player.png"));
}

TEST_F(AssetDatabaseTest, ForgetsAnAssetThatWasDeleted)
{
    /// @note 旧 GUID を残すと、同じ場所へ作り直したアセットが古い参照を乗っ取る。
    const std::filesystem::path texture = AssetsRoot() / "Doomed.png";
    WriteFile(texture, "x");
    InitDatabase();
    const std::string guid = AssetDatabase::TryGetGuidFromPath(Utf8(texture));
    ASSERT_FALSE(guid.empty());

    std::filesystem::remove(texture);
    std::filesystem::remove(texture.string() + ".meta");
    AssetDatabase::OnAssetRemoved(Utf8(texture));

    EXPECT_EQ(AssetDatabase::PathFromGuid(guid), "");
    EXPECT_EQ(AssetDatabase::TryGetGuidFromPath(Utf8(texture)), "");
}

TEST_F(AssetDatabaseTest, DeletingAFolderForgetsEverythingUnderIt)
{
    const std::filesystem::path texture = AssetsRoot() / "Doomed" / "Player.png";
    WriteFile(texture, "x");
    InitDatabase();
    const std::string guid = AssetDatabase::TryGetGuidFromPath(Utf8(texture));
    ASSERT_FALSE(guid.empty());

    std::filesystem::remove_all(AssetsRoot() / "Doomed");
    AssetDatabase::OnAssetRemoved(Utf8(AssetsRoot() / "Doomed"));

    EXPECT_EQ(AssetDatabase::PathFromGuid(guid), "");
}

TEST_F(AssetDatabaseTest, AssetsRootAlwaysEndsWithASeparator)
{
    /// @note "Assets/..." 相対化はこの末尾スラッシュ前提で長さを引いている。
    InitDatabase();

    const std::string root = AssetDatabase::AssetsRoot();
    ASSERT_FALSE(root.empty());
    EXPECT_EQ(root.back(), '/');
}

TEST_F(AssetDatabaseTest, ProjectRootIsTheParentOfTheAssetsFolder)
{
    InitDatabase();

    const std::string projectRoot = AssetDatabase::ProjectRoot();

    EXPECT_EQ(projectRoot, AssetDatabase::AssetsRoot().substr(
                               0, AssetDatabase::AssetsRoot().size() - 7));
}


/// @name Library/Baked の掃除

TEST_F(AssetDatabaseTest, SweepRemovesBakedContainersNobodyClaims)
{
    const std::filesystem::path model = AssetsRoot() / "Models" / "Player.fbx";
    WriteFile(model, "fbx");
    WriteFile(model.string() + ".meta",
              "file_format_version = 1\n\n[meta]\n"
              "guid = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'\n");
    InitDatabase();

    const std::filesystem::path baked = AssetsRoot().parent_path() / "Library" / "Baked";
    WriteFile(baked / "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" / "Player.fzasset", "live");
    WriteFile(baked / "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" / "Old.fzasset", "orphan");
    /// @note guid 以外の名前は掃除の対象外。
    WriteFile(baked / "notaguid" / "keep.txt", "keep");

    const auto result = AssetDatabase::SweepOrphanedBaked(/*dryRun=*/false);

    EXPECT_FALSE(result.aborted);
    EXPECT_EQ(result.removed, 1u);
    EXPECT_TRUE(std::filesystem::exists(baked / "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    EXPECT_FALSE(std::filesystem::exists(baked / "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
    EXPECT_TRUE(std::filesystem::exists(baked / "notaguid"));
}

TEST_F(AssetDatabaseTest, SweepDryRunReportsWithoutDeleting)
{
    const std::filesystem::path model = AssetsRoot() / "Models" / "Player.fbx";
    WriteFile(model, "fbx");
    WriteFile(model.string() + ".meta",
              "file_format_version = 1\n\n[meta]\n"
              "guid = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'\n");
    InitDatabase();

    const std::filesystem::path baked = AssetsRoot().parent_path() / "Library" / "Baked";
    WriteFile(baked / "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" / "Old.fzasset", "orphan");

    const auto result = AssetDatabase::SweepOrphanedBaked(/*dryRun=*/true);

    EXPECT_EQ(result.removed, 1u);
    EXPECT_TRUE(std::filesystem::exists(baked / "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
}

TEST_F(AssetDatabaseTest, SweepRefusesToRunWithoutAnIndex)
{
    /// @note 索引を作らずに呼ぶ。«誰も名乗っていない» の判定ができないので 1 件も消さない。
    const std::filesystem::path baked = AssetsRoot().parent_path() / "Library" / "Baked";
    WriteFile(baked / "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" / "Old.fzasset", "orphan");

    const auto result = AssetDatabase::SweepOrphanedBaked(/*dryRun=*/false);

    EXPECT_TRUE(result.aborted);
    EXPECT_EQ(result.removed, 0u);
    EXPECT_TRUE(std::filesystem::exists(baked / "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
}


/// @name 同じ guid を名乗る 2 つの実体

TEST_F(AssetDatabaseTest, CopiesOfTheSameAssetAreNotReportedAsDuplicates)
{
    /// @note エンジンは共通シェーダーをプロジェクトの Assets へ配る。.meta ごとコピーされるので
    ///       guid も同じになるが、これは «同じアセットが 2 箇所にある» だけで衝突ではない。
    const std::string guid = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const std::string meta =
        "file_format_version = 1\n\n[meta]\nguid = '" + guid + "'\n";

    const std::filesystem::path projShader = AssetsRoot() / "Shaders" / "Lit.hlsl";
    WriteFile(projShader, "shader");
    WriteFile(projShader.string() + ".meta", meta);
    InitDatabase();

    /// @note エンジン側は Init の走査範囲外。参照解決で «後から» 索引に載る。
    const std::filesystem::path engineShader =
        AssetsRoot().parent_path() / "EngineAssets" / "Assets" / "Shaders" / "Lit.hlsl";
    WriteFile(engineShader, "shader");
    WriteFile(engineShader.string() + ".meta", meta);

    EXPECT_EQ(AssetDatabase::TryGetGuidFromPath(Utf8(engineShader)), guid);
    EXPECT_EQ(AssetDatabase::GuidConflictCount(), 0u);
    /// @note 実体として引けるのはプロジェクト側。
    EXPECT_EQ(AssetDatabase::PathFromGuid(guid), Utf8(projShader));
}

TEST_F(AssetDatabaseTest, DifferentAssetsSharingAGuidAreStillReported)
{
    /// @note Assets からの相対パスが違う = 別のアセットが同じ guid を名乗っている。
    ///       これは参照が黙って他人へ吸われる本物の衝突なので、報告しなければならない。
    const std::string guid = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const std::string meta =
        "file_format_version = 1\n\n[meta]\nguid = '" + guid + "'\n";

    const std::filesystem::path first  = AssetsRoot() / "Textures" / "Player.png";
    const std::filesystem::path second = AssetsRoot() / "Textures" / "Enemy.png";
    WriteFile(first,  "a");
    WriteFile(first.string()  + ".meta", meta);
    WriteFile(second, "b");
    WriteFile(second.string() + ".meta", meta);

    InitDatabase();

    EXPECT_EQ(AssetDatabase::GuidConflictCount(), 1u);
}


TEST_F(AssetDatabaseTest, TheSameFileUnderTwoSpellingsIsNotADuplicate)
{
    /// @note 索引には絶対パスが載るが、参照解決は大文字小文字や区切りの違う表記で入ってくる。
    ///       同じ実体なら «重複» ではないので、警告も衝突記録もしてはいけない。
    const std::string guid = "cccccccccccccccccccccccccccccccc";
    const std::filesystem::path texture = AssetsRoot() / "Textures" / "Player.png";
    WriteFile(texture, "png");
    WriteFile(texture.string() + ".meta",
              "file_format_version = 1\n\n[meta]\nguid = '" + guid + "'\n");
    InitDatabase();

    /// @note 同じファイルを «別表記» で引く (区切りをバックスラッシュに変えたもの)。
    std::string spelled = Utf8(texture);
    std::replace(spelled.begin(), spelled.end(), '/', '\\');

    EXPECT_EQ(AssetDatabase::TryGetGuidFromPath(spelled), guid);
    EXPECT_EQ(AssetDatabase::GuidConflictCount(), 0u);
    EXPECT_EQ(AssetDatabase::PathFromGuid(guid), Utf8(texture));
}

} // namespace fbzz::tests
