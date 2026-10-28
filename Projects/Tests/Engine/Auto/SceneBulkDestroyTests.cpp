/// @file    SceneBulkDestroyTests.cpp
/// @brief   大量破棄の順序、callback 内の生存一覧と再入・世代再利用を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Scene/Managed/ManagedObjectLifecycle.hpp>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

class DestroyObserver final : public scene::Script {
public:
    explicit DestroyObserver(std::function<void()> callback) : m_callback(std::move(callback)) {}
    void OnDestroy() override { m_callback(); }
private:
    std::function<void()> m_callback;
};

} /// @note namespace

class SceneBulkDestroyTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;

    void Observe(scene::GameObject& object, std::function<void()> callback)
    {
        auto* component = object.GetComponent<scene::ScriptComponent>();
        if (!component) component = &object.AddComponent<scene::ScriptComponent>();
        auto& entry = component->scripts.emplace_back();
        entry.script = std::make_unique<DestroyObserver>(std::move(callback));
        entry.script->SetContext(&m_scene, &object);
        entry.m_awoken = true;
        entry.m_started = true;
    }

    std::vector<scene::EntityID> LiveIds()
    {
        std::vector<scene::EntityID> ids;
        for (scene::GameObject& object : m_scene.GameObjects()) ids.push_back(object.GetID());
        return ids;
    }
};

TEST_F(SceneBulkDestroyTest, LargeQueuedBurstPreservesSurvivorOrderAndDelayedObjects)
{
    std::vector<scene::EntityID> survivors;
    for (uint32_t index = 0; index < 8193; ++index) {
        auto& object = m_scene.CreateGameObject("Population" + std::to_string(index));
        if (index % 3 == 0) {
            scene::GameObject::Destroy(object);
            scene::GameObject::Destroy(object);
        } else {
            survivors.push_back(object.GetID());
        }
    }
    auto& delayed = m_scene.CreateGameObject("Delayed");
    const auto delayedId = delayed.GetID();
    scene::GameObject::Destroy(delayed, 1.0f);
    survivors.push_back(delayedId);

    m_scene.FlushDestroyQueue(0.0f);
    EXPECT_EQ(LiveIds(), survivors);
    EXPECT_EQ(m_scene.GameObjectCount(), survivors.size());
    EXPECT_EQ(m_scene.Find("Population0"), nullptr);
    EXPECT_NE(m_scene.GetGameObject(delayedId), nullptr);

    m_scene.FlushDestroyQueue(1.0f);
    survivors.pop_back();
    EXPECT_EQ(LiveIds(), survivors);
    EXPECT_EQ(m_scene.GameObjectCount(), survivors.size());
}

TEST_F(SceneBulkDestroyTest, CallbackSeesLiveOrderAndCanCreateReorderAndDestroyReentrantly)
{
    auto& first = m_scene.CreateGameObject("First");
    auto& second = m_scene.CreateGameObject("Second");
    auto& third = m_scene.CreateGameObject("Third");
    auto& survivor = m_scene.CreateGameObject("Survivor");
    const auto firstId = first.GetID();
    const auto secondId = second.GetID();
    const auto thirdId = third.GetID();
    const auto survivorId = survivor.GetID();
    scene::EntityID bornId;
    int calls = 0;
    Observe(second, [&] {
        ++calls;
        EXPECT_EQ(m_scene.GameObjectCount(), 3u);
        EXPECT_EQ(LiveIds(), (std::vector<scene::EntityID>{secondId, thirdId, survivorId}));
        EXPECT_EQ(m_scene.Find("First"), nullptr);
        EXPECT_EQ(m_scene.GetGameObject(firstId), nullptr);
        auto& born = m_scene.CreateGameObject("Born");
        bornId = born.GetID();
        EXPECT_EQ(bornId.index, firstId.index);
        EXPECT_NE(bornId.generation, firstId.generation);
        ASSERT_TRUE(m_scene.MoveGameObjectToIndex(bornId, 0));
        ASSERT_TRUE(m_scene.DestroyGameObject(thirdId));
        ASSERT_TRUE(m_scene.DestroyGameObject(secondId));
        scene::GameObject::Destroy(born);
    });
    scene::GameObject::Destroy(first);
    scene::GameObject::Destroy(second);
    m_scene.FlushDestroyQueue(0.0f);

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(LiveIds(), (std::vector<scene::EntityID>{bornId, survivorId}));
    EXPECT_EQ(m_scene.GameObjectCount(), 2u);
    EXPECT_EQ(m_scene.GetGameObject(secondId), nullptr);
    EXPECT_EQ(m_scene.GetGameObject(thirdId), nullptr);
    m_scene.FlushDestroyQueue(0.0f);
    EXPECT_EQ(LiveIds(), (std::vector<scene::EntityID>{survivorId}));
}

TEST_F(SceneBulkDestroyTest, DescendantsDestroyBeforeParentAndDuplicateRequestsDoNotRepeatCallbacks)
{
    auto& survivor = m_scene.CreateGameObject("Survivor");
    auto& root = m_scene.CreateGameObject("Root");
    auto& first = m_scene.CreateGameObject("FirstChild");
    auto& second = m_scene.CreateGameObject("SecondChild");
    auto& grandchild = m_scene.CreateGameObject("Grandchild");
    ASSERT_TRUE(first.SetParent(&root));
    ASSERT_TRUE(second.SetParent(&root));
    ASSERT_TRUE(grandchild.SetParent(&first));
    const auto survivorId = survivor.GetID();
    std::vector<std::string> observed;
    Observe(grandchild, [&] { observed.push_back("Grandchild"); EXPECT_EQ(first.GetChildCount(), 0); });
    Observe(first, [&] { observed.push_back("FirstChild"); EXPECT_EQ(root.GetChildCount(), 1); });
    Observe(second, [&] { observed.push_back("SecondChild"); EXPECT_EQ(root.GetChildCount(), 0); });
    Observe(root, [&] { observed.push_back("Root"); EXPECT_EQ(m_scene.GameObjectCount(), 2u); });
    scene::GameObject::Destroy(root);
    scene::GameObject::Destroy(first);
    scene::GameObject::Destroy(root);
    m_scene.FlushDestroyQueue(0.0f);

    EXPECT_EQ(observed, (std::vector<std::string>{"Grandchild", "FirstChild", "SecondChild", "Root"}));
    EXPECT_EQ(LiveIds(), (std::vector<scene::EntityID>{survivorId}));
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
}

TEST_F(SceneBulkDestroyTest, SpawnDuringChildDestroyKeepsTheNewRootOutsideTheCapturedSubtree)
{
    auto& root = m_scene.CreateGameObject("Root");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(child.SetParent(&root));
    std::vector<scene::GameObject*> created;
    scene::EntityID createdId;
    Observe(child, [&] {
        EXPECT_EQ(root.GetChildCount(), 0);
        const scene::managed_detail::ManagedObjectLifecycle::SpawnPose pose{math::Vector3::ZERO, math::Quaternion::Identity()};
        ASSERT_EQ(scene::managed_detail::ManagedObjectLifecycle::SpawnMany(m_scene, "Born", false,
            std::span<const scene::managed_detail::ManagedObjectLifecycle::SpawnPose>{&pose, 1}, created), scene::ManagedBridgeStatus::OK);
        ASSERT_EQ(created.size(), 1u);
        createdId = created.front()->GetID();
    });
    scene::GameObject::Destroy(root);
    m_scene.FlushDestroyQueue(0.0f);
    EXPECT_EQ(LiveIds(), (std::vector<scene::EntityID>{createdId}));
    EXPECT_EQ(m_scene.GameObjectCount(), 1u);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_EQ(m_scene.Find("Born"), created.front());
}

TEST_F(SceneBulkDestroyTest, OwnerLocationsRemainCorrectAfterMovesAndSceneMove)
{
    std::vector<scene::EntityID> ids;
    for (int index = 0; index < 130; ++index)
        ids.push_back(m_scene.CreateGameObject("Object" + std::to_string(index)).GetID());
    ASSERT_TRUE(m_scene.MoveGameObjectToIndex(ids.back(), 0));
    ASSERT_TRUE(m_scene.SetRootSiblingIndex(ids[0], 129));
    scene::Scene moved(std::move(m_scene));
    EXPECT_EQ(m_scene.GameObjectCount(), 0u);
    EXPECT_EQ(moved.GameObjectCount(), ids.size());
    ASSERT_TRUE(moved.DestroyGameObject(ids.back()));
    ASSERT_TRUE(moved.DestroyGameObject(ids[0]));
    ASSERT_TRUE(moved.DestroyGameObject(ids[64]));
    EXPECT_EQ(moved.GameObjectCount(), ids.size() - 3);
    EXPECT_EQ(moved.Find("Object129"), nullptr);
    EXPECT_EQ(moved.Find("Object0"), nullptr);
    EXPECT_EQ(moved.Find("Object64"), nullptr);
    EXPECT_NE(moved.GetGameObject(ids[63]), nullptr);
    EXPECT_NE(moved.GetGameObject(ids[65]), nullptr);
}

} /// @note namespace fbzz::tests
