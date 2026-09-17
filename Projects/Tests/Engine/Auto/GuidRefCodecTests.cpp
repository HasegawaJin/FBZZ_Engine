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
#include <Engine/Asset/GuidRefCodec.hpp>

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

    std::filesystem::path AssetsRoot() const { return m_temp.File("Assets"); }

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

    void InitDatabase() { asset::AssetDatabase::Init(AssetsRoot().generic_string()); }

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
