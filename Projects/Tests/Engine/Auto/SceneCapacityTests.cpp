/// @file    SceneCapacityTests.cpp
/// @brief   Scene と Component の動的拡張、Entity 再利用、寿命保持の契約。
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
    static constexpr uint32_t GROWTH_COUNT = 8193;
    bool CreatePopulation()
    {
        m_ids.reserve(GROWTH_COUNT);
        m_objects.reserve(GROWTH_COUNT);
        m_lifetimes.reserve(GROWTH_COUNT);
        for (uint32_t index = 0; index < GROWTH_COUNT; ++index) {
            auto* object = m_scene.TryCreateGameObject("Existing");
            if (!object) return false;
            object->AddComponent<scene::LightComponent>().intensity = static_cast<float>(index);
            m_ids.push_back(object->GetID());
            m_objects.push_back(object);
            m_lifetimes.push_back(m_scene.GetComponentLifetime<scene::LightComponent>(object->GetID()));
        }
        return true;
    }
    scene::Scene m_scene;
    std::vector<scene::EntityID> m_ids;
    std::vector<scene::GameObject*> m_objects;
    std::vector<uint64_t> m_lifetimes;
};

TEST_F(SceneCapacityTest, CapacityQueriesUseEntityRepresentationWithoutReserving)
{
    EXPECT_EQ(m_scene.EntitySlotCount(), 0u);
    EXPECT_EQ(m_scene.RemainingEntityCapacity(), static_cast<size_t>(scene::EntityID::INVALID_INDEX));
    EXPECT_TRUE(m_scene.CanCreateGameObjects(0));
    EXPECT_TRUE(m_scene.CanCreateGameObjects(100000));
    EXPECT_TRUE(m_scene.CanCreateGameObjects(scene::Scene::MAX_ENTITIES));
    EXPECT_FALSE(m_scene.CanCreateGameObjects(static_cast<size_t>(scene::Scene::MAX_ENTITIES) + 1));
    EXPECT_FALSE(m_scene.CanCreateGameObjects(std::numeric_limits<size_t>::max()));
    EXPECT_EQ(m_scene.GameObjectCount(), 0u);
    EXPECT_EQ(m_scene.EntitySlotCount(), 0u);

    auto* object = m_scene.TryCreateGameObject("First");
    ASSERT_NE(object, nullptr);
    EXPECT_EQ(object->GetID().index, 0u);
    EXPECT_EQ(object->GetID().generation, 1u);
    EXPECT_EQ(m_scene.EntitySlotCount(), 1u);
    EXPECT_EQ(m_scene.RemainingEntityCapacity(), static_cast<size_t>(scene::Scene::MAX_ENTITIES) - 1);
    EXPECT_FALSE(m_scene.CanCreateGameObjects(scene::Scene::MAX_ENTITIES));
}

TEST_F(SceneCapacityTest, GrowthPreservesObjectsHierarchyComponentsAndLifetimes)
{
    ASSERT_TRUE(CreatePopulation());
    ASSERT_TRUE(m_objects[1]->SetParent(m_objects[0]));
    m_objects[1]->SetActive(false);
    m_objects[0]->transform.position = {1.0f, 2.0f, 3.0f};
    const std::string rootGuid = m_objects[0]->instanceId;
    const auto sceneGeneration = m_scene.GetRenderSceneGeneration();

    ASSERT_NE(m_scene.TryCreateGameObject("BeyondPopulation"), nullptr);

    EXPECT_EQ(m_scene.GameObjectCount(), GROWTH_COUNT + 1u);
    EXPECT_EQ(m_scene.EntitySlotCount(), GROWTH_COUNT + 1u);
    EXPECT_EQ(m_scene.GetRenderSceneGeneration(), sceneGeneration);
    EXPECT_EQ(m_scene.FindByGuid(rootGuid), m_objects[0]);
    EXPECT_EQ(m_objects[0]->GetChildCount(), 1);
    EXPECT_EQ(m_objects[1]->GetParent(), m_objects[0]);
    EXPECT_FALSE(m_objects[1]->activeSelf());
    EXPECT_VEC3_NEAR(m_objects[0]->transform.position, math::Vector3(1.0f, 2.0f, 3.0f), testkit::kTolerance);
    EXPECT_EQ(m_scene.GetEntities<scene::LightComponent>().size(), GROWTH_COUNT);
    for (uint32_t index = 0; index < GROWTH_COUNT; ++index) {
        EXPECT_EQ(m_scene.GetGameObject(m_ids[index]), m_objects[index]);
        ASSERT_NE(m_scene.GetComponent<scene::LightComponent>(m_ids[index]), nullptr);
        EXPECT_FLOAT_EQ(m_scene.GetComponent<scene::LightComponent>(m_ids[index])->intensity, static_cast<float>(index));
        EXPECT_NE(m_lifetimes[index], 0u);
        EXPECT_EQ(m_scene.GetComponentLifetime<scene::LightComponent>(m_ids[index]), m_lifetimes[index]);
    }
}

TEST_F(SceneCapacityTest, FreedSlotsReuseGenerationsAndIssueNewComponentLifetimes)
{
    ASSERT_TRUE(CreatePopulation());
    const auto firstRemoved = m_ids[11];
    const auto secondRemoved = m_ids.back();
    const size_t slots = m_scene.EntitySlotCount();
    ASSERT_TRUE(m_scene.DestroyGameObject(firstRemoved));
    ASSERT_TRUE(m_scene.DestroyGameObject(secondRemoved));

    auto* firstReplacement = m_scene.TryCreateGameObject("FirstReplacement");
    ASSERT_NE(firstReplacement, nullptr);
    auto* secondReplacement = m_scene.TryCreateGameObject("SecondReplacement");
    ASSERT_NE(secondReplacement, nullptr);

    EXPECT_EQ(m_scene.EntitySlotCount(), slots);
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
    EXPECT_NE(m_scene.GetComponentLifetime<scene::LightComponent>(firstReplacement->GetID()), m_lifetimes.back());
    EXPECT_NE(m_scene.GetComponentLifetime<scene::LightComponent>(secondReplacement->GetID()), m_lifetimes[11]);
    EXPECT_EQ(m_scene.GetEntities<scene::LightComponent>().size(), GROWTH_COUNT);
    EXPECT_EQ(m_scene.GetComponentLifetime<scene::LightComponent>(m_ids[0]), m_lifetimes[0]);
}

TEST_F(SceneCapacityTest, InvalidEntityQueriesRemainSafeBeyondAllocatedSlots)
{
    ASSERT_TRUE(CreatePopulation());
    const scene::EntityID invalidIds[] = {
        scene::EntityID::INVALID, {GROWTH_COUNT + 1000, 1},
        {scene::EntityID::INVALID_INDEX - 1, 1},
        {m_ids.front().index, m_ids.front().generation + 1}
    };
    for (const auto id : invalidIds) {
        EXPECT_FALSE(m_scene.IsValid(id));
        EXPECT_EQ(m_scene.GetGameObject(id), nullptr);
        EXPECT_EQ(m_scene.GetComponent<scene::LightComponent>(id), nullptr);
        EXPECT_FALSE(m_scene.HasComponent<scene::LightComponent>(id));
        EXPECT_FALSE(m_scene.DestroyGameObject(id));
        EXPECT_EQ(m_scene.GetComponentLifetime<scene::LightComponent>(id), 0u);
    }
    EXPECT_EQ(m_scene.GameObjectCount(), GROWTH_COUNT);
    EXPECT_EQ(m_scene.EntitySlotCount(), GROWTH_COUNT);
}

TEST_F(SceneCapacityTest, QueuedDestructionKeepsObjectLiveUntilCleanupThenReusesSlot)
{
    ASSERT_TRUE(CreatePopulation());
    const auto queuedId = m_ids.back();
    const size_t available = m_scene.RemainingEntityCapacity();
    scene::GameObject::Destroy(*m_objects.back(), 1.0f);
    EXPECT_EQ(m_scene.RemainingEntityCapacity(), available);
    m_scene.FlushDestroyQueue(0.5f);
    EXPECT_EQ(m_scene.GetGameObject(queuedId), m_objects.back());
    m_scene.FlushDestroyQueue(0.5f);

    EXPECT_EQ(m_scene.RemainingEntityCapacity(), available + 1);
    EXPECT_EQ(m_scene.GetGameObject(queuedId), nullptr);
    auto* replacement = m_scene.TryCreateGameObject("AfterQueuedDestroy");
    ASSERT_NE(replacement, nullptr);
    EXPECT_EQ(replacement->GetID().index, queuedId.index);
    EXPECT_EQ(replacement->GetID().generation, queuedId.generation + 2);
    EXPECT_EQ(m_scene.EntitySlotCount(), GROWTH_COUNT);
}

TEST_F(SceneCapacityTest, ClearAndMovePreserveDynamicStorageOwnership)
{
    ASSERT_TRUE(CreatePopulation());
    const auto removed = m_ids.back();
    ASSERT_TRUE(m_scene.DestroyGameObject(removed));
    const auto liveId = m_ids.front();
    auto* liveObject = m_objects.front();
    scene::Scene moved(std::move(m_scene));

    EXPECT_EQ(moved.EntitySlotCount(), GROWTH_COUNT);
    EXPECT_EQ(moved.GetGameObject(liveId), liveObject);
    EXPECT_EQ(liveObject->GetScene(), &moved);
    EXPECT_EQ(moved.GetComponentLifetime<scene::LightComponent>(liveId), m_lifetimes.front());
    auto* replacement = moved.TryCreateGameObject("MovedReplacement");
    ASSERT_NE(replacement, nullptr);
    EXPECT_EQ(replacement->GetID().index, removed.index);
    EXPECT_EQ(replacement->GetID().generation, removed.generation + 2);
    EXPECT_EQ(m_scene.EntitySlotCount(), 0u);
    EXPECT_EQ(m_scene.GameObjectCount(), 0u);
    ASSERT_NE(m_scene.TryCreateGameObject("ReuseMovedSource"), nullptr);

    const auto generation = moved.GetRenderSceneGeneration();
    moved.Clear();
    EXPECT_EQ(moved.EntitySlotCount(), 0u);
    EXPECT_NE(moved.GetRenderSceneGeneration(), generation);
    EXPECT_TRUE(moved.GetEntities<scene::LightComponent>().empty());
    EXPECT_EQ(moved.GetComponentLifetime<scene::LightComponent>(liveId), 0u);
    EXPECT_TRUE(moved.CanCreateGameObjects(100000));
    auto* afterClear = moved.TryCreateGameObject("AfterClear");
    ASSERT_NE(afterClear, nullptr);
    afterClear->AddComponent<scene::LightComponent>();
    EXPECT_NE(moved.GetComponentLifetime<scene::LightComponent>(afterClear->GetID()), m_lifetimes.front());
}

TEST_F(SceneCapacityTest, SparseComponentRowsGrowWithoutDensePopulationForEverySlot)
{
    for (uint32_t index = 0; index < GROWTH_COUNT; ++index)
        ASSERT_NE(m_scene.TryCreateGameObject("Sparse"), nullptr);
    const scene::EntityID lastId{GROWTH_COUNT - 1, 1};
    auto* high = m_scene.GetGameObject(lastId);
    ASSERT_NE(high, nullptr);
    high->AddComponent<scene::LightComponent>().intensity = 7.0f;

    EXPECT_EQ(m_scene.GetEntities<scene::LightComponent>().size(), 1u);
    EXPECT_EQ(m_scene.GetComponent<scene::LightComponent>({0, 1}), nullptr);
    EXPECT_FLOAT_EQ(high->GetComponent<scene::LightComponent>()->intensity, 7.0f);
    EXPECT_NE(m_scene.GetComponentLifetime<scene::LightComponent>(lastId), 0u);
}

} /// @note namespace fbzz::tests
