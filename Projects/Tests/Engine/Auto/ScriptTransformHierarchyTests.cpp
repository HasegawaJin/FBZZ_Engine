/// @file    ScriptTransformHierarchyTests.cpp
/// @brief   Script の姿勢編集が親変換・子孫・物理前の同期をまたいで整合する契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Math/MathUtils.hpp>
#include <Physics/World.hpp>

namespace fbzz::tests {
namespace {

class TransformHierarchyProbe final : public scene::Script {};

} /// @note namespace

class ScriptTransformHierarchyTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_parent = &m_scene.CreateGameObject("Parent");
        m_child = &m_scene.CreateGameObject("Child");
        m_descendant = &m_scene.CreateGameObject("Descendant");
        ASSERT_TRUE(m_child->SetParent(m_parent));
        ASSERT_TRUE(m_descendant->SetParent(m_child));
        m_parent->transform.position = {10.0f, 20.0f, 30.0f};
        m_parent->transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));
        m_parent->transform.scale = {2.0f, 3.0f, -4.0f};
        m_descendant->transform.position = math::Vector3::FORWARD;
        m_script.SetContext(&m_scene, m_child);
        scene::FlushWorldTransforms(m_scene);
    }

    void UpdatePrePhysics()
    {
        SystemContext context{m_scene, m_world, nullptr, nullptr, 0.0f, 1.0f / 60.0f, true, true};
        scene::TransformPrePhysics system;
        system.Update(context);
    }

    scene::Scene m_scene;
    physics::World m_world;
    TransformHierarchyProbe m_script;
    scene::GameObject* m_parent = nullptr;
    scene::GameObject* m_child = nullptr;
    scene::GameObject* m_descendant = nullptr;
};

TEST_F(ScriptTransformHierarchyTest, PublishesLocalPositionToInactiveDescendantsImmediately)
{
    m_descendant->SetActive(false);
    auto& unrelated = m_scene.CreateGameObject("Unrelated");
    unrelated.transform.worldPosition = {99.0f, 98.0f, 97.0f};

    m_script.transform.position = {1.0f, 2.0f, 3.0f};

    EXPECT_VEC3_NEAR(m_script.transform.worldPosition, math::Vector3(-2.0f, 26.0f, 28.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(m_descendant->transform.worldPosition, math::Vector3(-6.0f, 26.0f, 28.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(unrelated.transform.worldPosition, math::Vector3(99.0f, 98.0f, 97.0f), testkit::kTolerance);
}

TEST_F(ScriptTransformHierarchyTest, PublishesLocalRotationAndScaleToDescendantsImmediately)
{
    m_script.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));
    EXPECT_VEC3_NEAR(m_script.transform.forward, math::Vector3(0.0f, 0.0f, -1.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(m_descendant->transform.worldPosition, math::Vector3(10.0f, 20.0f, 34.0f), testkit::kLooseTolerance);

    m_script.transform.scale = {2.0f, 2.0f, 2.0f};

    EXPECT_VEC3_NEAR(m_child->transform.worldScale, math::Vector3(4.0f, 6.0f, -8.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_descendant->transform.worldPosition, math::Vector3(10.0f, 20.0f, 38.0f), testkit::kLooseTolerance);
}

TEST_F(ScriptTransformHierarchyTest, InvertsRotatedNegativeParentScaleAndSurvivesPrePhysics)
{
    m_script.transform.worldPosition = {2.0f, 26.0f, 24.0f};

    EXPECT_VEC3_NEAR(m_script.transform.position, math::Vector3(3.0f, 2.0f, 2.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(m_script.transform.worldPosition, math::Vector3(2.0f, 26.0f, 24.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(m_descendant->transform.worldPosition, math::Vector3(-2.0f, 26.0f, 24.0f), testkit::kLooseTolerance);
    EXPECT_QUAT_NEAR(m_script.transform.rotation, math::Quaternion::Identity(), testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_script.transform.scale, math::Vector3::ONE, testkit::kTolerance);

    UpdatePrePhysics();

    EXPECT_VEC3_NEAR(m_script.transform.worldPosition, math::Vector3(2.0f, 26.0f, 24.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(m_descendant->transform.worldPosition, math::Vector3(-2.0f, 26.0f, 24.0f), testkit::kLooseTolerance);
}

TEST_F(ScriptTransformHierarchyTest, ResolvesChangedAncestorsBeforeInvertingTheWorldPosition)
{
    auto& ancestor = m_scene.CreateGameObject("Ancestor");
    ancestor.transform.position = {5.0f, 0.0f, 0.0f};
    ASSERT_TRUE(m_parent->SetParent(&ancestor));
    m_parent->transform.position = {20.0f, 0.0f, 0.0f};
    /// @note 描画補間や未同期のキャッシュを、ローカル位置の逆変換へ持ち込まない。
    m_parent->transform.worldPosition = {1000.0f, 1000.0f, 1000.0f};
    m_parent->transform.worldRotation = math::Quaternion::Identity();

    m_script.transform.worldPosition = {21.0f, 6.0f, -6.0f};

    EXPECT_VEC3_NEAR(ancestor.transform.worldPosition, math::Vector3(5.0f, 0.0f, 0.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_parent->transform.worldPosition, math::Vector3(25.0f, 0.0f, 0.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_script.transform.position, math::Vector3(3.0f, 2.0f, 1.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(m_script.transform.worldPosition, math::Vector3(21.0f, 6.0f, -6.0f), testkit::kLooseTolerance);
    UpdatePrePhysics();
    EXPECT_VEC3_NEAR(m_script.transform.worldPosition, math::Vector3(21.0f, 6.0f, -6.0f), testkit::kLooseTolerance);
}

TEST_F(ScriptTransformHierarchyTest, ProjectsSingularParentAxesWithoutChangingLocalRotationOrScale)
{
    m_parent->transform.scale = {0.0f, 3.0f, -4.0f};
    const auto rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(45.0f));
    m_child->transform.rotation = rotation;
    m_child->transform.scale = {5.0f, 6.0f, 7.0f};

    m_script.transform.worldPosition = {2.0f, 26.0f, 24.0f};

    EXPECT_VEC3_NEAR(m_script.transform.position, math::Vector3(0.0f, 2.0f, 2.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(m_script.transform.worldPosition, math::Vector3(2.0f, 26.0f, 30.0f), testkit::kLooseTolerance);
    EXPECT_QUAT_NEAR(m_script.transform.rotation, rotation, testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_script.transform.scale, math::Vector3(5.0f, 6.0f, 7.0f), testkit::kTolerance);
    UpdatePrePhysics();
    EXPECT_VEC3_NEAR(m_script.transform.worldPosition, math::Vector3(2.0f, 26.0f, 30.0f), testkit::kLooseTolerance);
}

TEST_F(ScriptTransformHierarchyTest, PublishesTranslateLookAtAndRotateWithoutAFrameBoundary)
{
    m_script.transform.Translate(math::Vector3::FORWARD);
    EXPECT_VEC3_NEAR(m_script.transform.worldPosition, math::Vector3(6.0f, 20.0f, 30.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(m_descendant->transform.worldPosition, math::Vector3(2.0f, 20.0f, 30.0f), testkit::kLooseTolerance);

    m_script.transform.LookAt({6.0f, 20.0f, 40.0f});
    EXPECT_VEC3_NEAR(m_script.transform.forward, math::Vector3::FORWARD, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(m_descendant->transform.worldPosition, math::Vector3(6.0f, 20.0f, 26.0f), testkit::kLooseTolerance);

    m_script.transform.Rotate(math::Vector3::UP, 90.0f);
    EXPECT_VEC3_NEAR(m_script.transform.forward, math::Vector3::RIGHT, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(m_descendant->transform.worldPosition, math::Vector3(2.0f, 20.0f, 30.0f), testkit::kLooseTolerance);
}

TEST_F(ScriptTransformHierarchyTest, PreservesThePublishedRenderPoseWhenOnlyReading)
{
    m_child->transform.worldPosition = {12.0f, 22.0f, 32.0f};
    m_child->transform.worldRotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(180.0f));

    EXPECT_VEC3_NEAR(m_script.transform.worldPosition, math::Vector3(12.0f, 22.0f, 32.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(m_script.transform.forward, math::Vector3(0.0f, 0.0f, -1.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(m_child->transform.worldPosition, math::Vector3(12.0f, 22.0f, 32.0f), testkit::kTolerance);
}

TEST_F(ScriptTransformHierarchyTest, CapturesWorldArgumentsBeforeRefreshingAnAliasedTransform)
{
    m_child->transform.worldPosition = {2.0f, 26.0f, 24.0f};

    m_script.transform.worldPosition = m_child->transform.worldPosition;

    EXPECT_VEC3_NEAR(m_script.transform.worldPosition, math::Vector3(2.0f, 26.0f, 24.0f), testkit::kLooseTolerance);
    m_child->transform.worldPosition = {2.0f, 26.0f, 34.0f};
    m_script.transform.LookAt(m_child->transform.worldPosition);
    EXPECT_VEC3_NEAR(m_script.transform.forward, math::Vector3::FORWARD, testkit::kLooseTolerance);
}

} /// @note namespace fbzz::tests
