/// @file    ManagedObjectControlsTests.cpp
/// @brief   Component 制御と深い階層・Prefab 部位の全件検索を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Systems/LifetimeSystem.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Physics/World.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedObjectControls.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedObjectRegistry.hpp"
#include <string>
#include <vector>
#include <limits>
#include <functional>

namespace fbzz::tests {
using Controls = scene::managed_detail::ManagedObjectControls;
using Status = scene::ManagedBridgeStatus;
class ManagedObjectControlsTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
};
namespace {
} /// @note namespace

TEST_F(ManagedObjectControlsTest, WritesActiveLayerNameTagAndRejectsInvalidValues)
{
    auto& object = m_scene.CreateGameObject("Original");
    EXPECT_EQ(Controls::Write(object, 0, 0, ""), Status::OK);
    EXPECT_FALSE(object.activeSelf());
    EXPECT_EQ(Controls::Write(object, 2, 31, ""), Status::OK);
    EXPECT_EQ(Controls::Write(object, 2, 32, ""), Status::INVALID_ARGUMENT);
    EXPECT_EQ(object.layer, 31);
    EXPECT_EQ(Controls::Write(object, 3, 0, "Renamed"), Status::OK);
    EXPECT_EQ(Controls::Write(object, 3, 0, ""), Status::INVALID_ARGUMENT);
    EXPECT_EQ(object.name, "Renamed");
    EXPECT_EQ(Controls::Write(object, 4, 0, "Enemy"), Status::OK);
    EXPECT_EQ(object.tag, "Enemy");
    scene::ManagedBridgeReply reply;
    std::string text;
    EXPECT_EQ(Controls::Read(object, 4, reply, text), Status::OK);
    EXPECT_EQ(text, "Enemy");
}

TEST_F(ManagedObjectControlsTest, NativeComponentAddIsIdempotentAndPermanentComponentsCannotBeRemoved)
{
    auto& object = m_scene.CreateGameObject();
    bool exists = false;
    EXPECT_EQ(Controls::Component(object, 1, scene::ManagedBridgeOp::COMPONENT_READ, exists), Status::OK);
    EXPECT_FALSE(exists);
    ASSERT_EQ(Controls::Component(object, 1, scene::ManagedBridgeOp::COMPONENT_ADD, exists), Status::OK);
    auto* original = object.GetComponent<scene::RigidBodyComponent>();
    ASSERT_NE(original, nullptr);
    ASSERT_NE(original->rigidBody, nullptr);
    physics::RigidBody* originalBody = original->rigidBody.get();
    EXPECT_VEC3_NEAR(originalBody->GetPosition(), object.transform.worldPosition, testkit::kTolerance);
    EXPECT_EQ(Controls::Component(object, 1, scene::ManagedBridgeOp::COMPONENT_ADD, exists), Status::OK);
    EXPECT_EQ(object.GetComponent<scene::RigidBodyComponent>(), original);
    EXPECT_EQ(original->rigidBody.get(), originalBody);
    EXPECT_EQ(Controls::Component(object, 1, scene::ManagedBridgeOp::COMPONENT_REMOVE, exists), Status::OK);
    EXPECT_TRUE(exists);
    EXPECT_NE(object.GetComponent<scene::RigidBodyComponent>(), nullptr);
    Controls::FlushScriptRemovals(m_scene);
    EXPECT_EQ(object.GetComponent<scene::RigidBodyComponent>(), nullptr);
    EXPECT_EQ(Controls::Component(object, 0, scene::ManagedBridgeOp::COMPONENT_REMOVE, exists), Status::UNSUPPORTED);
    EXPECT_EQ(Controls::Component(object, 7, scene::ManagedBridgeOp::COMPONENT_ADD, exists), Status::UNSUPPORTED);
    object.transform.worldPosition.x = std::numeric_limits<float>::infinity();
    EXPECT_EQ(Controls::Component(object, 1, scene::ManagedBridgeOp::COMPONENT_ADD, exists), Status::INVALID_ARGUMENT);
    EXPECT_EQ(object.GetComponent<scene::RigidBodyComponent>(), nullptr);
}

TEST_F(ManagedObjectControlsTest, PendingRemovalHasNoLifetimeAndCannotBeCancelledByAdd)
{
    auto& object = m_scene.CreateGameObject();
    bool exists = false;
    uint64_t first = 0;
    ASSERT_EQ(Controls::Component(object, 1, scene::ManagedBridgeOp::COMPONENT_ADD, exists, &first), Status::OK);
    ASSERT_NE(first, 0U);
    ASSERT_EQ(Controls::Component(object, 1, scene::ManagedBridgeOp::COMPONENT_REMOVE, exists), Status::OK);
    uint64_t pending = 99;
    ASSERT_EQ(Controls::Component(object, 1, scene::ManagedBridgeOp::COMPONENT_READ, exists, &pending), Status::OK);
    EXPECT_FALSE(exists); EXPECT_EQ(pending, 0U);
    EXPECT_EQ(Controls::Component(object, 1, scene::ManagedBridgeOp::COMPONENT_ADD, exists), Status::INVALID_ARGUMENT);
    ASSERT_NE(object.GetComponent<scene::RigidBodyComponent>(), nullptr);
    Controls::FlushScriptRemovals(m_scene);
    uint64_t replacement = 0;
    ASSERT_EQ(Controls::Component(object, 1, scene::ManagedBridgeOp::COMPONENT_ADD, exists, &replacement), Status::OK);
    EXPECT_NE(replacement, first); EXPECT_NE(replacement, 0U);
}

TEST_F(ManagedObjectControlsTest, AuthoredRigidBodyRowKeepsItsBirthAcrossPhysicsInitialization)
{
    auto& object = m_scene.CreateGameObject();
    auto& body = object.AddComponent<scene::RigidBodyComponent>();
    ASSERT_EQ(body.rigidBody, nullptr);
    const auto birth = m_scene.GetComponentLifetime<scene::RigidBodyComponent>(object.GetID());
    bool exists = false; uint64_t observed = 0;
    ASSERT_EQ(Controls::Component(object, 1, scene::ManagedBridgeOp::COMPONENT_READ, exists, &observed), Status::OK);
    EXPECT_TRUE(exists); EXPECT_EQ(observed, birth); EXPECT_NE(observed, 0U);
    ASSERT_EQ(Controls::Component(object, 1, scene::ManagedBridgeOp::COMPONENT_ADD, exists, &observed), Status::OK);
    ASSERT_NE(object.GetComponent<scene::RigidBodyComponent>()->rigidBody, nullptr);
    EXPECT_EQ(observed, birth);
}

TEST_F(ManagedObjectControlsTest, ObjectRegistryKeepsStableTokensAndRetiresChangedIdentity)
{
    scene::managed_detail::ManagedObjectRegistry registry;
    std::vector<uint64_t> tokens;
    std::vector<scene::GameObject*> objects;
    for (int index = 0; index < 128; ++index) {
        auto& object = m_scene.CreateGameObject();
        objects.push_back(&object); tokens.push_back(registry.Register(m_scene, object));
    }
    for (size_t index = 0; index < objects.size(); ++index) {
        EXPECT_EQ(registry.Register(m_scene, *objects[index]), tokens[index]);
        EXPECT_EQ(registry.Resolve(m_scene, tokens[index]), objects[index]);
    }
    EXPECT_EQ(registry.Count(), 128U);
    objects[0]->instanceId += "-changed";
    EXPECT_NE(registry.Register(m_scene, *objects[0]), tokens[0]);
    EXPECT_EQ(registry.Find(tokens[0]), nullptr);
    EXPECT_EQ(registry.Count(), 128U);
    registry.RetireScene(m_scene);
    EXPECT_EQ(registry.Count(), 0U);
}

TEST_F(ManagedObjectControlsTest, ObjectRegistryNeverReusesClearedOrDestroyedTokens)
{
    scene::managed_detail::ManagedObjectRegistry registry;
    auto& first = m_scene.CreateGameObject();
    const auto entity = first.GetID();
    const auto guid = first.instanceId;
    const uint64_t token = registry.Register(m_scene, first);
    registry.RetireObject(m_scene, entity); ASSERT_TRUE(m_scene.DestroyGameObject(entity)); m_scene.FlushDestroyQueue(0.0f);
    auto& second = m_scene.CreateGameObject(); second.instanceId = guid;
    EXPECT_NE(second.GetID(), entity);
    EXPECT_EQ(registry.Resolve(m_scene, token), nullptr);
    const uint64_t replacement = registry.Register(m_scene, second);
    EXPECT_NE(replacement, token); EXPECT_EQ(registry.Count(), 1U);
    registry.Clear();
    EXPECT_NE(registry.Register(m_scene, second), replacement);
    m_scene.Clear();
    auto& third = m_scene.CreateGameObject(); third.instanceId = guid;
    EXPECT_EQ(registry.Resolve(m_scene, replacement), nullptr);
    EXPECT_NE(registry.Register(m_scene, third), replacement);
    EXPECT_EQ(registry.Count(), 1U);
}

TEST_F(ManagedObjectControlsTest, DeepTraversalExcludesRootAndKeepsPrefabInstancesSeparate)
{
    auto& root = m_scene.CreateGameObject("Root");
    scene::GameObject* parent = &root;
    for (int index = 0; index < 1024; ++index) {
        auto& child = m_scene.CreateGameObject("Part");
        child.SetParent(*parent); parent = &child;
        child.tag = "Enemy"; child.prefabSourceId = "source-child";
    }
    auto& other = m_scene.CreateGameObject("OtherRoot");
    auto& otherPart = m_scene.CreateGameObject("Part");
    otherPart.prefabSourceId = "source-child"; otherPart.SetParent(other);
    std::vector<scene::GameObject*> result;
    ASSERT_EQ(Controls::Query(m_scene, &root, 2, "", "", "", true, result), Status::OK);
    ASSERT_EQ(result.size(), 1024);
    EXPECT_EQ(result.back(), parent);
    ASSERT_EQ(Controls::Query(m_scene, &root, 1, "", "", "", true, result), Status::OK);
    EXPECT_EQ(result.size(), 1);
    ASSERT_EQ(Controls::Query(m_scene, &root, 3, "", "Enemy", "source-child", true, result), Status::OK);
    EXPECT_EQ(result.size(), 1024);
    root.SetActive(false);
    ASSERT_EQ(Controls::Query(m_scene, parent, 3, "", "", "", false, result), Status::OK);
    EXPECT_TRUE(result.empty());
    ASSERT_EQ(Controls::Query(m_scene, &root, 3, "", "", "", true, result), Status::OK);
    EXPECT_EQ(result.size(), 1025);
}

TEST_F(ManagedObjectControlsTest, SceneSearchReturnsAllBeyondEditorPageLimitInHierarchyOrder)
{
    auto& root = m_scene.CreateGameObject("Root");
    auto& first = m_scene.CreateGameObject("Enemy"); first.SetParent(root);
    auto& last = m_scene.CreateGameObject("Enemy"); last.SetParent(root);
    for (int index = 0; index < 1000; ++index) m_scene.CreateGameObject("Enemy");
    std::vector<scene::GameObject*> result;
    ASSERT_EQ(Controls::Query(m_scene, nullptr, 0, "Enemy", "", "", true, result), Status::OK);
    ASSERT_EQ(result.size(), 1002);
    EXPECT_EQ(result[0], &first); EXPECT_EQ(result[1], &last);
}

TEST_F(ManagedObjectControlsTest, ReparentRejectsCyclesAndOverflowBeforeChangingHierarchyOrTransforms)
{
    auto& root = m_scene.CreateGameObject("Root");
    auto& child = m_scene.CreateGameObject("Child"); child.SetParent(root);
    auto& target = m_scene.CreateGameObject("Target");
    target.transform.position = {2, 0, 0};
    root.transform.worldScale = {std::numeric_limits<float>::max(), 1, 1};
    const auto before = target.transform;
    EXPECT_FALSE(Controls::SetParent(target, &root));
    EXPECT_EQ(target.GetParent(), nullptr);
    EXPECT_VEC3_NEAR(target.transform.worldPosition, before.worldPosition, testkit::kTolerance);
    EXPECT_FALSE(Controls::SetParent(root, &child));
    EXPECT_EQ(child.GetParent(), &root);
    root.transform.worldScale = math::Vector3::ONE;
    EXPECT_TRUE(Controls::SetParent(target, &root));
    EXPECT_EQ(target.GetParent(), &root);
    EXPECT_VEC3_NEAR(target.transform.position, before.position, testkit::kTolerance);
}
} /// @note namespace fbzz::tests
