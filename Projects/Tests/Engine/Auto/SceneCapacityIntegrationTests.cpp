/// @file    SceneCapacityIntegrationTests.cpp
/// @brief   スクリプト生成とシーン復元が容量不足を既存状態の無変更で拒否する契約。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

class CapacityScriptProbe final : public scene::Script {
public:
    int runtimeCounter = 37;
    int serializeCalls = 0;
    void OnBeforeSerialize() override { ++serializeCalls; }
};

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
    bool FillLeaving(std::size_t available)
    {
        while (m_scene.RemainingEntityCapacity() > available)
            if (!m_scene.TryCreateGameObject("Existing")) return false;
        return true;
    }

    scene::Scene m_scene;
};

TEST_F(SceneCapacityIntegrationTest, ScriptCreationReportsDetachedAndFullScenesWithoutChangingOwner)
{
    CapacityScriptProbe detached;
    EXPECT_EQ(detached.scene.Create("Detached"), nullptr);
    EXPECT_FALSE(detached.scene.CanCreate());

    auto& owner = m_scene.CreateGameObject("Owner");
    CapacityScriptProbe probe;
    probe.SetContext(&m_scene, &owner);
    ASSERT_TRUE(FillLeaving(0));

    EXPECT_FALSE(probe.scene.CanCreate());
    EXPECT_TRUE(probe.scene.CanCreate(0));
    EXPECT_EQ(probe.scene.Create("Rejected"), nullptr);
    scene::AudioLoop loop;
    EXPECT_FALSE(probe.audio.UpdateLoop(loop, std::string_view{"Assets/Audio/Loop.wav"}, 0.5f));
    EXPECT_FALSE(loop.IsValid());
    const auto decal = probe.decal.Spawn({1.0f, 0.0f, 2.0f}, math::Vector3::UP, {}, 1.0f);
    EXPECT_EQ(decal.Resolve(m_scene), nullptr);
    EXPECT_EQ(owner.GetChildCount(), 0);
    EXPECT_EQ(m_scene.GameObjectCount(), scene::Scene::MAX_ENTITIES);
    EXPECT_EQ(m_scene.Find("Rejected"), nullptr);
    EXPECT_EQ(probe.runtimeCounter, 37);
}

TEST_F(SceneCapacityIntegrationTest, AppendPreflightKeepsUnserializedScriptAndHierarchyState)
{
    auto& owner = m_scene.CreateGameObject("Owner");
    const auto ownerId = owner.GetID();
    auto& child = m_scene.CreateGameObject("Child");
    child.SetParent(owner);
    ASSERT_EQ(child.GetParent(), &owner);
    auto probe = std::make_unique<CapacityScriptProbe>();
    auto* existingScript = probe.get();
    probe->SetContext(&m_scene, &owner);
    scene::ScriptEntry entry;
    entry.script = std::move(probe);
    owner.AddComponent<scene::ScriptComponent>().scripts.push_back(std::move(entry));
    ASSERT_TRUE(FillLeaving(1));
    const auto count = m_scene.GameObjectCount();
    std::vector<scene::EntityID> roots{ownerId};

    EXPECT_FALSE(scene::SceneSerializer::AppendObjects(m_scene, SceneText(2), nullptr, roots));

    EXPECT_TRUE(roots.empty());
    EXPECT_EQ(m_scene.GameObjectCount(), count);
    EXPECT_EQ(m_scene.RemainingEntityCapacity(), 1u);
    EXPECT_EQ(m_scene.GetGameObject(ownerId), &owner);
    EXPECT_EQ(owner.GetChildCount(), 1);
    EXPECT_EQ(child.GetParent(), &owner);
    EXPECT_EQ(owner.GetComponent<scene::ScriptComponent>()->scripts.front().script.get(), existingScript);
    EXPECT_EQ(existingScript->runtimeCounter, 37);
    EXPECT_EQ(existingScript->serializeCalls, 0);
    EXPECT_EQ(m_scene.Find("Object0"), nullptr);
}

TEST_F(SceneCapacityIntegrationTest, AppendUsesAllRemainingCapacityAndReturnsCreatedRoots)
{
    ASSERT_TRUE(FillLeaving(2));
    std::vector<scene::EntityID> roots;

    ASSERT_TRUE(scene::SceneSerializer::AppendObjects(m_scene, SceneText(2), nullptr, roots));

    ASSERT_EQ(roots.size(), 2u);
    EXPECT_NE(m_scene.GetGameObject(roots[0]), nullptr);
    EXPECT_NE(m_scene.GetGameObject(roots[1]), nullptr);
    EXPECT_EQ(m_scene.GameObjectCount(), scene::Scene::MAX_ENTITIES);
    EXPECT_FALSE(scene::SceneSerializer::AppendObjects(m_scene, SceneText(1), nullptr, roots));
    EXPECT_TRUE(roots.empty());
}

TEST_F(SceneCapacityIntegrationTest, LoadingOverCapacityFailsAndSkippedRuntimeNamesDoNotConsumeCapacity)
{
    EXPECT_EQ(scene::SceneSerializer::LoadDataFromText(SceneText(scene::Scene::MAX_ENTITIES + 1), {}), nullptr);
    const std::string text = SceneText(scene::Scene::MAX_ENTITIES)
        + "[[gameobjects]]\nname = '__RuntimeGenerated'\n";

    auto loaded = scene::SceneSerializer::LoadDataFromText(text, {});

    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GameObjectCount(), scene::Scene::MAX_ENTITIES);
    EXPECT_EQ(loaded->Find("__RuntimeGenerated"), nullptr);
}

} /// @note namespace fbzz::tests
