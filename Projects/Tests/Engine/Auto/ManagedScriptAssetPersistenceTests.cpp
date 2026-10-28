/// @file    ManagedScriptAssetPersistenceTests.cpp
/// @brief   C# 設定の GUID 値と native コンポーネントのアセットパスが保存境界で混同されないことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <system_error>
#include <vector>
#include <toml++/toml.hpp>

namespace fbzz::tests {

class ManagedScriptAssetPersistenceTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        std::error_code error;
        std::filesystem::create_directories(m_temp.File("Assets/Materials"), error);
        ASSERT_FALSE(error);
        m_assetPath = m_temp.File("Assets/Materials/Benchmark.mat").generic_string();
        ASSERT_TRUE(util::FileSystem::WriteText(m_assetPath,
            "version = 1\nshader = 'Assets/Shaders/Material/Surface/Phong.hlsl'\n"
            "render_path = 'auto'\nmesh_type = 'surface'\n[params]\nalbedo = [1.0, 1.0, 1.0, 1.0]\n"));
        asset::AssetDatabase::Init(m_temp.File("Assets").generic_string());
        m_assetGuid = asset::AssetDatabase::TryGetGuidFromPath(m_assetPath);
        ASSERT_FALSE(m_assetGuid.empty());
        ASSERT_EQ(asset::AssetDatabase::PathFromGuid(m_assetGuid), m_assetPath);
        m_reference = "guid:" + m_assetGuid;
    }

    void TearDown() override
    {
        asset::AssetDatabase::Shutdown();
        EngineFixture::TearDown();
    }

    scene::GameObject& AddOwner(scene::Scene& source)
    {
        auto& owner = source.CreateGameObject("ManagedAssetOwner");
        /// @note Decal のデータ読取は GPU や Material importer を要求せず、native の参照変換を直接観測できる。
        owner.AddComponent<scene::DecalComponent>().materialPath = m_assetPath;
        auto& scripts = owner.AddComponent<scene::ScriptComponent>();
        for (const char* type : {"cs:missing-managed-asset-persistence", "cs-internal:MissingManagedAssetPersistence"}) {
            auto& entry = scripts.scripts.emplace_back();
            const std::string fields = "visualMaterial = '" + m_reference + "'\n"
                "pathLiteral = '" + m_assetPath + "'\n"
                "[nested]\nmaterial = '" + m_reference + "'\n";
            entry.serialized = std::make_unique<scene::SerializedScriptData>(
                scene::SerializedScriptData{type, fields, true, entry.savedScriptId});
        }
        return owner;
    }

    void ExpectPreservedSettings(scene::GameObject& owner)
    {
        auto* scripts = owner.GetComponent<scene::ScriptComponent>();
        ASSERT_NE(scripts, nullptr);
        ASSERT_EQ(scripts->scripts.size(), 2u);
        for (auto& entry : scripts->scripts) {
            EXPECT_EQ(entry.script.get(), nullptr);
            ASSERT_NE(entry.serialized, nullptr);
            auto fields = toml::parse(entry.serialized->fieldsToml);
            ASSERT_TRUE(fields);
            EXPECT_EQ(fields.table()["visualMaterial"].value<std::string>(), m_reference);
            EXPECT_EQ(fields.table()["pathLiteral"].value<std::string>(), m_assetPath);
            EXPECT_EQ(fields.table()["nested"]["material"].value<std::string>(), m_reference);
        }
    }

    void ExpectNativeReferenceEncoded(const std::string& text)
    {
        auto document = toml::parse(text);
        ASSERT_TRUE(document);
        const auto* objects = document.table()["gameobjects"].as_array();
        ASSERT_NE(objects, nullptr);
        ASSERT_EQ(objects->size(), 1u);
        const auto* owner = (*objects)[0].as_table();
        ASSERT_NE(owner, nullptr);
        const auto reference = (*owner)["DecalComponent"]["material"].value<std::string>();
        ASSERT_TRUE(reference.has_value());
        EXPECT_TRUE(asset::AssetDatabase::IsGuidRef(*reference));
        EXPECT_EQ(asset::AssetDatabase::GuidFromRef(*reference), m_assetGuid);
    }

    testkit::TempDir m_temp{"managed-asset-persistence"};
    std::string m_assetPath;
    std::string m_assetGuid;
    std::string m_reference;
};

TEST_F(ManagedScriptAssetPersistenceTest, MissingManagedSettingsKeepGuidValuesAndLiteralPathsAcrossSceneSaveAndLoad)
{
    scene::Scene source;
    auto& owner = AddOwner(source);
    const std::string ownerGuid = owner.instanceId;
    const std::string path = m_temp.File("Assets/Benchmark.scene").generic_string();
    ASSERT_TRUE(scene::SceneSerializer::Save(source, path));
    std::string text;
    ASSERT_TRUE(util::FileSystem::ReadText(path, text));
    ExpectNativeReferenceEncoded(text);

    auto loaded = scene::SceneSerializer::LoadData(path);
    ASSERT_NE(loaded, nullptr);
    auto* loadedOwner = loaded->FindByGuid(ownerGuid);
    ASSERT_NE(loadedOwner, nullptr);
    ExpectPreservedSettings(*loadedOwner);
    auto* decal = loadedOwner->GetComponent<scene::DecalComponent>();
    ASSERT_NE(decal, nullptr);
    EXPECT_EQ(decal->materialPath, "Assets/Materials/Benchmark.mat");

    const std::string resaved = scene::SceneSerializer::SaveToText(*loaded);
    ASSERT_FALSE(resaved.empty());
    auto reopened = scene::SceneSerializer::LoadDataFromText(resaved, path);
    ASSERT_NE(reopened, nullptr);
    auto* reopenedOwner = reopened->FindByGuid(ownerGuid);
    ASSERT_NE(reopenedOwner, nullptr);
    ExpectPreservedSettings(*reopenedOwner);
}

TEST_F(ManagedScriptAssetPersistenceTest, AppendedManagedSettingsKeepGuidValuesAndUnrelatedObjects)
{
    scene::Scene source;
    AddOwner(source);
    const std::string text = scene::SceneSerializer::SaveToText(source);
    ASSERT_FALSE(text.empty());
    scene::Scene destination;
    auto& unrelated = destination.CreateGameObject("Unrelated");
    const auto unrelatedId = unrelated.GetID();
    std::vector<scene::EntityID> roots;

    ASSERT_TRUE(scene::SceneSerializer::AppendObjects(destination, text, nullptr, roots));
    ASSERT_EQ(roots.size(), 1u);
    auto* appended = destination.GetGameObject(roots.front());
    ASSERT_NE(appended, nullptr);
    /// @note Decal は AppendObjects の復元対象外なので、native 参照の往復は Save/Load 側で検証する。
    ExpectPreservedSettings(*appended);
    EXPECT_EQ(destination.GetGameObject(unrelatedId), &unrelated);
}

} /// @note namespace fbzz::tests
