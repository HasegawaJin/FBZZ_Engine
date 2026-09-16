/// @file    SpriteReferenceTests.cpp
/// @brief   Sprite 参照文字列の組み立て・分解と、ID / 名前どちらでも引けることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// 参照は "<画像パス>::sprite::<ID または名前>" の 1 本の文字列で流通する。
/// 分解に失敗したときに «Sprite 参照ではない» と答えず途中で切ってしまうと、
/// 画像パスが壊れて絵が出ない。逆に引けなかったときに黙って全面を返すと、
/// アトラスの 1 枚を指したはずが «アトラス全体» に化ける ── どちらも無言で壊れる。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/TextureAsset.hpp>

#include <string>

namespace fbzz::tests {
namespace {

asset::SpriteRect MakeRect(std::string id, std::string name)
{
    asset::SpriteRect rect;
    rect.id     = std::move(id);
    rect.name   = std::move(name);
    rect.width  = 32;
    rect.height = 32;
    return rect;
}

} // namespace

class SpriteReferenceTest : public testkit::EngineFixture {};

// --- 組み立てと分解 ---------------------------------------------------------

TEST_F(SpriteReferenceTest, RoundTripsThroughMakeAndParse)
{
    const std::string reference =
        asset::MakeSpriteReference("Assets/UI/Atlas.png", "Key_W");

    std::string texturePath;
    std::string token;
    ASSERT_TRUE(asset::ParseSpriteReference(reference, texturePath, token));

    EXPECT_EQ(texturePath, "Assets/UI/Atlas.png");
    EXPECT_EQ(token, "Key_W");
}

TEST_F(SpriteReferenceTest, KeepsThePathIntactWhenTheStringIsNotASpriteReference)
{
    // «Sprite 参照ではない» と答えるときも、入力のパスはそのまま返す。
    // ここで空にすると、ただのテクスチャ参照が全部壊れる。
    std::string texturePath;
    std::string token;

    EXPECT_FALSE(asset::ParseSpriteReference("Assets/UI/Atlas.png", texturePath, token));
    EXPECT_EQ(texturePath, "Assets/UI/Atlas.png");
    EXPECT_TRUE(token.empty());
}

TEST_F(SpriteReferenceTest, RejectsAMarkerWithNothingAfterIt)
{
    std::string texturePath;
    std::string token;

    EXPECT_FALSE(asset::ParseSpriteReference("Assets/UI/Atlas.png::sprite::",
                                             texturePath, token));
    EXPECT_EQ(texturePath, "Assets/UI/Atlas.png::sprite::");
}

TEST_F(SpriteReferenceTest, RejectsAMarkerWithNothingBeforeIt)
{
    std::string texturePath;
    std::string token;

    EXPECT_FALSE(asset::ParseSpriteReference("::sprite::Key_W", texturePath, token));
}

TEST_F(SpriteReferenceTest, AcceptsATokenThatContainsColons)
{
    // ID は UUID、名前は人が付ける。区切りの後ろは最後まで丸ごとトークンとして扱う。
    std::string texturePath;
    std::string token;
    ASSERT_TRUE(asset::ParseSpriteReference("Assets/A.png::sprite::a:b:c", texturePath, token));

    EXPECT_EQ(token, "a:b:c");
}

TEST_F(SpriteReferenceTest, ParsesAnEmptyStringAsNotAReference)
{
    std::string texturePath;
    std::string token;

    EXPECT_FALSE(asset::ParseSpriteReference("", texturePath, token));
    EXPECT_TRUE(texturePath.empty());
}

// --- 検索 -------------------------------------------------------------------

TEST_F(SpriteReferenceTest, FindsASpriteById)
{
    asset::TextureImportSettings settings;
    settings.sprites.push_back(MakeRect("836d2b4d", "Key_W"));
    settings.sprites.push_back(MakeRect("0f1e2d3c", "Key_A"));

    const asset::SpriteRect* found = asset::FindSprite(settings, "0f1e2d3c");

    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "Key_A");
}

TEST_F(SpriteReferenceTest, FindsASpriteByName)
{
    // ID は UUID なので人も AI も手では書けない。名前でも引けないと、
    // 手で書いた参照が «エラーにならずアトラス全面» に化ける。
    asset::TextureImportSettings settings;
    settings.sprites.push_back(MakeRect("836d2b4d", "Key_W"));

    const asset::SpriteRect* found = asset::FindSprite(settings, "Key_W");

    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->id, "836d2b4d");
}

TEST_F(SpriteReferenceTest, PrefersTheIdWhenANameCollidesWithAnotherId)
{
    // ID を先に見る契約。名前が偶然ほかの ID と一致しても、ID の方を返す。
    asset::TextureImportSettings settings;
    settings.sprites.push_back(MakeRect("alpha", "beta"));
    settings.sprites.push_back(MakeRect("beta", "gamma"));

    const asset::SpriteRect* found = asset::FindSprite(settings, "beta");

    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "gamma");
}

TEST_F(SpriteReferenceTest, FindsNothingForAnUnknownToken)
{
    asset::TextureImportSettings settings;
    settings.sprites.push_back(MakeRect("836d2b4d", "Key_W"));

    EXPECT_EQ(asset::FindSprite(settings, "Key_Q"), nullptr);
}

TEST_F(SpriteReferenceTest, FindsNothingForAnEmptyToken)
{
    // 空の id / name を持つ矩形に «空トークン» が当たると、無関係な 1 枚を返してしまう。
    asset::TextureImportSettings settings;
    settings.sprites.push_back(MakeRect("", ""));

    EXPECT_EQ(asset::FindSprite(settings, ""), nullptr);
}

TEST_F(SpriteReferenceTest, FindsNothingWhenThereAreNoSprites)
{
    const asset::TextureImportSettings settings;

    EXPECT_EQ(asset::FindSprite(settings, "Key_W"), nullptr);
}

// --- 型の推定 ---------------------------------------------------------------

TEST_F(SpriteReferenceTest, GuessesTheTextureTypeFromTheFileNameSuffix)
{
    EXPECT_EQ(asset::GuessTextureType("wall_n.png"), asset::TextureType::Normal);
    EXPECT_EQ(asset::GuessTextureType("sky.hdr"), asset::TextureType::HDR);
    EXPECT_EQ(asset::GuessTextureType("button_ui.png"), asset::TextureType::UI);
    EXPECT_EQ(asset::GuessTextureType("ui_panel.png"), asset::TextureType::UI);
}

TEST_F(SpriteReferenceTest, GuessingIgnoresCase)
{
    EXPECT_EQ(asset::GuessTextureType("WALL_N.PNG"), asset::TextureType::Normal);
    EXPECT_EQ(asset::GuessTextureType("Sky.HDR"), asset::TextureType::HDR);
}

TEST_F(SpriteReferenceTest, DefaultSettingsMatchTheGuessedType)
{
    // 法線マップを sRGB で読むと明るさが変わる。既定値がここで決まる。
    const asset::TextureImportSettings normal =
        asset::DefaultSettingsForType(asset::TextureType::Normal);
    const asset::TextureImportSettings color =
        asset::DefaultSettingsForType(asset::TextureType::Color);

    EXPECT_FALSE(normal.srgb);
    EXPECT_TRUE(color.srgb);
    EXPECT_EQ(normal.type, asset::TextureType::Normal);
}

} // namespace fbzz::tests
