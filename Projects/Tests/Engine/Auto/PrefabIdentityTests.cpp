/// @file    PrefabIdentityTests.cpp
/// @brief   プレファブの GUID・適用済み定義・プールの識別子が保存と移動を跨いで保たれることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>
#include <Engine/Scene/PrefabPool.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::tests {

class PrefabIdentityTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        std::filesystem::create_directories(m_temp.File("Assets"));
        m_path = m_temp.File("Assets/Original.prefab").generic_string();
        ASSERT_TRUE(util::FileSystem::WriteText(m_path, "version = 1\n"));
        asset::AssetDatabase::Init(m_temp.File("Assets").generic_string());
        m_reference = scene::CanonicalPrefabAssetRef(m_path);
        ASSERT_TRUE(asset::AssetDatabase::IsGuidRef(m_reference));
    }

    void TearDown() override
    {
        scene::PrefabPool::ClearAll();
        asset::AssetDatabase::Shutdown();
        EngineFixture::TearDown();
    }

    std::string MoveAsset()
    {
        const std::string destination = m_temp.File("Assets/Renamed.prefab").generic_string();
        std::filesystem::rename(m_path, destination);
        std::filesystem::rename(m_path + ".meta", destination + ".meta");
        asset::AssetDatabase::OnAssetMoved(m_path, destination);
        return destination;
    }

    testkit::TempDir m_temp{"prefab-identity"};
    std::string m_path;
    std::string m_reference;
};

TEST_F(PrefabIdentityTest, SceneRoundTripPreservesGuidAndOpaqueSourceSnapshot)
{
    scene::Scene source;
    auto& object = source.CreateGameObject("Instance");
    object.prefabAssetPath = m_reference;
    object.prefabSourceId = "source-root";
    object.prefabSourceSnapshot = "[[gameobjects]]\ninstanceId = 'source-root'\n"
                                  "prefabAssetPath = 'Assets/Original.prefab'\n";
    const std::string snapshot = object.prefabSourceSnapshot;
    const std::string instanceId = object.instanceId;
    const std::string text = scene::SceneSerializer::SaveToText(source);
    auto restored = scene::SceneSerializer::LoadDataFromText(text, m_temp.File("test.scene").generic_string());

    ASSERT_NE(restored, nullptr);
    const auto* loaded = restored->FindByGuid(instanceId);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->prefabAssetPath, m_reference);
    EXPECT_EQ(loaded->prefabSourceId, "source-root");
    EXPECT_EQ(loaded->prefabSourceSnapshot, snapshot);
}

TEST_F(PrefabIdentityTest, AppendPreservesPrefabMetadataAndUnrelatedObjects)
{
    scene::Scene source;
    auto& object = source.CreateGameObject("Instance");
    object.prefabAssetPath = m_reference;
    object.prefabSourceSnapshot = "source = 'Assets/Original.prefab'\n";
    const std::string snapshot = object.prefabSourceSnapshot;
    const std::string text = scene::SceneSerializer::SaveToText(source);
    scene::Scene destination;
    auto& unrelated = destination.CreateGameObject("Unrelated");
    const auto unrelatedId = unrelated.GetID();
    std::vector<scene::EntityID> roots;

    ASSERT_TRUE(scene::SceneSerializer::AppendObjects(destination, text, nullptr, roots));
    ASSERT_EQ(roots.size(), 1u);
    const auto* loaded = destination.GetGameObject(roots.front());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->prefabAssetPath, m_reference);
    EXPECT_EQ(loaded->prefabSourceSnapshot, snapshot);
    EXPECT_EQ(destination.GetGameObject(unrelatedId), &unrelated);
}

TEST_F(PrefabIdentityTest, LegacyAssetsPathIsPromotedToRegisteredGuid)
{
    const auto canonical = scene::CanonicalPrefabAssetRef("Assets/Original.prefab");
    EXPECT_EQ(scene::PrefabAssetKey(canonical), scene::PrefabAssetKey(m_reference));

    scene::Scene source;
    source.CreateGameObject("Legacy").prefabAssetPath = "Assets/Original.prefab";
    const auto text = scene::SceneSerializer::SaveToText(source);
    auto restored = scene::SceneSerializer::LoadDataFromText(text, m_temp.File("test.scene").generic_string());
    ASSERT_NE(restored, nullptr);
    const auto* object = restored->Find("Legacy");
    ASSERT_NE(object, nullptr);
    EXPECT_TRUE(asset::AssetDatabase::IsGuidRef(object->prefabAssetPath));
    EXPECT_EQ(scene::PrefabAssetKey(object->prefabAssetPath), scene::PrefabAssetKey(m_reference));
}

TEST_F(PrefabIdentityTest, OldGuidHintStillResolvesAfterMoveAndSceneReload)
{
    scene::Scene source;
    source.CreateGameObject("Instance").prefabAssetPath = m_reference;
    const auto text = scene::SceneSerializer::SaveToText(source);
    const auto movedPath = MoveAsset();
    const auto updatedReference = scene::CanonicalPrefabAssetRef(movedPath);
    EXPECT_EQ(scene::PrefabAssetKey(m_reference), scene::PrefabAssetKey(updatedReference));
    EXPECT_EQ(scene::ResolvePrefabAssetPath(m_reference), movedPath);

    auto restored = scene::SceneSerializer::LoadDataFromText(text, m_temp.File("test.scene").generic_string());
    ASSERT_NE(restored, nullptr);
    const auto* object = restored->Find("Instance");
    ASSERT_NE(object, nullptr);
    EXPECT_EQ(scene::ResolvePrefabAssetPath(object->prefabAssetPath), movedPath);
}

TEST_F(PrefabIdentityTest, MissingGuidKeepsIdentityAndNeverUsesExistingHint)
{
    const std::string missing = "guid:ffffffffffffffffffffffffffffffff|Assets/Original.prefab";
    EXPECT_TRUE(scene::ResolvePrefabAssetPath(missing).empty());
    scene::Scene source;
    source.CreateGameObject("Missing").prefabAssetPath = missing;
    const auto text = scene::SceneSerializer::SaveToText(source);
    auto restored = scene::SceneSerializer::LoadDataFromText(text, m_temp.File("test.scene").generic_string());
    ASSERT_NE(restored, nullptr);
    const auto* object = restored->Find("Missing");
    ASSERT_NE(object, nullptr);
    EXPECT_EQ(object->prefabAssetPath, missing);
    EXPECT_TRUE(scene::ResolvePrefabAssetPath(object->prefabAssetPath).empty());
}

TEST_F(PrefabIdentityTest, PoolReusesSameBucketAndLimitAcrossGuidHintChanges)
{
    scene::Scene source;
    auto& object = source.CreateGameObject("Pooled");
    object.prefabAssetPath = m_reference;
    auto& child = source.CreateGameObject("Child");
    child.SetParent(&object);
    child.transform.position = { 1.0f, 0.0f, 0.0f };
    scene::PrefabPool::SetLimit(m_reference, 2);
    ASSERT_TRUE(scene::PrefabPool::Despawn(source, object));
    const auto updatedReference = scene::CanonicalPrefabAssetRef(MoveAsset());

    EXPECT_EQ(scene::PrefabPool::AvailableCount(source, updatedReference), 1u);
    EXPECT_EQ(scene::PrefabPool::GetLimit(updatedReference), 2);
    auto* spawned = scene::PrefabPool::Spawn(source, updatedReference,
                                             { 5.0f, 0.0f, 0.0f }, math::Quaternion::Identity());
    ASSERT_EQ(spawned, &object);
    EXPECT_VEC3_NEAR(spawned->transform.worldPosition, (math::Vector3{ 5.0f, 0.0f, 0.0f }), 1e-5f);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{ 6.0f, 0.0f, 0.0f }), 1e-5f);
    EXPECT_EQ(scene::PrefabPool::LiveCount(source, m_reference), 1u);
    EXPECT_EQ(scene::PrefabPool::AvailableCount(source, m_reference), 0u);
}

} /// @note namespace fbzz::tests
