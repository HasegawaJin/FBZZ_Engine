/// @file    ManagedAiPoseTransferTests.cpp
/// @brief   Scene-based AI pose commit contracts, hierarchy staging and failure atomicity.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Physics/World.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedAiPoseTransfer.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <xmmintrin.h>

namespace fbzz::tests {
namespace {
template<typename T> void AppendPoseValue(std::string& bytes, T value)
{
    const size_t offset = bytes.size();
    bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
} /// @note namespace

class ManagedAiPoseTransferTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::managed_detail::ManagedAiPoseTransfer m_transfer;
    std::vector<scene::GameObject*> m_objects;
    std::string m_input, m_output;
    uint32_t m_count = 0;
    scene::ManagedBridgeRequest m_request;
    scene::ManagedBridgeReply m_reply;

    void Begin()
    {
        m_count = 0;
        m_objects.clear();
        m_input.assign(4, '\0');
        m_request = {};
        m_reply = {};
    }
    void Add(scene::GameObject* object, const math::Vector3& position = math::Vector3::ZERO,
        const math::Quaternion& rotation = math::Quaternion::Identity(), uint64_t token = 0, uint64_t lifetime = 0)
    {
        if (object) { m_objects.push_back(object); token = m_objects.size(); lifetime = m_scene.GetRenderSceneGeneration(); }
        AppendPoseValue(m_input, token); AppendPoseValue(m_input, lifetime);
        AppendPoseValue(m_input, position.x); AppendPoseValue(m_input, position.y); AppendPoseValue(m_input, position.z);
        AppendPoseValue(m_input, rotation.x); AppendPoseValue(m_input, rotation.y); AppendPoseValue(m_input, rotation.z); AppendPoseValue(m_input, rotation.w);
        ++m_count;
        std::memcpy(m_input.data(), &m_count, 4);
    }
    void Prepare()
    {
        m_request.op = scene::ManagedBridgeOp::AI_CROWD_STEP;
        m_request.index = 1;
        m_request.integer = m_count;
        m_request.input = m_input.data();
        m_request.inputBytes = static_cast<uint32_t>(m_input.size());
        m_output.assign(m_count * 4 + 4, '\x7f');
        m_request.output = m_output.data();
        m_request.outputCapacity = static_cast<uint32_t>(m_count * 4);
    }
    scene::ManagedBridgeStatus Run()
    {
        return m_transfer.Run(m_scene, m_request, m_reply, [](void* state, uint64_t token, scene::GameObject*& object) {
            auto& fixture = *static_cast<ManagedAiPoseTransferTest*>(state);
            object = token && token <= fixture.m_objects.size() ? fixture.m_objects[static_cast<size_t>(token - 1)] : nullptr;
            return scene::ManagedBridgeStatus::OK;
        }, this);
    }
    scene::ManagedObjectStatus Status(uint32_t index)
    {
        uint32_t value{};
        std::memcpy(&value, m_output.data() + index * 4, 4);
        return static_cast<scene::ManagedObjectStatus>(value);
    }
    void ExpectUnpublished() { EXPECT_TRUE(std::all_of(m_output.begin(), m_output.end(), [](char byte) { return byte == '\x7f'; })); }
};

TEST_F(ManagedAiPoseTransferTest, CommitsThousandIndependentPosesAndRetainsCallerTail)
{
    Begin();
    std::vector<scene::GameObject*> objects;
    for (uint32_t index = 0; index < 1000; ++index) {
        auto& object = m_scene.CreateGameObject("Fish");
        object.transform.scale = {1, 2, 3};
        objects.push_back(&object);
        Add(&object, {static_cast<float>(index), 2, 3}, {0, 0, 0, 2});
    }
    Prepare();
    ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_reply.index, 1000u); EXPECT_EQ(m_reply.flags, 0u); EXPECT_EQ(m_reply.outputBytes, 4000u);
    for (uint32_t index = 0; index < 1000; ++index) {
        EXPECT_VEC3_NEAR(objects[index]->transform.worldPosition, (math::Vector3{static_cast<float>(index), 2, 3}), testkit::kTolerance);
        EXPECT_QUAT_NEAR(objects[index]->transform.worldRotation, math::Quaternion::Identity(), testkit::kTolerance);
        EXPECT_VEC3_NEAR(objects[index]->transform.scale, (math::Vector3{1, 2, 3}), testkit::kTolerance);
        EXPECT_EQ(Status(index), scene::ManagedObjectStatus::VALID);
    }
    EXPECT_EQ(m_output.back(), '\x7f');
}

TEST_F(ManagedAiPoseTransferTest, PreservesOrderedDuplicatesAndSkipsUnsetAndExpiredSlots)
{
    auto& object = m_scene.CreateGameObject("Fish");
    Begin(); Add(&object, {1, 2, 3}); Add(nullptr, {std::numeric_limits<float>::quiet_NaN(), 0, 0}, {0, 0, 0, 0});
    Add(nullptr, {}, {}, 0, m_scene.GetRenderSceneGeneration());
    Add(nullptr, {}, {}, 999, m_scene.GetRenderSceneGeneration());
    Add(&object, {5, 6, 7}, math::Quaternion::FromEuler({0.2f, 0.3f, 0.4f}));
    Prepare();
    ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_reply.index, 2u); EXPECT_EQ(m_reply.flags, 3u);
    EXPECT_EQ(Status(0), scene::ManagedObjectStatus::VALID); EXPECT_EQ(Status(1), scene::ManagedObjectStatus::UNSET);
    EXPECT_EQ(Status(2), scene::ManagedObjectStatus::EXPIRED); EXPECT_EQ(Status(3), scene::ManagedObjectStatus::EXPIRED);
    EXPECT_VEC3_NEAR(object.transform.worldPosition, (math::Vector3{5, 6, 7}), testkit::kTolerance);
    EXPECT_QUAT_NEAR(object.transform.worldRotation, math::Quaternion::FromEuler({0.2f, 0.3f, 0.4f}), testkit::kTolerance);
}

TEST_F(ManagedAiPoseTransferTest, SynchronizesRotatedScaledParentAndInactiveDescendant)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& fish = m_scene.CreateGameObject("Fish");
    auto& child = m_scene.CreateGameObject("InactiveChild");
    ASSERT_TRUE(fish.SetParent(&parent)); ASSERT_TRUE(child.SetParent(&fish)); child.SetActive(false);
    parent.transform.position = {10, 20, 30}; parent.transform.scale = {-2, 3, 4};
    parent.transform.rotation = math::Quaternion::FromEuler({0.2f, -0.4f, 0.3f});
    fish.transform.scale = {1.5f, 2, 0.5f}; child.transform.position = {1, 0, 2};
    const math::Quaternion rotation = math::Quaternion::FromEuler({-0.3f, 0.5f, 0.1f});
    Begin(); Add(&fish, {17, 26, 35}, rotation); Prepare();
    ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    EXPECT_VEC3_NEAR(fish.transform.worldPosition, (math::Vector3{17, 26, 35}), testkit::kTolerance);
    EXPECT_QUAT_NEAR(fish.transform.worldRotation, rotation, testkit::kTolerance);
    EXPECT_VEC3_NEAR(fish.transform.scale, (math::Vector3{1.5f, 2, 0.5f}), testkit::kTolerance);
    scene::Transform expected = child.transform;
    scene::UpdateWorldTransform(expected, &fish.transform);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, expected.worldPosition, testkit::kTolerance);
}

TEST_F(ManagedAiPoseTransferTest, RejectsAncestorDescendantPairsInEitherOrderWithoutMutation)
{
    auto& parent = m_scene.CreateGameObject("Parent"); auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(child.SetParent(&parent));
    const scene::Transform beforeParent = parent.transform, beforeChild = child.transform;
    for (bool reverse : {false, true}) {
        Begin(); Add(reverse ? &child : &parent, {10, 20, 30}); Add(reverse ? &parent : &child, {40, 50, 60}); Prepare();
        EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
        EXPECT_VEC3_NEAR(parent.transform.position, beforeParent.position, testkit::kTolerance);
        EXPECT_VEC3_NEAR(child.transform.position, beforeChild.position, testkit::kTolerance);
    }
}

TEST_F(ManagedAiPoseTransferTest, RejectsLateInvalidQuaternionForeignSceneAndLivePhysicsBody)
{
    auto& first = m_scene.CreateGameObject("First"); auto& second = m_scene.CreateGameObject("Second");
    Begin(); Add(&first, {5, 0, 0}); Add(&second, {6, 0, 0}, {0, 0, 0, 0}); Prepare();
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
    EXPECT_VEC3_NEAR(first.transform.position, math::Vector3::ZERO, testkit::kTolerance);
    Begin(); Add(&first, {5, 0, 0}); Add(nullptr, {}, {}, 999, m_scene.GetRenderSceneGeneration() + 1); Prepare();
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::SCENE_MISMATCH); ExpectUnpublished();
    auto& body = second.AddComponent<scene::RigidBodyComponent>();
    /// @note An enabled native body is protected before any transform math; no physics world or simulation is required.
    body.rigidBody = std::make_unique<physics::RigidBody>();
    Begin(); Add(&first, {5, 0, 0}); Add(&second, {6, 0, 0}); Prepare();
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::PHYSICS_BODY); ExpectUnpublished();
    EXPECT_VEC3_NEAR(first.transform.position, math::Vector3::ZERO, testkit::kTolerance);
    body.enabled = false;
    ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    scene::Scene foreign; auto& other = foreign.CreateGameObject("Other");
    Begin(); Add(&first, {20, 0, 0}); Add(&other, {30, 0, 0}); Prepare();
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::SCENE_MISMATCH); ExpectUnpublished();
    EXPECT_VEC3_NEAR(first.transform.position, (math::Vector3{5, 0, 0}), testkit::kTolerance);
}

TEST_F(ManagedAiPoseTransferTest, RejectsDescendantOverflowWithoutRepairingAncestorCaches)
{
    auto& ancestor = m_scene.CreateGameObject("Ancestor"); auto& target = m_scene.CreateGameObject("Target");
    auto& child = m_scene.CreateGameObject("Child"); ASSERT_TRUE(target.SetParent(&ancestor)); ASSERT_TRUE(child.SetParent(&target));
    ancestor.transform.position = {10, 0, 0}; ancestor.transform.scale = {2, 1, 1};
    child.transform.position = {std::numeric_limits<float>::max(), 0, 0}; target.transform.worldPosition = {99, 0, 0};
    const scene::Transform beforeAncestor = ancestor.transform, beforeTarget = target.transform, beforeChild = child.transform;
    Begin(); Add(&target, {15, 0, 0}); Prepare();
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
    EXPECT_VEC3_NEAR(ancestor.transform.worldPosition, beforeAncestor.worldPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(target.transform.worldPosition, beforeTarget.worldPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, beforeChild.worldPosition, testkit::kTolerance);
}

TEST_F(ManagedAiPoseTransferTest, RejectsInsufficientCapacityBeforeComputationAndMalformedPackets)
{
    auto& object = m_scene.CreateGameObject("Fish");
    Begin(); Add(&object, {5, 6, 7}); Prepare(); m_request.outputCapacity = 3;
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::BUFFER_TOO_SMALL); EXPECT_EQ(m_reply.outputBytes, 4u); ExpectUnpublished();
    EXPECT_VEC3_NEAR(object.transform.position, math::Vector3::ZERO, testkit::kTolerance);
    m_request.outputCapacity = 4; m_request.integer = 2;
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
    m_request.integer = 1; m_request.flags = 1;
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
    m_request.flags = 0; m_request.index = 0;
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
    m_request.index = 1; m_request.integer = 4097;
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
}

TEST_F(ManagedAiPoseTransferTest, RejectsUnusedRequestFieldsAndOverlappingPackets)
{
    auto& object = m_scene.CreateGameObject("Fish");
    Begin(); Add(&object, {5, 6, 7}); Prepare();
    m_request.reserved0 = 1;
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
    m_request.reserved0 = 0; m_request.reserved1 = 1;
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
    m_request.reserved1 = 0; m_request.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
    m_request.x = 0; m_request.y = 1;
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
    m_request.y = 0; m_request.z = 1;
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
    m_request.z = 0; m_request.w = 1;
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
    m_request.w = 0;
    const std::string inputSnapshot = m_input;
    m_request.output = m_input.data() + 4;
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(m_input, inputSnapshot); ExpectUnpublished();
    EXPECT_VEC3_NEAR(object.transform.position, math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(ManagedAiPoseTransferTest, StaticPoseDrivenBodyKeepsAiAuthorityAndConfirmedColliderPose)
{
    auto& object = m_scene.CreateGameObject("PoseDrivenFish");
    auto& body = object.AddComponent<scene::RigidBodyComponent>();
    body.rigidBody = std::make_unique<physics::RigidBody>();
    body.rigidBody->m_isStatic = true;
    body.poseDriven = true;
    object.AddComponent<scene::SphereColliderComponent>().radius = 0.5f;
    physics::World world;
    scene::PhysicsSystem system;
    SystemContext context{m_scene, world, nullptr, nullptr, 1.0f / 60, 1.0f / 60, true, true};
    Begin(); Add(&object, {3, 4, 5}); Prepare();
    ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    EXPECT_VEC3_NEAR(object.transform.worldPosition, (math::Vector3{3, 4, 5}), testkit::kTolerance);
    system.Update(context);
    EXPECT_VEC3_NEAR(body.rigidBody->GetPosition(), (math::Vector3{3, 4, 5}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(object.transform.worldPosition, (math::Vector3{3, 4, 5}), testkit::kTolerance);
    physics::World::RaycastHit hit;
    ASSERT_TRUE(world.Raycast({3, 4, 8}, {0, 0, -1}, 10, hit));
    EXPECT_NEAR(hit.distance, 2.5f, 1.0e-4f);
    Begin(); Add(&object, {6, 7, 8}); Prepare();
    ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    /// @note Late AI publication does not mutate the already confirmed Physics query state.
    ASSERT_TRUE(world.Raycast({3, 4, 8}, {0, 0, -1}, 10, hit));
    system.Update(context);
    EXPECT_FALSE(world.Raycast({3, 4, 8}, {0, 0, -1}, 10, hit));
    ASSERT_TRUE(world.Raycast({6, 7, 11}, {0, 0, -1}, 10, hit));
    EXPECT_NEAR(hit.distance, 2.5f, 1.0e-4f);
    EXPECT_VEC3_NEAR(object.transform.worldPosition, (math::Vector3{6, 7, 8}), testkit::kTolerance);
}

TEST_F(ManagedAiPoseTransferTest, PoseDrivenRequiresStaticOptInAndRetainsFailureAtomicity)
{
    auto& first = m_scene.CreateGameObject("First");
    auto& second = m_scene.CreateGameObject("Second");
    auto& body = second.AddComponent<scene::RigidBodyComponent>();
    body.rigidBody = std::make_unique<physics::RigidBody>();
    body.rigidBody->m_isStatic = true;
    Begin(); Add(&first, {1, 2, 3}); Add(&second, {4, 5, 6}); Prepare();
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::PHYSICS_BODY); ExpectUnpublished();
    body.poseDriven = true;
    body.rigidBody->m_isStatic = false;
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::PHYSICS_BODY); ExpectUnpublished();
    EXPECT_VEC3_NEAR(first.transform.position, math::Vector3::ZERO, testkit::kTolerance);
    body.rigidBody->m_isStatic = true;
    ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    EXPECT_VEC3_NEAR(first.transform.worldPosition, (math::Vector3{1, 2, 3}), testkit::kTolerance);
    scene::RigidBodyComponent copy(body);
    EXPECT_TRUE(copy.poseDriven);
    scene::RigidBodyComponent assigned;
    assigned = body;
    EXPECT_TRUE(assigned.poseDriven);
}

TEST_F(ManagedAiPoseTransferTest, PoseDrivenSurvivesSceneTextAndDefaultsFalseForLegacyScene)
{
    auto& object = m_scene.CreateGameObject("PoseDriverRoundTrip");
    auto& body = object.AddComponent<scene::RigidBodyComponent>();
    body.rigidBody = std::make_unique<physics::RigidBody>();
    body.rigidBody->m_isStatic = true;
    body.poseDriven = true;
    const std::string text = scene::SceneSerializer::SaveToText(m_scene);
    ASSERT_FALSE(text.empty());
    ASSERT_NE(text.find("poseDriven = true"), std::string::npos);
    auto loaded = scene::SceneSerializer::LoadDataFromText(text, "PoseDriverRoundTrip.scene");
    ASSERT_NE(loaded, nullptr);
    auto* restored = loaded->FindByGuid(object.instanceId);
    ASSERT_NE(restored, nullptr);
    auto* restoredBody = restored->GetComponent<scene::RigidBodyComponent>();
    ASSERT_NE(restoredBody, nullptr);
    EXPECT_TRUE(restoredBody->poseDriven);
    EXPECT_TRUE(restoredBody->rigidBody->IsStatic());
    std::string legacy = text;
    const size_t at = legacy.find("poseDriven = true");
    legacy.erase(at, std::string("poseDriven = true").size());
    auto old = scene::SceneSerializer::LoadDataFromText(legacy, "LegacyBody.scene");
    ASSERT_NE(old, nullptr);
    auto* oldBody = old->FindByGuid(object.instanceId)->GetComponent<scene::RigidBodyComponent>();
    ASSERT_NE(oldBody, nullptr);
    EXPECT_FALSE(oldBody->poseDriven);
}

TEST_F(ManagedAiPoseTransferTest, PoseDrivenSurvivesAppendObjectsTextPath)
{
    auto& object = m_scene.CreateGameObject("AppendedPoseDriver");
    auto& body = object.AddComponent<scene::RigidBodyComponent>();
    body.rigidBody = std::make_unique<physics::RigidBody>();
    body.rigidBody->m_isStatic = true;
    body.poseDriven = true;
    const std::string text = scene::SceneSerializer::SaveToText(m_scene);
    ASSERT_FALSE(text.empty());
    scene::Scene target;
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(scene::SceneSerializer::AppendObjects(target, text, nullptr, roots));
    ASSERT_EQ(roots.size(), 1u);
    auto* appended = target.GetGameObject(roots[0]);
    ASSERT_NE(appended, nullptr);
    auto* appendedBody = appended->GetComponent<scene::RigidBodyComponent>();
    ASSERT_NE(appendedBody, nullptr);
    EXPECT_TRUE(appendedBody->poseDriven);
    EXPECT_TRUE(appendedBody->rigidBody->IsStatic());
}

TEST_F(ManagedAiPoseTransferTest, SharedAncestorsPreserveInterleavedDuplicatesAndInactiveDescendants)
{
    auto& root = m_scene.CreateGameObject("Root");
    auto& middle = m_scene.CreateGameObject("Middle");
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& first = m_scene.CreateGameObject("First");
    auto& second = m_scene.CreateGameObject("Second");
    auto& child = m_scene.CreateGameObject("InactiveChild");
    ASSERT_TRUE(middle.SetParent(&root)); ASSERT_TRUE(parent.SetParent(&middle));
    ASSERT_TRUE(first.SetParent(&parent)); ASSERT_TRUE(second.SetParent(&parent)); ASSERT_TRUE(child.SetParent(&first));
    child.SetActive(false);
    root.transform.position = {10, 3, -2}; root.transform.rotation = {0, 0, 0, 2};
    middle.transform.position = {1, 2, 3}; middle.transform.scale = {-2, 3, 4};
    middle.transform.rotation = math::Quaternion::FromEuler({0.2f, -0.4f, 0.3f});
    parent.transform.position = {2, 1, -1}; parent.transform.rotation = math::Quaternion::FromEuler({-0.1f, 0.3f, 0.2f});
    first.transform.scale = {1.5f, 2, 0.5f}; child.transform.position = {1, 0, 2};
    const math::Quaternion firstRotation = math::Quaternion::FromEuler({0.3f, -0.2f, 0.1f});
    const math::Quaternion secondRotation = math::Quaternion::FromEuler({-0.2f, 0.5f, 0.4f});
    Begin(); Add(&first, {12, 15, 7}); Add(&second, {-4, 6, 2}, secondRotation); Add(&first, {3, 5, 11}, firstRotation); Prepare();
    ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_reply.index, 3u); EXPECT_EQ(m_reply.flags, 0u);
    EXPECT_VEC3_NEAR(first.transform.worldPosition, (math::Vector3{3, 5, 11}), testkit::kTolerance);
    EXPECT_QUAT_NEAR(first.transform.worldRotation, firstRotation, testkit::kTolerance);
    EXPECT_VEC3_NEAR(second.transform.worldPosition, (math::Vector3{-4, 6, 2}), testkit::kTolerance);
    EXPECT_QUAT_NEAR(second.transform.worldRotation, secondRotation, testkit::kTolerance);
    EXPECT_VEC3_NEAR(first.transform.scale, (math::Vector3{1.5f, 2, 0.5f}), testkit::kTolerance);
    scene::Transform expected = child.transform;
    scene::UpdateWorldTransform(expected, &first.transform);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, expected.worldPosition, testkit::kTolerance);
    EXPECT_QUAT_NEAR(child.transform.worldRotation, expected.worldRotation, testkit::kTolerance);
    for (uint32_t index = 0; index < 3; ++index) EXPECT_EQ(Status(index), scene::ManagedObjectStatus::VALID);
    EXPECT_EQ(m_output.back(), '\x7f');
}

TEST_F(ManagedAiPoseTransferTest, SharedAncestorArithmeticExpiresAfterFailedPacketAndParentEdit)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& first = m_scene.CreateGameObject("First");
    auto& second = m_scene.CreateGameObject("Second");
    auto& child = m_scene.CreateGameObject("OverflowChild");
    ASSERT_TRUE(first.SetParent(&parent)); ASSERT_TRUE(second.SetParent(&parent)); ASSERT_TRUE(child.SetParent(&second));
    parent.transform.position = {10, 0, 0}; parent.transform.scale = {2, 1, 1};
    child.transform.position = {std::numeric_limits<float>::max(), 0, 0};
    parent.transform.worldPosition = {99, 0, 0}; first.transform.worldPosition = {98, 0, 0}; second.transform.worldPosition = {97, 0, 0};
    const scene::Transform beforeParent = parent.transform, beforeFirst = first.transform, beforeSecond = second.transform, beforeChild = child.transform;
    Begin(); Add(&first, {15, 0, 0}); Add(&second, {20, 0, 0}); Prepare();
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
    EXPECT_VEC3_NEAR(parent.transform.worldPosition, beforeParent.worldPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(first.transform.worldPosition, beforeFirst.worldPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(second.transform.worldPosition, beforeSecond.worldPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, beforeChild.worldPosition, testkit::kTolerance);
    parent.transform.position = {30, 4, 5}; parent.transform.scale = {2, 3, 4}; child.transform.position = {1, 0, 0};
    Begin(); Add(&first, {50, 10, 17}); Add(&second, {60, 10, 17}); Prepare();
    ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    EXPECT_VEC3_NEAR(parent.transform.worldPosition, (math::Vector3{30, 4, 5}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(parent.transform.worldScale, (math::Vector3{2, 3, 4}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(first.transform.position, (math::Vector3{10, 2, 3}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(second.transform.position, (math::Vector3{15, 2, 3}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{62, 10, 17}), testkit::kTolerance);
    EXPECT_EQ(Status(0), scene::ManagedObjectStatus::VALID); EXPECT_EQ(Status(1), scene::ManagedObjectStatus::VALID);
}

TEST_F(ManagedAiPoseTransferTest, SharedAncestorArithmeticDoesNotCrossScenesWithMatchingEntitySlots)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& first = m_scene.CreateGameObject("First");
    auto& second = m_scene.CreateGameObject("Second");
    ASSERT_TRUE(first.SetParent(&parent)); ASSERT_TRUE(second.SetParent(&parent));
    parent.transform.position = {100, 0, 0}; parent.transform.scale = {2, 3, 4};
    Begin(); Add(&first, {110, 6, 12}); Add(&second, {120, 9, 16}); Prepare();
    ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    scene::Scene other;
    auto& otherParent = other.CreateGameObject("OtherParent");
    auto& otherFirst = other.CreateGameObject("OtherFirst");
    auto& otherSecond = other.CreateGameObject("OtherSecond");
    ASSERT_TRUE(otherFirst.SetParent(&otherParent)); ASSERT_TRUE(otherSecond.SetParent(&otherParent));
    ASSERT_EQ(otherParent.GetID().index, parent.GetID().index);
    ASSERT_EQ(otherFirst.GetID().index, first.GetID().index);
    ASSERT_EQ(otherSecond.GetID().index, second.GetID().index);
    otherParent.transform.position = {-20, 1, 5}; otherParent.transform.scale = {-2, 4, 0};
    otherFirst.transform.scale = {1.5f, 2, 0.5f};
    std::vector<scene::GameObject*> objects{&otherFirst, &otherSecond};
    Begin(); Add(nullptr, {-10, 9, 5}, math::Quaternion::Identity(), 1, other.GetRenderSceneGeneration());
    Add(nullptr, {-4, 13, 5}, math::Quaternion::Identity(), 2, other.GetRenderSceneGeneration()); Prepare();
    ASSERT_EQ(m_transfer.Run(other, m_request, m_reply, [](void* state, uint64_t token, scene::GameObject*& object) {
        const auto& targets = *static_cast<const std::vector<scene::GameObject*>*>(state);
        object = token && token <= targets.size() ? targets[static_cast<size_t>(token - 1)] : nullptr;
        return scene::ManagedBridgeStatus::OK;
    }, &objects), scene::ManagedBridgeStatus::OK);
    EXPECT_VEC3_NEAR(otherParent.transform.worldPosition, (math::Vector3{-20, 1, 5}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(otherParent.transform.worldScale, (math::Vector3{-2, 4, 0}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(otherFirst.transform.worldPosition, (math::Vector3{-10, 9, 5}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(otherSecond.transform.worldPosition, (math::Vector3{-4, 13, 5}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(otherFirst.transform.scale, (math::Vector3{1.5f, 2, 0.5f}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(parent.transform.worldPosition, (math::Vector3{100, 0, 0}), testkit::kTolerance);
    EXPECT_EQ(Status(0), scene::ManagedObjectStatus::VALID); EXPECT_EQ(Status(1), scene::ManagedObjectStatus::VALID);
    EXPECT_EQ(m_output.back(), '\x7f');
}
TEST_F(ManagedAiPoseTransferTest, WarmWorldReuseRepairsWorldCachesAndObservesDirectLocalAndParentEdits)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& target = m_scene.CreateGameObject("Fish");
    auto& child = m_scene.CreateGameObject("Bone");
    ASSERT_TRUE(target.SetParent(&parent)); ASSERT_TRUE(child.SetParent(&target));
    parent.transform.scale = {-2, 3, 4}; child.transform.position = {1, 2, 3};
    Begin(); Add(&target, {4, 5, 6}); Prepare(); ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    const scene::Transform expected = child.transform;
    parent.transform.worldPosition.x = std::numeric_limits<float>::quiet_NaN();
    target.transform.worldScale.x = std::numeric_limits<float>::quiet_NaN();
    child.transform.worldRotation.w = std::numeric_limits<float>::quiet_NaN();
    Begin(); Add(&target, {4, 5, 6}); Prepare(); ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, expected.worldPosition, testkit::kTolerance);
    EXPECT_QUAT_NEAR(child.transform.worldRotation, expected.worldRotation, testkit::kTolerance);
    parent.transform.position = {20, 10, 5}; parent.transform.scale = {0, -2, 3};
    child.transform.position = {-4, 2, 1}; child.transform.rotation = math::Quaternion::FromEuler({0.2f, 0.3f, 0.1f});
    Begin(); Add(&target, {4, 5, 6}); Prepare(); ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    scene::Transform next = child.transform; scene::UpdateWorldTransform(next, &target.transform);
    EXPECT_VEC3_NEAR(parent.transform.worldPosition, parent.transform.position, testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, next.worldPosition, testkit::kTolerance);
    EXPECT_QUAT_NEAR(child.transform.worldRotation, next.worldRotation, testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldScale, next.worldScale, testkit::kTolerance);
}

TEST_F(ManagedAiPoseTransferTest, WarmWorldReuseDoesNotPublishAfterInvalidDescendantOrOverflow)
{
    auto& target = m_scene.CreateGameObject("Fish");
    auto& child = m_scene.CreateGameObject("Bone"); ASSERT_TRUE(child.SetParent(&target));
    target.transform.scale = {2, 1, 1}; child.transform.position = {1, 2, 3};
    Begin(); Add(&target, {4, 5, 6}); Prepare(); ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    const scene::Transform before = target.transform;
    child.transform.rotation = {0, 0, 0, 0};
    Begin(); Add(&target, {10, 20, 30}); Prepare(); EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
    EXPECT_VEC3_NEAR(target.transform.position, before.position, testkit::kTolerance);
    child.transform.rotation = math::Quaternion::Identity(); child.transform.position.x = std::numeric_limits<float>::max();
    Begin(); Add(&target, {10, 20, 30}); Prepare(); EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
    EXPECT_VEC3_NEAR(target.transform.worldPosition, before.worldPosition, testkit::kTolerance);
    child.transform.position = {1, 2, 3};
    Begin(); Add(&target, {10, 20, 30}); Prepare(); ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{12, 22, 33}), testkit::kTolerance);
}

TEST_F(ManagedAiPoseTransferTest, WarmWorldReuseDistinguishesRootAndIdentityParentForNonunitRotation)
{
    auto& ancestor = m_scene.CreateGameObject("Ancestor");
    auto& target = m_scene.CreateGameObject("Fish"); ASSERT_TRUE(target.SetParent(&ancestor));
    ancestor.transform.rotation = {0, 0, 0, 2};
    Begin(); Add(&target, {4, 5, 6}); Prepare(); ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    EXPECT_FLOAT_EQ(ancestor.transform.worldRotation.w, 2.0f);
    auto& outer = m_scene.CreateGameObject("IdentityParent"); ASSERT_TRUE(ancestor.SetParent(&outer));
    ancestor.transform.rotation = {0, 0, 0, 2};
    Begin(); Add(&target, {4, 5, 6}); Prepare(); ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    EXPECT_FLOAT_EQ(ancestor.transform.worldRotation.w, 1.0f);
    EXPECT_VEC3_NEAR(target.transform.worldPosition, (math::Vector3{4, 5, 6}), testkit::kTolerance);
}

TEST_F(ManagedAiPoseTransferTest, StagedDescendantOverflowAfterMovementRemainsAtomicWithWarmTraversal)
{
    auto& target = m_scene.CreateGameObject("Fish");
    auto& child = m_scene.CreateGameObject("Bone"); ASSERT_TRUE(child.SetParent(&target));
    child.transform.position = {1, 2, 3};
    Begin(); Add(&target, {4, 5, 6}); Prepare(); ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    const scene::Transform before = target.transform;
    const float large = std::numeric_limits<float>::max() * 0.75f;
    child.transform.position = {large, 0, 0};
    Begin(); Add(&target, {large, 0, 0}); Prepare();
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
    EXPECT_VEC3_NEAR(target.transform.worldPosition, before.worldPosition, testkit::kTolerance);
    child.transform.position = {1, 2, 3}; child.SetActive(false);
    Begin(); Add(&target, {10, 20, 30}); Prepare(); ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{11, 22, 33}), testkit::kTolerance);
}

TEST_F(ManagedAiPoseTransferTest, SceneFlushUsesAiWorldMathWithoutIgnoringEditedInputsOrFailedPoses)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& target = m_scene.CreateGameObject("Fish");
    auto& child = m_scene.CreateGameObject("Bone");
    ASSERT_TRUE(target.SetParent(&parent)); ASSERT_TRUE(child.SetParent(&target));
    child.transform.position = {1, 2, 3};
    Begin(); Add(&target, {4, 5, 6}); Prepare(); ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    child.transform.worldPosition = {99, 98, 97};
    scene::FlushWorldTransforms(m_scene);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{5, 7, 9}), testkit::kTolerance);
    child.transform.position = {2, 3, 4}; parent.transform.position = {10, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{16, 8, 10}), testkit::kTolerance);
    Begin(); Add(&target, {30, 40, 50}); Add(&parent, {1, 2, 3}); Prepare();
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT); ExpectUnpublished();
    scene::FlushWorldTransforms(m_scene);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{16, 8, 10}), testkit::kTolerance);
}

TEST_F(ManagedAiPoseTransferTest, SceneFlushDoesNotReuseWorldMathAcrossChangedRoundingControl)
{
    const uint32_t saved = _mm_getcsr();
    struct RestoreControl { uint32_t value; ~RestoreControl() { _mm_setcsr(value); } } restore{saved};
    _mm_setcsr((saved & ~_MM_ROUND_MASK) | _MM_ROUND_NEAREST);
    auto& target = m_scene.CreateGameObject("Fish");
    auto& child = m_scene.CreateGameObject("Bone"); ASSERT_TRUE(child.SetParent(&target));
    child.transform.position = {0.00000001f, 0, 0};
    Begin(); Add(&target, {1, 0, 0}); Prepare(); ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    const float oldX = child.transform.worldPosition.x;
    _mm_setcsr((saved & ~_MM_ROUND_MASK) | _MM_ROUND_UP);
    scene::Transform expected = child.transform;
    scene::UpdateWorldTransform(expected, &target.transform);
    ASSERT_NE(expected.worldPosition.x, oldX);
    scene::FlushWorldTransforms(m_scene);
    EXPECT_EQ(child.transform.worldPosition.x, expected.worldPosition.x);
}

TEST_F(ManagedAiPoseTransferTest, ValidatedArithmeticReuseRepairsWorldWritesAndObservesLocalAndParentEdits)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& child = m_scene.CreateGameObject("Child");
    child.SetParent(parent);
    parent.transform.position = {3, 4, 5};
    parent.transform.scale = {-2, 0, 3};
    child.transform.position = {1, 2, 3};
    scene::FlushWorldTransforms(m_scene);
    Begin(); Add(&parent, parent.transform.position, parent.transform.rotation); Prepare();
    ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    const auto expected = child.transform;
    child.transform.worldPosition = {-99, 99, 99};
    child.transform.worldRotation = math::Quaternion::Identity();
    child.transform.worldScale = math::Vector3::ONE;
    scene::FlushWorldTransforms(m_scene);
    EXPECT_EQ(std::memcmp(&expected, &child.transform, sizeof(expected)), 0);
    child.transform.position.x += 0.0000005f;
    parent.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, 0.31f);
    auto changed = child.transform;
    scene::UpdateWorldTransform(changed, &parent.transform);
    scene::FlushWorldTransforms(child);
    changed = child.transform;
    scene::UpdateWorldTransform(changed, &parent.transform);
    EXPECT_EQ(std::memcmp(&changed, &child.transform, sizeof(changed)), 0);
    child.ClearParent();
    scene::FlushWorldTransforms(m_scene);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, child.transform.position, 0.0f);
}

TEST_F(ManagedAiPoseTransferTest, ArithmeticReuseInvalidatesForFloatingPointControlChanges)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& child = m_scene.CreateGameObject("Child");
    child.SetParent(parent);
    parent.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, 0.31f);
    child.transform.position = {1.1f, 2.2f, 3.3f};
    scene::FlushWorldTransforms(m_scene);
    Begin(); Add(&parent, parent.transform.position, parent.transform.rotation); Prepare();
    ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    const unsigned original = _mm_getcsr();
    _mm_setcsr((original & ~0x6000u) | 0x4000u);
    auto expected = child.transform;
    scene::UpdateWorldTransform(expected, &parent.transform);
    scene::FlushWorldTransforms(m_scene);
    const auto actual = child.transform;
    _mm_setcsr(original);

    EXPECT_EQ(std::memcmp(&expected, &actual, sizeof(expected)), 0);
}


TEST_F(ManagedAiPoseTransferTest, ValidatedQuaternionProductsMatchOrdinaryHierarchyArithmeticBitwise)
{
    Begin();
    std::vector<scene::GameObject*> children;
    for (int index = 0; index < 96; ++index) {
        auto& parent = m_scene.CreateGameObject("Parent");
        auto& child = m_scene.CreateGameObject("Child");
        child.SetParent(parent);
        child.SetActive(index % 2 == 0);
        const float value = static_cast<float>(index + 1) * 0.03125f;
        parent.transform.scale = {index % 3 == 0 ? -2.0f : 0.0f, 1.25f, 3.5f};
        child.transform.position = {value, -value * 0.3f, value * 0.7f};
        child.transform.rotation = {value * 0.13f, -value * 0.27f, value * 0.39f, 1.75f};
        child.transform.scale = {0.75f, -1.25f, index % 2 == 0 ? -0.0f : 2.0f};
        Add(&parent, {value * 3.0f, 2.0f - value, value * 0.37f},
            {value * 0.17f, value * 0.31f, -value * 0.11f, 2.5f});
        children.push_back(&child);
    }
    Prepare();
    ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    for (const auto* child : children) {
        auto expected = child->transform;
        scene::UpdateWorldTransform(expected, &child->GetParent()->transform);
        EXPECT_EQ(std::memcmp(&expected, &child->transform, sizeof(expected)), 0);
    }
}


TEST_F(ManagedAiPoseTransferTest, SharedTopologyMatchesSequentialSetWorldPoseBitwise)
{
    std::vector<scene::GameObject*> nodes;
    auto& ancestor = m_scene.CreateGameObject("Ancestor");
    nodes.push_back(&ancestor);
    ancestor.transform.position = {4, -3, 2};
    ancestor.transform.rotation = {0.1f, -0.2f, 0.05f, 1.5f};
    scene::GameObject* parent = &ancestor;
    for (int depth = 0; depth < 32; ++depth) {
        auto& next = m_scene.CreateGameObject("SharedParent");
        ASSERT_TRUE(next.SetParent(parent));
        next.transform.position = {0.03f, -0.01f, 0.02f};
        next.transform.rotation = {0.001f, -0.003f, 0.002f, 1.01f};
        next.transform.scale = {1.002f, 0.999f, 1.001f};
        parent = &next;
        nodes.push_back(parent);
    }
    struct PoseRow {
        scene::GameObject* object;
        math::Vector3 position;
        math::Quaternion rotation;
    };
    std::vector<PoseRow> rows;
    for (int index = 0; index < 64; ++index) {
        auto& target = m_scene.CreateGameObject("Target");
        ASSERT_TRUE(target.SetParent(parent));
        target.transform.position = {0.25f, -0.5f, 0.75f};
        target.transform.rotation = {0.1f, 0.2f, -0.3f, 1.2f};
        target.transform.scale = {1.5f, -0.0f, 0.75f};
        nodes.push_back(&target);
        auto& child = m_scene.CreateGameObject("InactiveChild");
        ASSERT_TRUE(child.SetParent(&target));
        child.SetActive(false);
        child.transform.position = {0.2f, -0.3f, 0.4f};
        child.transform.rotation = {0.03f, 0.04f, -0.02f, 1.1f};
        nodes.push_back(&child);
        auto& tip = m_scene.CreateGameObject("Tip");
        ASSERT_TRUE(tip.SetParent(&child));
        tip.transform.position = {0.5f, 0.25f, -0.125f};
        nodes.push_back(&tip);
        const float value = static_cast<float>(index + 1) * 0.0625f;
        rows.push_back({&target, {value, -value * 0.3f, value * 0.7f},
            {value * 0.13f, -value * 0.07f, value * 0.19f, 1.75f}});
    }
    /// @note Interleaved duplicates retain their intermediate arithmetic and final-row authority.
    rows.push_back({rows[0].object, {7, 8, 9}, {0.2f, -0.3f, 0.1f, 2.1f}});
    rows.push_back({rows[31].object, {-7, 3, 4}, {-0.1f, 0.2f, 0.3f, 1.9f}});
    ancestor.transform.worldPosition.x = std::numeric_limits<float>::quiet_NaN();
    std::vector<scene::Transform> expected(m_scene.EntitySlotCount());
    for (auto* node : nodes) {
        auto& transform = expected[node->GetID().index];
        transform = node->transform;
        const auto* nodeParent = node->GetParent();
        scene::UpdateWorldTransform(transform, nodeParent ? &expected[nodeParent->GetID().index] : nullptr);
    }
    Begin();
    for (const auto& row : rows) {
        Add(row.object, row.position, row.rotation);
        auto& transform = expected[row.object->GetID().index];
        const auto& parentTransform = expected[row.object->GetParent()->GetID().index];
        const auto localScale = transform.scale;
        scene::SetWorldPose(transform, &parentTransform, row.position, row.rotation.Normalized(), transform.worldScale);
        transform.scale = localScale;
        for (auto* node : nodes) {
            if (node != row.object && !node->IsDescendantOf(*row.object)) continue;
            const auto* nodeParent = node->GetParent();
            scene::UpdateWorldTransform(expected[node->GetID().index],
                nodeParent ? &expected[nodeParent->GetID().index] : nullptr);
        }
    }
    Prepare();
    ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    for (const auto* node : nodes)
        EXPECT_EQ(std::memcmp(&expected[node->GetID().index], &node->transform, sizeof(scene::Transform)), 0);
    EXPECT_EQ(m_reply.index, static_cast<uint32_t>(rows.size()));
}

TEST_F(ManagedAiPoseTransferTest, RepeatedTargetStillRejectsIntermediateDescendantOverflow)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& target = m_scene.CreateGameObject("Target");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(target.SetParent(&parent));
    ASSERT_TRUE(child.SetParent(&target));
    const float large = std::numeric_limits<float>::max() * 0.75f;
    child.transform.position = {large, 0, 0};
    const scene::Transform beforeParent = parent.transform;
    const scene::Transform beforeTarget = target.transform, beforeChild = child.transform;
    Begin();
    Add(&target, {large, 0, 0});
    Add(&target, {0, 0, 0});
    Prepare();
    EXPECT_EQ(Run(), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    ExpectUnpublished();
    EXPECT_EQ(std::memcmp(&beforeParent, &parent.transform, sizeof(scene::Transform)), 0);
    EXPECT_EQ(std::memcmp(&beforeTarget, &target.transform, sizeof(scene::Transform)), 0);
    EXPECT_EQ(std::memcmp(&beforeChild, &child.transform, sizeof(scene::Transform)), 0);
    Begin(); Add(&target, {0, 0, 0}); Prepare();
    ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    EXPECT_FLOAT_EQ(child.transform.worldPosition.x, large);
}

TEST_F(ManagedAiPoseTransferTest, TopologyBuildObservesLateResolverHierarchyAndRawTransformEdits)
{
    auto& oldParent = m_scene.CreateGameObject("OldParent");
    auto& newParent = m_scene.CreateGameObject("NewParent");
    auto& first = m_scene.CreateGameObject("First");
    auto& second = m_scene.CreateGameObject("Second");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(first.SetParent(&oldParent));
    ASSERT_TRUE(second.SetParent(&newParent));
    ASSERT_TRUE(child.SetParent(&first));
    oldParent.transform.position = {10, 0, 0};
    const auto oldParentBefore = oldParent.transform;
    child.transform.position = {1, 2, 3};
    Begin(); Add(&first, {20, 30, 40}); Add(&second, {50, 60, 70}); Prepare();
    struct ResolutionState {
        scene::GameObject* first;
        scene::GameObject* second;
        scene::GameObject* parent;
        scene::GameObject* child;
        bool changed = false;
    } state{&first, &second, &newParent, &child};
    ASSERT_EQ(m_transfer.Run(m_scene, m_request, m_reply,
        [](void* raw, uint64_t token, scene::GameObject*& object) {
            auto& state = *static_cast<ResolutionState*>(raw);
            if (token == 2) {
                state.changed = state.first->SetParent(state.parent);
                state.parent->transform.position = {-3, 4, 5};
                state.parent->transform.scale = {-2, 3, 4};
                state.parent->transform.rotation = math::Quaternion::FromEuler({0.1f, -0.2f, 0.3f});
                state.child->transform.position = {2, 3, 4};
            }
            object = token == 1 ? state.first : token == 2 ? state.second : nullptr;
            return scene::ManagedBridgeStatus::OK;
        }, &state), scene::ManagedBridgeStatus::OK);
    ASSERT_TRUE(state.changed);
    EXPECT_EQ(std::memcmp(&oldParentBefore, &oldParent.transform, sizeof(scene::Transform)), 0);
    EXPECT_VEC3_NEAR(first.transform.worldPosition, (math::Vector3{20, 30, 40}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(second.transform.worldPosition, (math::Vector3{50, 60, 70}), testkit::kTolerance);
    auto expected = child.transform;
    scene::UpdateWorldTransform(expected, &first.transform);
    EXPECT_EQ(std::memcmp(&expected, &child.transform, sizeof(scene::Transform)), 0);
}

TEST_F(ManagedAiPoseTransferTest, ValidatedPoseFactorsMatchSetWorldPoseBitwise)
{
    const float edge = 1e-6f;
    const float scales[] = {0.0f, -0.0f, edge, -edge,
        std::nextafter(edge, std::numeric_limits<float>::infinity()),
        std::nextafter(-edge, -std::numeric_limits<float>::infinity()), 1e-7f, -2.0f, 3.0f};
    std::vector<scene::GameObject*> targets;
    std::vector<scene::Transform> expected;
    Begin();
    for (int index = 0; index < 108; ++index) {
        const float value = static_cast<float>(index + 1) * 0.015625f;
        scene::GameObject* parent = nullptr;
        scene::Transform parentExpected;
        if (index % 3 != 0) {
            parent = &m_scene.CreateGameObject("NonunitParent");
            parent->transform.position = {value * 0.5f, -value, value * 0.3f};
            parent->transform.rotation = {0.07f, -0.11f, 0.13f, 1.7f};
            parent->transform.scale = {scales[index % 9], scales[(index + 3) % 9], scales[(index + 5) % 9]};
            parentExpected = parent->transform;
            scene::UpdateWorldTransform(parentExpected, nullptr);
        }
        auto& target = m_scene.CreateGameObject("Target");
        if (parent) ASSERT_TRUE(target.SetParent(parent));
        target.transform.position = {value * 0.1f, -value * 0.3f, value * 0.7f};
        target.transform.rotation = {value * 0.13f, -value * 0.27f, value * 0.39f, 1.75f};
        target.transform.scale = {1.25f, -0.0f, 0.75f};
        const math::Vector3 position{value * 3.1f, -value * 0.37f, value * 1.13f};
        const math::Quaternion rotation{value * 0.23f, value * 0.41f, -value * 0.19f, 2.3f};
        auto transform = target.transform;
        scene::UpdateWorldTransform(transform, parent ? &parentExpected : nullptr);
        const auto localScale = transform.scale;
        scene::SetWorldPose(transform, parent ? &parentExpected : nullptr,
            position, rotation.Normalized(), transform.worldScale);
        transform.scale = localScale;
        scene::UpdateWorldTransform(transform, parent ? &parentExpected : nullptr);
        targets.push_back(&target);
        expected.push_back(transform);
        Add(&target, position, rotation);
    }
    Prepare();
    ASSERT_EQ(Run(), scene::ManagedBridgeStatus::OK);
    for (size_t index = 0; index < targets.size(); ++index)
        EXPECT_EQ(std::memcmp(&expected[index], &targets[index]->transform, sizeof(scene::Transform)), 0);
}

TEST_F(ManagedAiPoseTransferTest, QuaternionFactorsDoNotCrossResolverFloatingPointControlChanges)
{
    const uint32_t saved = _mm_getcsr();
    struct RestoreControl { uint32_t value; ~RestoreControl() { _mm_setcsr(value); } } restore{saved};
    _mm_setcsr((saved & ~_MM_ROUND_MASK) | _MM_ROUND_NEAREST);
    auto& first = m_scene.CreateGameObject("First");
    auto& second = m_scene.CreateGameObject("Second");
    const math::Quaternion firstInput{0.13f, -0.27f, 0.39f, 1.75f};
    const math::Quaternion secondInput{-0.17f, 0.31f, 0.11f, 2.3f};
    const auto firstParsed = firstInput.Normalized();
    const auto firstBefore = first.transform, secondBefore = second.transform;
    Begin(); Add(&first, {1.1f, 2.2f, 3.3f}, firstInput); Add(&second, {4.4f, 5.5f, 6.6f}, secondInput); Prepare();
    struct ResolutionState { scene::GameObject* first; scene::GameObject* second; uint32_t control; } state{&first, &second, saved};
    ASSERT_EQ(m_transfer.Run(m_scene, m_request, m_reply,
        [](void* raw, uint64_t token, scene::GameObject*& object) {
            const auto& state = *static_cast<const ResolutionState*>(raw);
            if (token == 2) _mm_setcsr((state.control & ~_MM_ROUND_MASK) | _MM_ROUND_UP);
            object = token == 1 ? state.first : token == 2 ? state.second : nullptr;
            return scene::ManagedBridgeStatus::OK;
        }, &state), scene::ManagedBridgeStatus::OK);
    auto firstExpected = firstBefore, secondExpected = secondBefore;
    scene::SetWorldPose(firstExpected, nullptr, {1.1f, 2.2f, 3.3f}, firstParsed, firstExpected.worldScale);
    scene::UpdateWorldTransform(firstExpected, nullptr);
    scene::SetWorldPose(secondExpected, nullptr, {4.4f, 5.5f, 6.6f}, secondInput.Normalized(), secondExpected.worldScale);
    scene::UpdateWorldTransform(secondExpected, nullptr);
    EXPECT_EQ(std::memcmp(&firstExpected, &first.transform, sizeof(scene::Transform)), 0);
    EXPECT_EQ(std::memcmp(&secondExpected, &second.transform, sizeof(scene::Transform)), 0);
}

} /// @note namespace fbzz::tests
