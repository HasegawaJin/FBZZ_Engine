/// @file    ManagedAiPoseTransferTests.cpp
/// @brief   Scene-based AI pose commit contracts, hierarchy staging and failure atomicity.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedAiPoseTransfer.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

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

} /// @note namespace fbzz::tests
