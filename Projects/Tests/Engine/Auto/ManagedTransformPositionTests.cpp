/// @file    ManagedTransformPositionTests.cpp
/// @brief   Scalar managed position changes preserve finite hierarchy state and reject invalid results atomically.
/// @author  Hasegawa Jin
/// @date    2026-10-05
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Math/MathUtils.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedTransformBatch.hpp"
#include <limits>
#include <memory>

namespace fbzz::tests {

class ManagedTransformPositionTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::managed_detail::ManagedTransformBatch m_kernel;

    scene::ManagedBridgeStatus Run(scene::GameObject& target, scene::ManagedBridgeOp operation,
        const math::Vector3& value)
    {
        scene::ManagedBridgeRequest request;
        request.op = operation;
        request.x = value.x;
        request.y = value.y;
        request.z = value.z;
        return m_kernel.RunPosition(target, request);
    }

    void ExpectUnchanged(const scene::Transform& actual, const scene::Transform& expected)
    {
        EXPECT_VEC3_NEAR(actual.position, expected.position, testkit::kTolerance);
        EXPECT_VEC3_NEAR(actual.scale, expected.scale, testkit::kTolerance);
        EXPECT_VEC3_NEAR(actual.worldPosition, expected.worldPosition, testkit::kTolerance);
        EXPECT_VEC3_NEAR(actual.worldScale, expected.worldScale, testkit::kTolerance);
        /// @note Rejection preserves quaternion components, including nonunit local values, rather than only orientation.
        EXPECT_FLOAT_EQ(actual.rotation.x, expected.rotation.x);
        EXPECT_FLOAT_EQ(actual.rotation.y, expected.rotation.y);
        EXPECT_FLOAT_EQ(actual.rotation.z, expected.rotation.z);
        EXPECT_FLOAT_EQ(actual.rotation.w, expected.rotation.w);
        EXPECT_FLOAT_EQ(actual.worldRotation.x, expected.worldRotation.x);
        EXPECT_FLOAT_EQ(actual.worldRotation.y, expected.worldRotation.y);
        EXPECT_FLOAT_EQ(actual.worldRotation.z, expected.worldRotation.z);
        EXPECT_FLOAT_EQ(actual.worldRotation.w, expected.worldRotation.w);
    }
};

TEST_F(ManagedTransformPositionTest, LocalSetterSynchronizesScaledHierarchyAndInactiveChildren)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& target = m_scene.CreateGameObject("Target");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(target.SetParent(&parent));
    ASSERT_TRUE(child.SetParent(&target));
    parent.transform.position = {10, 20, 30};
    parent.transform.scale = {2, 3, 4};
    target.transform.rotation = {0, 0, 0, 2};
    target.transform.scale = {-1, 2, 0};
    child.transform.position = {1, 0, 0};
    child.SetActive(false);

    ASSERT_EQ(Run(target, scene::ManagedBridgeOp::TRANSFORM_WRITE_LOCAL, {2, 1, 3}), scene::ManagedBridgeStatus::OK);

    EXPECT_VEC3_NEAR(target.transform.position, (math::Vector3{2, 1, 3}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(target.transform.worldPosition, (math::Vector3{14, 23, 42}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{12, 23, 42}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(parent.transform.worldPosition, parent.transform.position, testkit::kTolerance);
    EXPECT_VEC3_NEAR(target.transform.scale, (math::Vector3{-1, 2, 0}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(target.transform.rotation.w, 2);
}

TEST_F(ManagedTransformPositionTest, WorldSetterPreservesFiniteProjectionThroughZeroParentScale)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& target = m_scene.CreateGameObject("Target");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(target.SetParent(&parent));
    ASSERT_TRUE(child.SetParent(&target));
    parent.transform.position = {10, 20, 30};
    parent.transform.scale = {0, 2, -3};
    target.transform.position = {3, 4, 5};
    target.transform.rotation = {0, 0, 0, 2};
    target.transform.scale = {-2, 1, 0};
    child.transform.position = {1, 1, 1};

    ASSERT_EQ(Run(target, scene::ManagedBridgeOp::TRANSFORM_WRITE_WORLD, {999, 26, 21}), scene::ManagedBridgeStatus::OK);

    EXPECT_VEC3_NEAR(target.transform.position, (math::Vector3{0, 3, 3}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(target.transform.worldPosition, (math::Vector3{10, 26, 21}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{10, 28, 21}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(target.transform.scale, (math::Vector3{-2, 1, 0}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(target.transform.rotation.w, 2);
}

TEST_F(ManagedTransformPositionTest, LocalTranslationUsesLocalRotationWithoutApplyingTargetScaleToDelta)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& target = m_scene.CreateGameObject("Target");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(target.SetParent(&parent));
    ASSERT_TRUE(child.SetParent(&target));
    parent.transform.position = {10, 20, 30};
    parent.transform.scale = {2, 3, 4};
    target.transform.position = {1, 2, 3};
    target.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::HALF_PI);
    target.transform.scale = {5, 6, 7};
    child.transform.position = {1, 0, 0};
    const math::Vector3 delta{0, 0, 2};
    const math::Vector3 expectedLocal = target.transform.position + target.transform.rotation * delta;
    scene::Transform expectedTarget = target.transform;
    expectedTarget.position = expectedLocal;
    scene::Transform expectedParent = parent.transform;
    scene::UpdateWorldTransform(expectedParent, nullptr);
    scene::UpdateWorldTransform(expectedTarget, &expectedParent);
    scene::Transform expectedChild = child.transform;
    scene::UpdateWorldTransform(expectedChild, &expectedTarget);

    ASSERT_EQ(Run(target, scene::ManagedBridgeOp::TRANSFORM_TRANSLATE_LOCAL, delta), scene::ManagedBridgeStatus::OK);

    EXPECT_VEC3_NEAR(target.transform.position, expectedLocal, testkit::kTolerance);
    EXPECT_VEC3_NEAR(target.transform.worldPosition, expectedTarget.worldPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, expectedChild.worldPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(target.transform.scale, (math::Vector3{5, 6, 7}), testkit::kTolerance);
}

TEST_F(ManagedTransformPositionTest, LocalSetterRejectsParentAdditionOverflowWithoutRepairingLiveCaches)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& target = m_scene.CreateGameObject("Target");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(target.SetParent(&parent));
    ASSERT_TRUE(child.SetParent(&target));
    const float large = std::numeric_limits<float>::max() * 0.75f;
    parent.transform.position = {large, 0, 0};
    parent.transform.worldPosition = {99, 0, 0};
    target.transform.worldPosition = {88, 0, 0};
    child.transform.worldPosition = {77, 0, 0};
    const scene::Transform previousParent = parent.transform;
    const scene::Transform previousTarget = target.transform;
    const scene::Transform previousChild = child.transform;

    EXPECT_EQ(Run(target, scene::ManagedBridgeOp::TRANSFORM_WRITE_LOCAL, {large, 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);

    ExpectUnchanged(parent.transform, previousParent);
    ExpectUnchanged(target.transform, previousTarget);
    ExpectUnchanged(child.transform, previousChild);
}

TEST_F(ManagedTransformPositionTest, LocalTranslationRejectsAccumulationOverflowWithoutMutation)
{
    auto& target = m_scene.CreateGameObject("Target");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(child.SetParent(&target));
    const float large = std::numeric_limits<float>::max() * 0.75f;
    target.transform.position = {large, 0, 0};
    target.transform.worldPosition = {88, 0, 0};
    child.transform.worldPosition = {77, 0, 0};
    const scene::Transform previousTarget = target.transform;
    const scene::Transform previousChild = child.transform;

    EXPECT_EQ(Run(target, scene::ManagedBridgeOp::TRANSFORM_TRANSLATE_LOCAL, {large, 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);

    ExpectUnchanged(target.transform, previousTarget);
    ExpectUnchanged(child.transform, previousChild);
}

TEST_F(ManagedTransformPositionTest, WorldSetterRejectsDescendantOverflowWithoutChangingAncestorsOrChildren)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& target = m_scene.CreateGameObject("Target");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(target.SetParent(&parent));
    ASSERT_TRUE(child.SetParent(&target));
    const float large = std::numeric_limits<float>::max() * 0.75f;
    parent.transform.position = {10, 0, 0};
    parent.transform.worldPosition = {99, 0, 0};
    target.transform.worldPosition = {88, 0, 0};
    child.transform.position = {large, 0, 0};
    child.transform.worldPosition = {77, 0, 0};
    const scene::Transform previousParent = parent.transform;
    const scene::Transform previousTarget = target.transform;
    const scene::Transform previousChild = child.transform;

    EXPECT_EQ(Run(target, scene::ManagedBridgeOp::TRANSFORM_WRITE_WORLD, {large, 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);

    ExpectUnchanged(parent.transform, previousParent);
    ExpectUnchanged(target.transform, previousTarget);
    ExpectUnchanged(child.transform, previousChild);
}

TEST_F(ManagedTransformPositionTest, WorldSetterRejectsOffsetOverflowHiddenByZeroParentScale)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& target = m_scene.CreateGameObject("Target");
    ASSERT_TRUE(target.SetParent(&parent));
    const float large = std::numeric_limits<float>::max() * 0.75f;
    parent.transform.position = {-large, 0, 0};
    parent.transform.scale = {0, 1, 1};
    parent.transform.worldPosition = {99, 0, 0};
    target.transform.worldPosition = {88, 0, 0};
    const scene::Transform previousParent = parent.transform;
    const scene::Transform previousTarget = target.transform;

    EXPECT_EQ(Run(target, scene::ManagedBridgeOp::TRANSFORM_WRITE_WORLD, {large, 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);

    ExpectUnchanged(parent.transform, previousParent);
    ExpectUnchanged(target.transform, previousTarget);
}

TEST_F(ManagedTransformPositionTest, NonfiniteInputLeavesPositionAndWorldCacheUnchanged)
{
    auto& target = m_scene.CreateGameObject("Target");
    target.transform.position = {1, 2, 3};
    target.transform.worldPosition = {88, 0, 0};
    const scene::Transform previous = target.transform;
    const scene::ManagedBridgeOp operations[] = {
        scene::ManagedBridgeOp::TRANSFORM_WRITE_LOCAL,
        scene::ManagedBridgeOp::TRANSFORM_WRITE_WORLD,
        scene::ManagedBridgeOp::TRANSFORM_TRANSLATE_LOCAL
    };

    for (auto operation : operations) {
        SCOPED_TRACE(static_cast<uint32_t>(operation));
        EXPECT_EQ(Run(target, operation, {std::numeric_limits<float>::quiet_NaN(), 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
        EXPECT_EQ(Run(target, operation, {0, std::numeric_limits<float>::infinity(), 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
        ExpectUnchanged(target.transform, previous);
    }
}

TEST_F(ManagedTransformPositionTest, PreservesExistingScalarPositionSupportForLivePhysicsBodies)
{
    auto& target = m_scene.CreateGameObject("Target");
    target.AddComponent<scene::RigidBodyComponent>().rigidBody = std::make_unique<physics::RigidBody>();

    ASSERT_EQ(Run(target, scene::ManagedBridgeOp::TRANSFORM_WRITE_LOCAL, {1, 2, 3}), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(Run(target, scene::ManagedBridgeOp::TRANSFORM_WRITE_WORLD, {4, 5, 6}), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(Run(target, scene::ManagedBridgeOp::TRANSFORM_TRANSLATE_LOCAL, {1, 0, 0}), scene::ManagedBridgeStatus::OK);

    EXPECT_VEC3_NEAR(target.transform.position, (math::Vector3{5, 5, 6}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(target.transform.worldPosition, (math::Vector3{5, 5, 6}), testkit::kTolerance);
}

} /// @note namespace fbzz::tests
