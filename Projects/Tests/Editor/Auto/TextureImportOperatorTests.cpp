/// @file    TextureImportOperatorTests.cpp
/// @brief   画面なしのテクスチャ設定編集・GUID保全・原子性・履歴を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Playtest/ImageCompare.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <fstream>

namespace fbzz::tests {
class TextureImportOperatorTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        Context().projectRoot = ProjectRoot().generic_string();
        std::ofstream(File("Assets/Color.png")) << "settings-only";
        asset::AssetDatabase::Init(File("Assets").generic_string());
        editor::RegisterTextureImportOperators(m_registry);
    }
    void TearDown() override
    {
        m_undo.Clear(); m_registry.Clear(); asset::AssetDatabase::Shutdown();
        EditorFixture::TearDown();
    }
    editor::OpResult Configure(const editor::OpData& patch)
    {
        editor::OpArgs args; args.Set("path", std::string("Assets/Color.png")); args.Set("settings", patch);
        editor::OpContext context{Context(), m_undo};
        return m_registry.Invoke("asset.texture.configure", context, args);
    }
    asset::TextureImportSettings Settings()
    {
        asset::TextureAsset texture;
        EXPECT_TRUE(asset::TexDescSerializer{}.Load(File("Assets/Color.png.meta").generic_string(), texture));
        return texture.settings;
    }
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
};
TEST_F(TextureImportOperatorTest, PartialEditPreservesGuidAndSupportsUndoRedo)
{
    const std::string guid = asset::AssetDatabase::GuidFromPath(File("Assets/Color.png").generic_string());
    auto patch = editor::OpData::MakeObject(); patch.Set("maxSize", editor::OpData(1024));
    ASSERT_TRUE(Configure(patch).ok);
    EXPECT_EQ(Settings().maxSize, 1024u);
    EXPECT_EQ(asset::AssetDatabase::GuidFromPath(File("Assets/Color.png").generic_string()), guid);
    ASSERT_TRUE(m_undo.Undo()); EXPECT_EQ(Settings().maxSize, 4096u);
    ASSERT_TRUE(m_undo.Redo()); EXPECT_EQ(Settings().maxSize, 1024u);
}
TEST_F(TextureImportOperatorTest, UnknownFieldsRejectAllChanges)
{
    auto patch = editor::OpData::MakeObject(); patch.Set("maxSize", editor::OpData(1024));
    patch.Set("unknown", editor::OpData(true));
    EXPECT_FALSE(Configure(patch).ok); EXPECT_FALSE(m_undo.CanUndo());
    asset::TextureImportSettings settings;
    EXPECT_FALSE(asset::GetCachedTextureImportSettings(File("Assets/Color.png").generic_string(), settings));
}
TEST_F(TextureImportOperatorTest, IncompatibleCompressionAndNegativeNumbersAreRejected)
{
    auto patch = editor::OpData::MakeObject(); patch.Set("type", editor::OpData("Normal"));
    patch.Set("compression", editor::OpData("BC1"));
    EXPECT_FALSE(Configure(patch).ok);
    patch = editor::OpData::MakeObject(); patch.Set("maxSize", editor::OpData(-1));
    EXPECT_FALSE(Configure(patch).ok); EXPECT_FALSE(m_undo.CanUndo());
}
TEST_F(TextureImportOperatorTest, SpritePivotAndBorderEditsKeepStableIdentity)
{
    auto patch = editor::OpData::MakeObject(); patch.Set("type", editor::OpData("Sprite"));
    auto sprites = editor::OpData::MakeArray(); auto sprite = editor::OpData::MakeObject();
    sprite.Set("name", editor::OpData("Hero")); sprite.Set("width", editor::OpData(32));
    sprite.Set("height", editor::OpData(32)); sprite.Set("pivotX", editor::OpData(0.25f));
    sprite.Set("borderLeft", editor::OpData(4.0f)); sprites.Push(sprite); patch.Set("sprites", sprites);
    ASSERT_TRUE(Configure(patch).ok);
    auto first = Settings(); ASSERT_EQ(first.sprites.size(), 1u);
    EXPECT_FALSE(first.sprites[0].id.empty()); EXPECT_FLOAT_EQ(first.sprites[0].pivotX, 0.25f);
    EXPECT_FLOAT_EQ(first.sprites[0].borderLeft, 4.0f);
    const std::string id = first.sprites[0].id;
    patch = editor::OpData::MakeObject(); patch.Set("filter", editor::OpData("Point"));
    ASSERT_TRUE(Configure(patch).ok); EXPECT_EQ(Settings().sprites[0].id, id);
    ASSERT_TRUE(m_undo.Undo()); EXPECT_EQ(Settings().sprites[0].id, id);
}
TEST_F(TextureImportOperatorTest, UndoConflictPreservesExternalChanges)
{
    auto patch = editor::OpData::MakeObject(); patch.Set("maxSize", editor::OpData(1024));
    ASSERT_TRUE(Configure(patch).ok);
    asset::TextureAsset external; external.settings = Settings(); external.settings.maxSize = 512;
    ASSERT_TRUE(asset::TexDescSerializer{}.Save(external, File("Assets/Color.png.meta").generic_string()));
    EXPECT_FALSE(m_undo.Undo()); EXPECT_EQ(Settings().maxSize, 512u); EXPECT_EQ(m_undo.GetCursor(), 1u);
}
TEST_F(TextureImportOperatorTest, AssetsBoundaryRejectsTraversal)
{
    std::ofstream(File("Outside.png")) << "settings-only";
    editor::OpArgs args; args.Set("path", std::string("Assets/../Outside.png"));
    editor::OpContext context{Context(), m_undo};
    EXPECT_FALSE(m_registry.Invoke("asset.texture.inspect", context, args).ok);
}
TEST_F(TextureImportOperatorTest, ChangingTextureTypeKeepsAuthoredSpriteRectanglesAndIds)
{
    auto patch = editor::OpData::MakeObject(); patch.Set("type", editor::OpData("Sprite"));
    auto sprites = editor::OpData::MakeArray(); auto sprite = editor::OpData::MakeObject();
    sprite.Set("name", editor::OpData("Hero")); sprite.Set("width", editor::OpData(32)); sprite.Set("height", editor::OpData(32));
    sprites.Push(sprite); patch.Set("sprites", sprites); ASSERT_TRUE(Configure(patch).ok);
    const auto original = Settings(); ASSERT_EQ(original.sprites.size(), 1u); const std::string id = original.sprites[0].id;
    patch = editor::OpData::MakeObject(); patch.Set("type", editor::OpData("Color")); ASSERT_TRUE(Configure(patch).ok);
    const auto inactive = Settings(); ASSERT_EQ(inactive.sprites.size(), 1u); EXPECT_EQ(inactive.sprites[0].id, id);
    patch.Set("type", editor::OpData("Sprite")); ASSERT_TRUE(Configure(patch).ok);
    const auto restored = Settings(); ASSERT_EQ(restored.sprites.size(), 1u); EXPECT_EQ(restored.sprites[0].id, id);
    EXPECT_EQ(restored.sprites[0].width, 32u);
}
TEST_F(TextureImportOperatorTest, TrimUsesDecodedAlphaAndKeepsSpriteIdentityThroughUndo)
{
    editor::playtest::RgbaImage image; image.width = 8; image.height = 8; image.pixels.resize(8 * 8 * 4, 0);
    for (std::size_t y = 1; y < 7; ++y) for (std::size_t x = 2; x < 6; ++x) image.pixels[(y * 8 + x) * 4 + 3] = 255;
    std::vector<uint8_t> png; ASSERT_TRUE(editor::playtest::EncodePng(image, png));
    ASSERT_TRUE(util::FileSystem::WriteBinaryAtomic(File("Assets/Color.png"), png.data(), png.size()));
    asset::TextureAsset texture; texture.settings = asset::DefaultSettingsForType(asset::TextureType::Sprite);
    asset::SpriteRect sprite; sprite.id = "frame-id"; sprite.name = "Frame"; sprite.pivotX = 0.25f; sprite.borderLeft = 6;
    texture.settings.sprites.push_back(sprite);
    ASSERT_TRUE(asset::TexDescSerializer{}.Save(texture, File("Assets/Color.png.meta").generic_string()));
    editor::OpArgs args; args.Set("path", std::string("Assets/Color.png")); args.Set("sprite", sprite.id);
    editor::OpContext context{Context(), m_undo}; ASSERT_TRUE(m_registry.Invoke("asset.texture.trim", context, args).ok);
    const auto result = Settings().sprites.front(); EXPECT_EQ(result.id, sprite.id); EXPECT_FLOAT_EQ(result.pivotX, 0.25f);
    EXPECT_EQ(result.x, 2u); EXPECT_EQ(result.y, 1u); EXPECT_EQ(result.width, 4u); EXPECT_EQ(result.height, 6u); EXPECT_FLOAT_EQ(result.borderLeft, 4);
    ASSERT_TRUE(m_undo.Undo()); EXPECT_EQ(Settings().sprites.front().width, 0u);
}
} /// @note namespace fbzz::tests
