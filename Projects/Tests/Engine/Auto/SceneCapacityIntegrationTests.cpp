/// @file    SceneCapacityIntegrationTests.cpp
/// @brief   Scene の読込と追記が旧固定容量を超えて既存状態を保持する契約。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

std::string SceneText(std::size_t count)
{
    std::string text;
    for (std::size_t index = 0; index < count; ++index)
        text += "[[gameobjects]]\nname = 'Object" + std::to_string(index) + "'\n";
    return text;
}

} /// @note namespace

class SceneCapacityIntegrationTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
};

TEST_F(SceneCapacityIntegrationTest, AppendGrowsBeyondOldCapacityAndPreservesExistingIdentity)
{
    auto& existing = m_scene.CreateGameObject("Existing");
    const auto id = existing.GetID();
    const auto guid = existing.instanceId;
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(scene::SceneSerializer::AppendObjects(m_scene, SceneText(4097), nullptr, roots));

    ASSERT_EQ(roots.size(), 4097u);
    EXPECT_EQ(m_scene.GameObjectCount(), 4098u);
    EXPECT_EQ(m_scene.GetGameObject(id), &existing);
    EXPECT_EQ(existing.instanceId, guid);
    EXPECT_NE(m_scene.GetGameObject(roots.front()), nullptr);
    EXPECT_NE(m_scene.GetGameObject(roots.back()), nullptr);
    ASSERT_TRUE(scene::SceneSerializer::AppendObjects(m_scene, SceneText(2), nullptr, roots));
    EXPECT_EQ(roots.size(), 2u);
    EXPECT_EQ(m_scene.GameObjectCount(), 4100u);
}

TEST_F(SceneCapacityIntegrationTest, LoadingGrowsAndSkippedRuntimeNamesDoNotConsumeSlots)
{
    const std::string text = SceneText(8193) + "[[gameobjects]]\nname = '__RuntimeGenerated'\n";
    auto loaded = scene::SceneSerializer::LoadDataFromText(text, {});
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GameObjectCount(), 8193u);
    EXPECT_EQ(loaded->EntitySlotCount(), 8193u);
    EXPECT_NE(loaded->Find("Object8192"), nullptr);
    EXPECT_EQ(loaded->Find("__RuntimeGenerated"), nullptr);
}

} /// @note namespace fbzz::tests
