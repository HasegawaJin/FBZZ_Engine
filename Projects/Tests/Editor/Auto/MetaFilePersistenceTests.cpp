/// @file    MetaFilePersistenceTests.cpp
/// @brief   メタデータのLF保存・同値保存の抑止とGUID・参照の維持を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-06
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Import/FbxMetaSerializer.hpp>
#include <Editor/Util/EditorSceneState.hpp>
#include <Editor/Util/EditorSerializer.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/MetaFile.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <chrono>
#include <filesystem>
#include <string>

namespace fbzz::tests {
namespace {

constexpr const char* GUID = "0123456789abcdef0123456789abcdef";
constexpr const char* REFERENCE = "guid:fedcba9876543210fedcba9876543210|Assets/日本語.mat";

std::string SeedMeta()
{
    return std::string("# @note 保持するコメント。\n[meta]\nguid = '") + GUID
        + "'\n\n[custom]\nreference = '" + REFERENCE + "'\n";
}

std::filesystem::file_time_type Marker()
{
    return std::filesystem::file_time_type{std::chrono::seconds{1'000'000'000}};
}

} /// @note namespace

class MetaFilePersistenceTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        asset::AssetDatabase::Init(util::FileSystem::PathToUtf8(ProjectRoot() / "Assets"));
    }

    void TearDown() override
    {
        asset::AssetDatabase::Shutdown();
        EditorFixture::TearDown();
    }

    std::string Path(const char* name) const
    {
        return util::FileSystem::PathToUtf8(
            ProjectRoot() / util::FileSystem::PathFromUtf8(std::string("Assets/") + name));
    }

    std::string Bytes(const std::string& path) const
    {
        std::string result;
        EXPECT_TRUE(util::FileSystem::ReadText(util::FileSystem::PathFromUtf8(path), result));
        return result;
    }

    void Write(const std::string& path, const std::string& text) const
    {
        ASSERT_TRUE(util::FileSystem::WriteTextAtomic(path, text));
    }

    void PinWriteTime(const std::string& path) const
    {
        std::error_code error;
        std::filesystem::last_write_time(util::FileSystem::PathFromUtf8(path), Marker(), error);
        ASSERT_FALSE(error) << error.message();
    }

    void ExpectUnchanged(const std::string& path, const std::string& expected) const
    {
        EXPECT_EQ(Bytes(path), expected);
        EXPECT_EQ(std::filesystem::last_write_time(util::FileSystem::PathFromUtf8(path)), Marker());
    }

    void ExpectIdentity(const std::string& path) const
    {
        const std::string bytes = Bytes(path);
        EXPECT_EQ(bytes.find('\r'), std::string::npos);
        const auto parsed = toml::parse(bytes);
        ASSERT_TRUE(parsed);
        EXPECT_EQ(parsed.table()["meta"]["guid"].value_or(std::string{}), GUID);
        EXPECT_EQ(parsed.table()["custom"]["reference"].value_or(std::string{}), REFERENCE);
    }
};

TEST_F(MetaFilePersistenceTest, NormalizesCrLfOnceWithoutChangingCommentsOrTomlValues)
{
    const std::string path = Path("日本語.png.meta");
    const std::string expected = SeedMeta();
    std::string crlf;
    for (const char value : expected) {
        if (value == '\n') crlf.push_back('\r');
        crlf.push_back(value);
    }
    Write(path, crlf);
    const auto parsed = toml::parse(expected);
    ASSERT_TRUE(parsed);
    const std::string equivalent = std::string("[custom]\nreference = \"") + REFERENCE
        + "\"\n[meta]\nguid = \"" + GUID + "\"\n";

    ASSERT_TRUE(asset::MetaFile::Save(path, equivalent));
    EXPECT_EQ(Bytes(path), expected);
    PinWriteTime(path);
    ASSERT_TRUE(asset::MetaFile::Save(path, equivalent));
    ExpectUnchanged(path, expected);
    ExpectIdentity(path);
}

TEST_F(MetaFilePersistenceTest, CreatesNewMetadataWithLfEvenWhenInputUsesCrLf)
{
    const std::string path = Path("New.png.meta");
    ASSERT_TRUE(asset::MetaFile::Save(path, "[meta]\r\nguid = '0123456789abcdef0123456789abcdef'\r\n"));
    EXPECT_EQ(Bytes(path), "[meta]\nguid = '0123456789abcdef0123456789abcdef'\n");
}

TEST_F(MetaFilePersistenceTest, RejectsInvalidExistingOrIncomingTomlWithoutReplacingOriginal)
{
    const std::string path = Path("Broken.png.meta");
    const std::string broken = "[meta]\nguid = 'original'\n[broken\n";
    Write(path, broken);
    PinWriteTime(path);

    EXPECT_FALSE(asset::MetaFile::Save(path, SeedMeta()));
    ExpectUnchanged(path, broken);
    Write(path, SeedMeta());
    PinWriteTime(path);
    EXPECT_FALSE(asset::MetaFile::Save(path, broken));
    ExpectUnchanged(path, SeedMeta());
}

TEST_F(MetaFilePersistenceTest, ModelSettingsOnlyWriteWhenValuesChangeAndKeepOtherSections)
{
    const std::string source = Path("Model.fbx");
    const std::string path = source + ".meta";
    Write(path, SeedMeta());
    editor::FbxImportOptions options;
    ASSERT_TRUE(editor::FbxMetaSerializer::SaveOptions(source, options));
    const std::string expected = "# @note 保持するコメント。\n" + Bytes(path);
    Write(path, expected);
    PinWriteTime(path);

    ASSERT_TRUE(editor::FbxMetaSerializer::SaveOptions(source, options));
    ExpectUnchanged(path, expected);
    options.generateNormals = !options.generateNormals;
    ASSERT_TRUE(editor::FbxMetaSerializer::SaveOptions(source, options));
    editor::FbxImportOptions actual;
    ASSERT_TRUE(editor::FbxMetaSerializer::LoadOptions(source, actual));
    EXPECT_EQ(actual.generateNormals, options.generateNormals);
    ExpectIdentity(path);
}

TEST_F(MetaFilePersistenceTest, ModelSourceTimestampChangesOnlyUpdateTheLocalCache)
{
    const std::string source = Path("Model.fbx");
    const std::string path = source + ".meta";
    Write(source, "unchanged model bytes");
    Write(path, SeedMeta());
    editor::FbxImportOptions options;
    ASSERT_TRUE(editor::FbxMetaSerializer::SaveCacheInfo(source, options));
    const std::string expected = Bytes(path);
    PinWriteTime(path);
    PinWriteTime(source);

    ASSERT_TRUE(editor::FbxMetaSerializer::SaveCacheInfo(source, options));
    ExpectUnchanged(path, expected);
    ExpectIdentity(path);
    const auto cache = editor::FbxMetaSerializer::LoadCacheInfo(source);
    EXPECT_EQ(cache.contentHash, editor::FbxMetaSerializer::SourceContentHash(source));
    const auto parsed = toml::parse(expected);
    ASSERT_TRUE(parsed);
    EXPECT_FALSE(parsed.table().contains("cache"));
}

TEST_F(MetaFilePersistenceTest, TextureSettingsKeepSpriteIdsAndReferencesAcrossRepeatedSaves)
{
    const std::string path = Path("Sprite.png.meta");
    Write(path, SeedMeta());
    asset::TextureAsset texture;
    texture.settings.type = asset::TextureType::Sprite;
    asset::SpriteRect sprite;
    sprite.id = "sprite-existing-identity";
    sprite.name = "日本語";
    texture.settings.sprites.push_back(sprite);
    asset::TexDescSerializer serializer;
    ASSERT_TRUE(serializer.Save(texture, path));
    const std::string expected = Bytes(path);
    PinWriteTime(path);

    ASSERT_TRUE(serializer.Save(texture, path));
    ExpectUnchanged(path, expected);
    texture.settings.flipGreen = !texture.settings.flipGreen;
    ASSERT_TRUE(serializer.Save(texture, path));
    asset::TextureAsset loaded;
    ASSERT_TRUE(serializer.Load(path, loaded));
    ASSERT_EQ(loaded.settings.sprites.size(), 1u);
    EXPECT_EQ(loaded.settings.sprites.front().id, sprite.id);
    EXPECT_EQ(loaded.settings.flipGreen, texture.settings.flipGreen);
    ExpectIdentity(path);
}

TEST_F(MetaFilePersistenceTest, SceneMetadataHasStableOrderWithoutReorderingComponents)
{
    const std::string scene = Path("Stable.scene");
    const std::string otherScene = Path("Other.scene");
    Write(scene + ".meta", SeedMeta());
    Write(otherScene + ".meta", SeedMeta());
    editor::EditorSceneState first;
    first.SetComponentOrder("object-b", {"Water", "Transform"});
    first.SetComponentOrder("object-a", {"Terrain", "Terrain Collider"});
    first.SetHiddenObjects({"object-b", "object-a"});
    first.SetLockedObjects({"object-b", "object-a"});
    editor::EditorSceneState second;
    second.SetComponentOrder("object-a", {"Terrain", "Terrain Collider"});
    second.SetComponentOrder("object-b", {"Water", "Transform"});
    second.SetHiddenObjects({"object-a", "object-b"});
    second.SetLockedObjects({"object-a", "object-b"});

    ASSERT_TRUE(editor::EditorSerializer::Save(first, scene));
    ASSERT_TRUE(editor::EditorSerializer::Save(second, otherScene));
    const std::string expected = Bytes(scene + ".meta");
    EXPECT_EQ(Bytes(otherScene + ".meta"), expected);
    const auto parsed = toml::parse(expected);
    ASSERT_TRUE(parsed);
    const auto* objects = parsed.table()["editor"]["game_objects"].as_array();
    ASSERT_NE(objects, nullptr);
    ASSERT_EQ(objects->size(), 2u);
    const auto* firstObject = (*objects)[0].as_table();
    const auto* secondObject = (*objects)[1].as_table();
    ASSERT_NE(firstObject, nullptr);
    ASSERT_NE(secondObject, nullptr);
    EXPECT_EQ((*firstObject)["instance_id"].value_or(std::string{}), "object-a");
    EXPECT_EQ((*secondObject)["instance_id"].value_or(std::string{}), "object-b");
    PinWriteTime(scene + ".meta");
    ASSERT_TRUE(editor::EditorSerializer::Save(second, scene));
    ExpectUnchanged(scene + ".meta", expected);
    editor::EditorSceneState loaded;
    ASSERT_TRUE(editor::EditorSerializer::Load(loaded, scene));
    EXPECT_EQ(loaded.GetComponentOrder("object-b"), first.GetComponentOrder("object-b"));
    EXPECT_EQ(loaded.GetComponentOrder("object-a"), first.GetComponentOrder("object-a"));
    ExpectIdentity(scene + ".meta");
}

} /// @note namespace fbzz::tests
