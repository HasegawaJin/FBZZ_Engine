/// @file    SceneCapacityTests.cpp
/// @brief   シーンの容量不足を無変更で拒否し、解放済み Entity を再利用する契約。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::tests {

class SceneCapacityTest : public testkit::EngineFixture {
protected:
    bool FillToCapacity()
    {
        m_ids.reserve(scene::Scene::MAX_ENTITIES);
        m_objects.reserve(scene::Scene::MAX_ENTITIES);
        for (uint32_t index = 0; index < scene::Scene::MAX_ENTITIES; ++index) {
            auto* object = m_scene.TryCreateGameObject("Existing");
            if (!object) return false;
            object->AddComponent<scene::LightComponent>().intensity = static_cast<float>(index);
            m_ids.push_back(object->GetID());
            m_objects.push_back(object);
        }
        return true;
    }

    scene::Scene m_scene;
    std::vector<scene::EntityID> m_ids;
    std::vector<scene::GameObject*> m_objects;
};

TEST_F(SceneCapacityTest, CapacityQueriesDoNotReserveOrOverflow)
{
    EXPECT_EQ(m_scene.RemainingEntityCapacity(), scene::Scene::MAX_ENTITIES);
    EXPECT_TRUE(m_scene.CanCreateGameObjects(0));
    EXPECT_TRUE(m_scene.CanCreateGameObjects(scene::Scene::MAX_ENTITIES));
    EXPECT_FALSE(m_scene.CanCreateGameObjects(scene::Scene::MAX_ENTITIES + 1));
    EXPECT_FALSE(m_scene.CanCreateGameObjects(std::numeric_limits<size_t>::max()));
    EXPECT_EQ(m_scene.GameObjectCount(), 0u);

    auto* object = m_scene.TryCreateGameObject("First");

    ASSERT_NE(object, nullptr);
    EXPECT_EQ(object->GetID().index, 0u);
    EXPECT_EQ(object->GetID().generation, 1u);
    EXPECT_EQ(m_scene.RemainingEntityCapacity(), scene::Scene::MAX_ENTITIES - 1);
    EXPECT_TRUE(m_scene.CanCreateGameObjects());
    EXPECT_FALSE(m_scene.CanCreateGameObjects(scene::Scene::MAX_ENTITIES));
}

TEST_F(SceneCapacityTest, RejectsCreationAtCapacityWithoutChangingExistingObjects)
{
    ASSERT_TRUE(FillToCapacity());
    ASSERT_TRUE(m_objects[1]->SetParent(m_objects[0]));
    m_objects[1]->SetActive(false);
    m_objects[0]->transform.position = {1.0f, 2.0f, 3.0f};
    const std::string rootGuid = m_objects[0]->instanceId;
    const auto sceneGeneration = m_scene.GetRenderSceneGeneration();

    EXPECT_EQ(m_scene.TryCreateGameObject("Rejected"), nullptr);
    EXPECT_EQ(m_scene.TryCreateGameObject("RejectedAgain"), nullptr);

    EXPECT_EQ(m_scene.GameObjectCount(), scene::Scene::MAX_ENTITIES);
    EXPECT_EQ(m_scene.RemainingEntityCapacity(), 0u);
    EXPECT_TRUE(m_scene.CanCreateGameObjects(0));
    EXPECT_FALSE(m_scene.CanCreateGameObjects());
    EXPECT_EQ(m_scene.GetRenderSceneGeneration(), sceneGeneration);
    EXPECT_EQ(m_scene.Find("Rejected"), nullptr);
    EXPECT_EQ(m_scene.FindByGuid(rootGuid), m_objects[0]);
    EXPECT_EQ(m_objects[0]->GetChildCount(), 1);
    EXPECT_EQ(m_objects[1]->GetParent(), m_objects[0]);
    EXPECT_FALSE(m_objects[1]->activeSelf());
    EXPECT_VEC3_NEAR(m_objects[0]->transform.position, math::Vector3(1.0f, 2.0f, 3.0f), testkit::kTolerance);
    EXPECT_EQ(m_scene.GetEntities<scene::LightComponent>().size(), scene::Scene::MAX_ENTITIES);
    for (uint32_t index = 0; index < scene::Scene::MAX_ENTITIES; ++index) {
        EXPECT_EQ(m_scene.GetGameObject(m_ids[index]), m_objects[index]);
        ASSERT_NE(m_scene.GetComponent<scene::LightComponent>(m_ids[index]), nullptr);
        EXPECT_FLOAT_EQ(m_scene.GetComponent<scene::LightComponent>(m_ids[index])->intensity,
                        static_cast<float>(index));
    }
}

TEST_F(SceneCapacityTest, ReusesFreedSlotsAfterRejectionWithoutConsumingExtraGenerations)
{
    ASSERT_TRUE(FillToCapacity());
    const auto firstRemoved = m_ids[11];
    const auto secondRemoved = m_ids.back();
    EXPECT_EQ(m_scene.TryCreateGameObject(), nullptr);
    ASSERT_TRUE(m_scene.DestroyGameObject(firstRemoved));
    ASSERT_TRUE(m_scene.DestroyGameObject(secondRemoved));
    EXPECT_EQ(m_scene.RemainingEntityCapacity(), 2u);
    EXPECT_TRUE(m_scene.CanCreateGameObjects(2));
    EXPECT_FALSE(m_scene.CanCreateGameObjects(3));

    auto* firstReplacement = m_scene.TryCreateGameObject("FirstReplacement");
    ASSERT_NE(firstReplacement, nullptr);
    auto* secondReplacement = m_scene.TryCreateGameObject("SecondReplacement");
    ASSERT_NE(secondReplacement, nullptr);

    EXPECT_EQ(firstReplacement->GetID().index, secondRemoved.index);
    EXPECT_EQ(firstReplacement->GetID().generation, secondRemoved.generation + 2);
    EXPECT_EQ(secondReplacement->GetID().index, firstRemoved.index);
    EXPECT_EQ(secondReplacement->GetID().generation, firstRemoved.generation + 2);
    EXPECT_FALSE(m_scene.IsValid(firstRemoved));
    EXPECT_FALSE(m_scene.IsValid(secondRemoved));
    EXPECT_EQ(m_scene.GetGameObject(firstRemoved), nullptr);
    EXPECT_EQ(m_scene.GetComponent<scene::LightComponent>(secondRemoved), nullptr);
    EXPECT_EQ(firstReplacement->GetComponent<scene::LightComponent>(), nullptr);
    firstReplacement->AddComponent<scene::LightComponent>().intensity = 8.0f;
    secondReplacement->AddComponent<scene::LightComponent>().intensity = 9.0f;
    EXPECT_EQ(m_scene.GetEntities<scene::LightComponent>().size(), scene::Scene::MAX_ENTITIES);
    EXPECT_EQ(m_scene.RemainingEntityCapacity(), 0u);
    EXPECT_EQ(m_scene.TryCreateGameObject("StillFull"), nullptr);
    EXPECT_FLOAT_EQ(firstReplacement->GetComponent<scene::LightComponent>()->intensity, 8.0f);
    EXPECT_FLOAT_EQ(secondReplacement->GetComponent<scene::LightComponent>()->intensity, 9.0f);
    EXPECT_EQ(m_scene.GetGameObject(m_ids[0]), m_objects[0]);
}

TEST_F(SceneCapacityTest, InvalidEntityQueriesRemainSafeAtCapacity)
{
    ASSERT_TRUE(FillToCapacity());
    const scene::EntityID beyondCapacity{scene::Scene::MAX_ENTITIES, 1};
    const scene::EntityID invalidIds[] = {scene::EntityID::INVALID, beyondCapacity};

    for (const auto id : invalidIds) {
        EXPECT_FALSE(m_scene.IsValid(id));
        EXPECT_EQ(m_scene.GetGameObject(id), nullptr);
        EXPECT_EQ(m_scene.GetComponent<scene::LightComponent>(id), nullptr);
        EXPECT_FALSE(m_scene.HasComponent<scene::LightComponent>(id));
        EXPECT_FALSE(m_scene.DestroyGameObject(id));
    }

    EXPECT_EQ(m_scene.GameObjectCount(), scene::Scene::MAX_ENTITIES);
    EXPECT_EQ(m_scene.GetEntities<scene::LightComponent>().size(), scene::Scene::MAX_ENTITIES);
    EXPECT_EQ(m_scene.RemainingEntityCapacity(), 0u);
}

TEST_F(SceneCapacityTest, QueuedDestructionDoesNotFreeCapacityBeforeItRuns)
{
    ASSERT_TRUE(FillToCapacity());
    const auto queuedId = m_ids.back();
    scene::GameObject::Destroy(*m_objects.back(), 1.0f);

    EXPECT_EQ(m_scene.RemainingEntityCapacity(), 0u);
    EXPECT_EQ(m_scene.TryCreateGameObject(), nullptr);
    m_scene.FlushDestroyQueue(0.5f);
    EXPECT_EQ(m_scene.RemainingEntityCapacity(), 0u);
    EXPECT_EQ(m_scene.GetGameObject(queuedId), m_objects.back());
    m_scene.FlushDestroyQueue(0.5f);

    EXPECT_EQ(m_scene.RemainingEntityCapacity(), 1u);
    EXPECT_EQ(m_scene.GetGameObject(queuedId), nullptr);
    auto* replacement = m_scene.TryCreateGameObject("AfterQueuedDestroy");
    ASSERT_NE(replacement, nullptr);
    EXPECT_EQ(replacement->GetID().index, queuedId.index);
    EXPECT_EQ(replacement->GetID().generation, queuedId.generation + 2);
}

TEST_F(SceneCapacityTest, ClearAndMovePreserveCapacityAccounting)
{
    ASSERT_TRUE(FillToCapacity());
    const auto removed = m_ids.back();
    ASSERT_TRUE(m_scene.DestroyGameObject(removed));
    const auto liveId = m_ids.front();
    auto* liveObject = m_objects.front();

    scene::Scene moved(std::move(m_scene));

    EXPECT_EQ(moved.RemainingEntityCapacity(), 1u);
    EXPECT_EQ(moved.GetGameObject(liveId), liveObject);
    auto* replacement = moved.TryCreateGameObject("MovedReplacement");
    ASSERT_NE(replacement, nullptr);
    EXPECT_EQ(replacement->GetID().index, removed.index);
    EXPECT_EQ(replacement->GetID().generation, removed.generation + 2);
    EXPECT_EQ(moved.TryCreateGameObject(), nullptr);
    EXPECT_EQ(m_scene.RemainingEntityCapacity(), scene::Scene::MAX_ENTITIES);
    EXPECT_EQ(m_scene.GameObjectCount(), 0u);

    moved.Clear();

    EXPECT_EQ(moved.RemainingEntityCapacity(), scene::Scene::MAX_ENTITIES);
    EXPECT_TRUE(moved.GetEntities<scene::LightComponent>().empty());
    EXPECT_TRUE(moved.CanCreateGameObjects(scene::Scene::MAX_ENTITIES));
    ASSERT_NE(moved.TryCreateGameObject("AfterClear"), nullptr);
    EXPECT_EQ(moved.RemainingEntityCapacity(), scene::Scene::MAX_ENTITIES - 1);
}

} /// @note namespace fbzz::tests
