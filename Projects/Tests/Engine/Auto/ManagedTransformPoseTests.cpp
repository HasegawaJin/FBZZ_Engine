/// @file    ManagedTransformPoseTests.cpp
/// @brief   C# 回転・拡縮の実接続と子孫同期・失敗時の原子性を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <Engine/Scene/ScriptComponent.hpp>
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Math/MathUtils.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedTransformBatch.hpp"
#include <limits>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace fbzz::tests {

class ManagedTransformPoseKernelTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::managed_detail::ManagedTransformBatch m_kernel;

    scene::ManagedBridgeStatus Run(scene::GameObject& target, scene::ManagedBridgeOp operation,
        const math::Vector3& value, float w = 0)
    {
        scene::ManagedBridgeRequest request;
        request.op = operation;
        request.x = value.x;
        request.y = value.y;
        request.z = value.z;
        request.w = w;
        scene::ManagedBridgeReply reply;
        return m_kernel.RunPose(target, request, reply);
    }
};

TEST_F(ManagedTransformPoseKernelTest, SynchronizesRotatedScaledHierarchyAndInactiveDescendants)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& child = m_scene.CreateGameObject("Child");
    auto& grandchild = m_scene.CreateGameObject("Grandchild");
    ASSERT_TRUE(child.SetParent(&parent));
    ASSERT_TRUE(grandchild.SetParent(&child));
    parent.transform.position = {10, 20, 30};
    child.transform.position = {1, 2, 3};
    grandchild.transform.position = {2, 0, 0};
    grandchild.SetActive(false);
    const math::Quaternion rotation = math::Quaternion::FromEuler({0.2f, -0.7f, 0.3f});
    ASSERT_EQ(Run(parent, scene::ManagedBridgeOp::TRANSFORM_WRITE_LOCAL_ROTATION,
        {rotation.x, rotation.y, rotation.z}, rotation.w), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(Run(parent, scene::ManagedBridgeOp::TRANSFORM_WRITE_LOCAL_SCALE, {-2, 3, 4}), scene::ManagedBridgeStatus::OK);
    scene::Transform expectedChild = child.transform;
    scene::UpdateWorldTransform(expectedChild, &parent.transform);
    scene::Transform expectedGrandchild = grandchild.transform;
    scene::UpdateWorldTransform(expectedGrandchild, &expectedChild);
    EXPECT_QUAT_NEAR(child.transform.worldRotation, rotation.Normalized(), testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, expectedChild.worldPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(grandchild.transform.worldPosition, expectedGrandchild.worldPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(grandchild.transform.worldScale, (math::Vector3{-2, 3, 4}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.position, (math::Vector3{1, 2, 3}), testkit::kTolerance);
}

TEST_F(ManagedTransformPoseKernelTest, RejectsDescendantOverflowWithoutRepairingAnyLiveCache)
{
    auto& ancestor = m_scene.CreateGameObject("Ancestor");
    auto& target = m_scene.CreateGameObject("Target");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(target.SetParent(&ancestor));
    ASSERT_TRUE(child.SetParent(&target));
    ancestor.transform.position = {10, 0, 0};
    child.transform.position = {std::numeric_limits<float>::max(), 0, 0};
    target.transform.worldPosition = {99, 0, 0};
    const scene::Transform originalAncestor = ancestor.transform;
    const scene::Transform originalTarget = target.transform;
    const scene::Transform originalChild = child.transform;
    EXPECT_EQ(Run(target, scene::ManagedBridgeOp::TRANSFORM_WRITE_LOCAL_SCALE, {2, 1, 1}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_VEC3_NEAR(ancestor.transform.worldPosition, originalAncestor.worldPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(target.transform.worldPosition, originalTarget.worldPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(target.transform.scale, originalTarget.scale, testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, originalChild.worldPosition, testkit::kTolerance);
    EXPECT_EQ(Run(target, scene::ManagedBridgeOp::TRANSFORM_WRITE_LOCAL_ROTATION, math::Vector3::ZERO, 0), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_QUAT_NEAR(target.transform.rotation, originalTarget.rotation, testkit::kTolerance);
}

TEST_F(ManagedTransformPoseKernelTest, ReadsWorldCacheAndRejectsMalformedSelectorsWithoutMutation)
{
    auto& object = m_scene.CreateGameObject("Target");
    object.transform.rotation = math::Quaternion::FromEuler({0.3f, 0.4f, 0.5f});
    object.transform.worldRotation = math::Quaternion::FromEuler({-0.7f, 0.1f, 0.2f});
    const math::Quaternion cached = object.transform.worldRotation;
    scene::ManagedBridgeRequest request;
    scene::ManagedBridgeReply reply;
    request.op = scene::ManagedBridgeOp::TRANSFORM_READ_ROTATION;
    request.index = 1;
    ASSERT_EQ(m_kernel.RunPose(object, request, reply), scene::ManagedBridgeStatus::OK);
    EXPECT_QUAT_NEAR((math::Quaternion{reply.x, reply.y, reply.z, reply.w}), cached, testkit::kTolerance);
    request.op = scene::ManagedBridgeOp::TRANSFORM_READ_DIRECTION;
    request.index = 0;
    ASSERT_EQ(m_kernel.RunPose(object, request, reply), scene::ManagedBridgeStatus::OK);
    EXPECT_VEC3_NEAR((math::Vector3{reply.x, reply.y, reply.z}), cached * math::Vector3::FORWARD, testkit::kTolerance);
    request.index = 3;
    EXPECT_EQ(m_kernel.RunPose(object, request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    request.index = 0;
    request.flags = 1;
    EXPECT_EQ(m_kernel.RunPose(object, request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    request.flags = 0;
    request.inputBytes = 1;
    EXPECT_EQ(m_kernel.RunPose(object, request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::TRANSFORM_ROTATE, math::Vector3::ZERO, 45), scene::ManagedBridgeStatus::OK);
    EXPECT_QUAT_NEAR(object.transform.worldRotation, cached, testkit::kTolerance);
}

TEST_F(ManagedTransformPoseKernelTest, RotateLookAtMatchExistingMathAndGuardLivePhysicsBody)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(child.SetParent(&parent));
    parent.transform.rotation = math::Quaternion::FromEuler({0.2f, -0.3f, 0.4f});
    child.transform.rotation = math::Quaternion::FromEuler({-0.4f, 0.1f, 0.2f});
    child.transform.position = {2, 1, 3};
    scene::FlushWorldTransforms(m_scene);
    const math::Vector3 axis{1, 2, 3};
    const math::Quaternion expectedRotation = (math::Quaternion::FromAxisAngle(axis.Normalized(), -75 * math::DEG2RAD)
        * child.transform.rotation).Normalized();
    ASSERT_EQ(Run(child, scene::ManagedBridgeOp::TRANSFORM_ROTATE, axis, -75), scene::ManagedBridgeStatus::OK);
    EXPECT_QUAT_NEAR(child.transform.rotation, expectedRotation, testkit::kTolerance);
    scene::Transform expected = child.transform;
    const math::Vector3 target{7, 8, -2};
    expected.LookAt(target);
    ASSERT_EQ(Run(child, scene::ManagedBridgeOp::TRANSFORM_LOOK_AT, target), scene::ManagedBridgeStatus::OK);
    EXPECT_QUAT_NEAR(child.transform.rotation, expected.rotation, testkit::kTolerance);
    const math::Quaternion before = child.transform.rotation;
    ASSERT_EQ(Run(child, scene::ManagedBridgeOp::TRANSFORM_ROTATE, math::Vector3::ZERO, 45), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(Run(child, scene::ManagedBridgeOp::TRANSFORM_LOOK_AT, child.transform.worldPosition), scene::ManagedBridgeStatus::OK);
    EXPECT_QUAT_NEAR(child.transform.rotation, before, testkit::kTolerance);
    auto& body = child.AddComponent<scene::RigidBodyComponent>();
    body.rigidBody = std::make_unique<physics::RigidBody>();
    EXPECT_EQ(Run(child, scene::ManagedBridgeOp::TRANSFORM_WRITE_LOCAL_SCALE, {2, 2, 2}), scene::ManagedBridgeStatus::PHYSICS_BODY);
    EXPECT_QUAT_NEAR(child.transform.rotation, before, testkit::kTolerance);
    body.enabled = false;
    EXPECT_EQ(Run(child, scene::ManagedBridgeOp::TRANSFORM_WRITE_LOCAL_SCALE, {2, 2, 2}), scene::ManagedBridgeStatus::OK);
}

class ManagedTransformPoseTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::ManagedScriptAdapter* m_adapter = nullptr;
    scene::GameObject* m_owner = nullptr;

    void SetUp() override
    {
        EngineFixture::SetUp();
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
        m_owner = &m_scene.CreateGameObject("PoseOwner");
        auto& entry = m_owner->AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.TransformPoseComponent", entry.savedScriptId);
        ASSERT_NE(entry.script, nullptr);
        entry.script->SetContext(&m_scene, m_owner);
        m_adapter = runtime.FindAdapter(entry.script.get());
        ASSERT_NE(m_adapter, nullptr);
        ASSERT_TRUE(m_adapter->RefreshOwnerBinding());
    }

    void TearDown() override
    {
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        m_adapter = nullptr;
        m_owner = nullptr;
        runtime.Unload(&m_scene);
        m_scene.Clear();
        EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic();
        EngineFixture::TearDown();
    }

    bool Configure(std::string_view json) { return m_adapter->ApplySettingsJson(json); }
    bool Tick() { return m_adapter->ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate"); }
    scene::ManagedBridgeReply Field(std::string_view name, std::string* bytes = nullptr)
    {
        scene::ManagedBridgeRequest request;
        request.op = scene::ManagedBridgeOp::READ_FIELD;
        request.generation = m_adapter->Generation();
        request.instanceId = m_adapter->InstanceId();
        request.ownerId = m_adapter->OwnerId();
        request.input = name.data();
        request.inputBytes = static_cast<uint32_t>(name.size());
        scene::ManagedBridgeReply reply;
        EXPECT_TRUE(scene::ManagedScriptRuntime::Instance().Dispatch(request, reply, bytes));
        return reply;
    }
    math::Vector3 Vector(std::string_view name)
    {
        const auto value = Field(name);
        return {value.x, value.y, value.z};
    }
};

TEST_F(ManagedTransformPoseTest, PureQuaternionMatchesNativeMathBeforeAnyCallback)
{
    auto& entry = m_owner->GetComponent<scene::ScriptComponent>()->scripts.emplace_back();
    entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.QuaternionMathComponent", entry.savedScriptId);
    ASSERT_NE(entry.script, nullptr);
    m_adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
    ASSERT_NE(m_adapter, nullptr);
    std::string xyzBytes;
    std::string wBytes;
    std::string rotatedBytes;
    std::string scalarBytes;
    EXPECT_EQ(Field("quaternionXYZ", &xyzBytes).valueKind, scene::ManagedValueKind::VECTOR3_ARRAY);
    EXPECT_EQ(Field("quaternionW", &wBytes).valueKind, scene::ManagedValueKind::FLOAT_ARRAY);
    EXPECT_EQ(Field("rotated", &rotatedBytes).valueKind, scene::ManagedValueKind::VECTOR3_ARRAY);
    EXPECT_EQ(Field("scalar", &scalarBytes).valueKind, scene::ManagedValueKind::FLOAT_ARRAY);
    ASSERT_EQ(xyzBytes.size(), 4u + 15u * 12u);
    ASSERT_EQ(wBytes.size(), 4u + 15u * 4u);
    ASSERT_EQ(rotatedBytes.size(), 4u + 2u * 12u);
    ASSERT_EQ(scalarBytes.size(), 4u + 3u * 4u);
    const auto readFloat = [](const std::string& bytes, size_t offset) {
        float value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };
    const math::Quaternion first = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::HALF_PI);
    const math::Quaternion second = math::Quaternion::FromEuler({0.2f, -0.4f, 0.7f});
    const math::Quaternion nonunit{1, 2, 3, 4};
    const math::Quaternion negativeFirst{-first.x, -first.y, -first.z, -first.w};
    const math::Quaternion expected[] = {
        math::Quaternion::Identity(), first, second, first * second,
        nonunit.Normalized(), nonunit.Inverse(), nonunit.Conjugate(),
        math::Quaternion::LookRotation({1, 2, 3}),
        math::Quaternion::LookRotation(math::Vector3::UP, math::Vector3::UP),
        math::Quaternion::LookRotation(math::Vector3::RIGHT, math::Vector3::RIGHT),
        math::Quaternion::Lerp(first, second, 0.3f), math::Quaternion::Slerp(first, second, 0.3f),
        math::Quaternion::Lerp(first, second, 1.5f), math::Quaternion::Slerp(first, second, -0.5f),
        math::Quaternion::Slerp(first, negativeFirst, 0.4f)
    };
    for (size_t i = 0; i < 15; ++i) {
        SCOPED_TRACE(i);
        const math::Quaternion value{readFloat(xyzBytes, 4 + i * 12), readFloat(xyzBytes, 8 + i * 12),
            readFloat(xyzBytes, 12 + i * 12), readFloat(wBytes, 4 + i * 4)};
        EXPECT_QUAT_NEAR(value, expected[i], testkit::kTolerance);
    }
    const math::Vector3 rotated[] = {first * math::Vector3::FORWARD, second * math::Vector3{2, -3, 4}};
    for (size_t i = 0; i < 2; ++i)
        EXPECT_VEC3_NEAR((math::Vector3{readFloat(rotatedBytes, 4 + i * 12), readFloat(rotatedBytes, 8 + i * 12),
            readFloat(rotatedBytes, 12 + i * 12)}), rotated[i], testkit::kTolerance);
    EXPECT_NEAR(readFloat(scalarBytes, 4), math::Quaternion::Dot(first, second), testkit::kTolerance);
    EXPECT_NEAR(readFloat(scalarBytes, 8), nonunit.Length(), testkit::kTolerance);
    EXPECT_NEAR(readFloat(scalarBytes, 12), 30, testkit::kTolerance);
}

TEST_F(ManagedTransformPoseTest, ReadsLocalAndInterpolatedWorldRotationThroughCoreClr)
{
    const math::Quaternion local = math::Quaternion::FromEuler({0.4f, -0.7f, 0.2f});
    const math::Quaternion world = math::Quaternion::FromEuler({-0.2f, 0.3f, 0.6f});
    m_owner->transform.rotation = local;
    m_owner->transform.worldRotation = world;
    m_owner->transform.scale = {-2, 3, 4};
    ASSERT_TRUE(Tick());
    EXPECT_VEC3_NEAR(Vector("localRotationXYZ"), (math::Vector3{local.x, local.y, local.z}), testkit::kTolerance);
    EXPECT_NEAR(Field("localRotationW").x, local.w, testkit::kTolerance);
    EXPECT_VEC3_NEAR(Vector("worldRotationXYZ"), (math::Vector3{world.x, world.y, world.z}), testkit::kTolerance);
    EXPECT_NEAR(Field("worldRotationW").x, world.w, testkit::kTolerance);
    EXPECT_VEC3_NEAR(Vector("localScale"), m_owner->transform.scale, testkit::kTolerance);
    EXPECT_VEC3_NEAR(Vector("forward"), world * math::Vector3::FORWARD, testkit::kTolerance);
    EXPECT_VEC3_NEAR(Vector("right"), world * math::Vector3::RIGHT, testkit::kTolerance);
    EXPECT_VEC3_NEAR(Vector("up"), world * math::Vector3::UP, testkit::kTolerance);
    EXPECT_QUAT_NEAR(m_owner->transform.worldRotation, world, testkit::kTolerance);
}

TEST_F(ManagedTransformPoseTest, WritesTargetScaleAndSynchronizesChildrenThroughCoreClr)
{
    auto& target = m_scene.CreateGameObject("Target");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(child.SetParent(&target));
    child.transform.position = {2, 3, 4};
    child.SetActive(false);
    ASSERT_TRUE(Configure("{\"target\":\"" + target.instanceId + "\",\"operation\":2,\"scale\":[-2,3,4]}"));
    ASSERT_TRUE(Tick());
    EXPECT_VEC3_NEAR(target.transform.scale, (math::Vector3{-2, 3, 4}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{-4, 9, 16}), testkit::kTolerance);
    ASSERT_TRUE(Configure(R"({"operation":3,"axis":[0,1,0],"angleDegrees":90})"));
    ASSERT_TRUE(Tick());
    EXPECT_QUAT_NEAR(target.transform.rotation, math::Quaternion::FromAxisAngle(math::Vector3::UP, math::HALF_PI), testkit::kTolerance);
    EXPECT_VEC3_NEAR(Vector("forward"), target.transform.Forward(), testkit::kTolerance);
    ASSERT_TRUE(Configure(R"({"operation":4,"lookTarget":[10,0,0]})"));
    ASSERT_TRUE(Tick());
    EXPECT_VEC3_NEAR(target.transform.Forward(), math::Vector3::RIGHT, testkit::kTolerance);
}

TEST_F(ManagedTransformPoseTest, FailedDescendantPreflightRetainsOriginalSceneAndFaultDiagnostic)
{
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(child.SetParent(m_owner));
    child.transform.position = {std::numeric_limits<float>::max(), 0, 0};
    ASSERT_TRUE(Configure(R"({"operation":2,"scale":[2,1,1]})"));
    Tick();
    EXPECT_TRUE(m_adapter->IsRuntimeFaulted());
    EXPECT_FALSE(m_adapter->FaultDiagnostic().empty());
    EXPECT_VEC3_NEAR(m_owner->transform.scale, math::Vector3::ONE, testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(ManagedTransformPoseTest, CachedTransformRejectsDestroyedTargetWithReusedEntityAndGuid)
{
    auto& target = m_scene.CreateGameObject("Target");
    const scene::EntityID oldId = target.GetID();
    const std::string guid = target.instanceId;
    ASSERT_TRUE(Configure("{\"target\":\"" + guid + "\",\"useCached\":true,\"operation\":2,\"scale\":[2,3,4]}"));
    ASSERT_TRUE(m_adapter->ExecuteProfiledCallback(&scene::Script::OnAwake, scene::ScriptCallbackKind::AWAKE, "OnAwake"));
    ASSERT_TRUE(m_scene.DestroyGameObject(oldId));
    auto& recreated = m_scene.CreateGameObject("Recreated");
    recreated.instanceId = guid;
    Tick();
    EXPECT_TRUE(m_adapter->IsRuntimeFaulted());
    EXPECT_VEC3_NEAR(recreated.transform.scale, math::Vector3::ONE, testkit::kTolerance);
}

} /// @note namespace fbzz::tests
