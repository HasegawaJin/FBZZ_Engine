/// @file    ScriptSceneQueryTests.cpp
/// @brief   スクリプトから使う投影・階層検索・形状境界の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-13
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/ScriptEvent.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Scene/Systems/ColliderSync.hpp>
#include <Math/MathUtils.hpp>
#include <limits>

namespace fbzz::tests {
namespace {
class SceneQueryProbe final : public scene::Script {};
}

class ScriptSceneQueryTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    SceneQueryProbe m_probe;
    scene::EntityID m_manager;
    scene::EntityID m_camera;
    scene::ScriptRuntime m_runtime;
    scene::ScriptRuntime* m_previousRuntime = nullptr;

    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        m_previousRuntime = scene::ScriptRuntime::GetOverride();
        m_runtime.viewportWidth = 1600;
        m_runtime.viewportHeight = 900;
        scene::ScriptRuntime::Override(&m_runtime);
        m_manager = m_scene.CreateGameObject("Manager").GetID();
        m_camera = m_scene.CreateGameObject("Camera").GetID();
        auto& component = Object(m_camera).AddComponent<scene::CameraComponent>();
        component.fovY = 90.0f;
        component.aspectRatio = 2.0f;
        component.nearZ = 0.1f;
        component.farZ = 100.0f;
        m_probe.SetContext(&m_scene, &Object(m_manager));
    }

    void TearDown() override
    {
        scene::ScriptRuntime::Override(m_previousRuntime);
        testkit::EngineFixture::TearDown();
    }

    scene::GameObject& Object(scene::EntityID id) { return *m_scene.GetGameObject(id); }

    scene::EntityID Child(scene::EntityID parent, const char* name)
    {
        const auto id = m_scene.CreateGameObject(name).GetID();
        Object(id).SetParent(Object(parent));
        return id;
    }
};

TEST_F(ScriptSceneQueryTest, ManagerCanProjectThroughMainCameraAndUnprojectAtMetricDepth)
{
    math::Vector3 viewport;
    ASSERT_TRUE(m_probe.camera.TryWorldToViewportPoint({10.0f, 5.0f, 10.0f}, viewport));
    EXPECT_NEAR(viewport.x, 0.75f, 1.0e-5f);
    EXPECT_NEAR(viewport.y, 0.25f, 1.0e-5f);
    math::Vector3 world;
    ASSERT_TRUE(m_probe.camera.TryViewportToWorldPoint({viewport.x, viewport.y}, 10.0f, world));
    const math::Vector3 expected{10.0f, 5.0f, 10.0f};
    EXPECT_VEC3_NEAR(world, expected, 1.0e-4f);
}

TEST_F(ScriptSceneQueryTest, ExplicitCameraUsesItsWorldPoseAndOverridesMainCamera)
{
    auto& cameraObject = Object(m_camera);
    cameraObject.GetComponent<scene::CameraComponent>()->isMain = false;
    cameraObject.transform.worldPosition = {5.0f, 3.0f, -8.0f};
    cameraObject.transform.worldRotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, 0.7f);
    math::Vector3 world;
    ASSERT_TRUE(m_probe.camera.TryViewportToWorldPoint({0.2f, 0.8f}, 7.0f, world, &cameraObject));
    math::Vector3 viewport;
    ASSERT_TRUE(m_probe.camera.TryWorldToViewportPoint(world, viewport, &cameraObject));
    EXPECT_NEAR(viewport.x, 0.2f, 1.0e-5f);
    EXPECT_NEAR(viewport.y, 0.8f, 1.0e-5f);
    EXPECT_FALSE(m_probe.camera.TryWorldToViewportPoint(world, viewport));
}

TEST_F(ScriptSceneQueryTest, FailedProjectionLeavesOutputUntouchedAndVisibilityClipsDepth)
{
    const math::Vector3 sentinel{7.0f, 8.0f, 9.0f};
    math::Vector3 viewport = sentinel;
    EXPECT_FALSE(m_probe.camera.TryWorldToViewportPoint({0, 0, -10}, viewport));
    EXPECT_FALSE(m_probe.camera.TryWorldToViewportPoint({1, 0, 0}, viewport));
    EXPECT_FALSE(m_probe.camera.TryWorldToViewportPoint(
        {std::numeric_limits<float>::quiet_NaN(), 0, 10}, viewport));
    EXPECT_VEC3_NEAR(viewport, sentinel, 1.0e-6f);
    EXPECT_FALSE(m_probe.camera.IsVisible({0, 0, -10}));
    EXPECT_FALSE(m_probe.camera.IsVisible({0, 0, 0.01f}));
    EXPECT_FALSE(m_probe.camera.IsVisible({0, 0, 200}));
    EXPECT_TRUE(m_probe.camera.IsVisible({0, 0, 10}));
    ASSERT_TRUE(m_probe.camera.TryWorldToViewportPoint({100, 0, 10}, viewport));
    EXPECT_GT(viewport.x, 1.0f);
    EXPECT_FALSE(m_probe.camera.IsVisible({100, 0, 10}));
    Object(m_camera).GetComponent<scene::CameraComponent>()->enabled = false;
    viewport = sentinel;
    EXPECT_FALSE(m_probe.camera.TryWorldToViewportPoint({0, 0, 10}, viewport));
    EXPECT_VEC3_NEAR(viewport, sentinel, 1.0e-6f);
}

TEST_F(ScriptSceneQueryTest, InvalidLensAndUnprojectionInputsAreRejected)
{
    const math::Vector3 sentinel{7, 8, 9};
    math::Vector3 world = sentinel;
    EXPECT_FALSE(m_probe.camera.TryViewportToWorldPoint({0.5f, 0.5f}, 0.0f, world));
    EXPECT_FALSE(m_probe.camera.TryViewportToWorldPoint({0.5f, 0.5f}, -1.0f, world));
    auto* lens = Object(m_camera).GetComponent<scene::CameraComponent>();
    lens->aspectRatio = 0.0f;
    EXPECT_FALSE(m_probe.camera.TryViewportToWorldPoint({0.5f, 0.5f}, 10.0f, world));
    EXPECT_FALSE(m_probe.camera.TryWorldToViewportPoint({0, 0, 10}, world));
    EXPECT_VEC3_NEAR(world, sentinel, 1.0e-6f);
}

TEST_F(ScriptSceneQueryTest, SubtreeSearchIncludesRootAndInactiveNodesInDepthFirstSiblingOrder)
{
    const auto left = Child(m_manager, "Left");
    const auto first = Child(left, "Socket");
    const auto right = Child(m_manager, "Socket");
    const auto unrelated = Child(m_camera, "Socket");
    Object(left).SetActive(false);
    EXPECT_EQ(Object(m_manager).FindInSubtree("Manager"), &Object(m_manager));
    EXPECT_EQ(Object(m_manager).FindInSubtree("Socket"), &Object(first));
    EXPECT_EQ(Object(m_camera).FindInSubtree("Socket"), &Object(unrelated));
    EXPECT_EQ(Object(left).FindInSubtree("Missing"), nullptr);
    EXPECT_TRUE(Object(right).SetSiblingIndex(0));
    EXPECT_EQ(Object(m_manager).FindInSubtree("Socket"), &Object(right));
}

TEST_F(ScriptSceneQueryTest, PrimitiveBoundsIncludeRotationAndMirroredCenterWithoutChangingPhysics)
{
    auto& object = Object(m_manager);
    auto& box = object.AddComponent<scene::BoxColliderComponent>();
    box.size = {2, 4, 6};
    box.center = {0, 1, 0};
    scene::SyncColliderShape(box, math::Vector3::ONE);
    const auto oldBounds = box.collider->GetAABB();
    object.transform.worldPosition = {10, 20, 30};
    object.transform.worldScale = {2, -3, 0.5f};
    object.transform.worldRotation = math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, math::PI * 0.5f);
    math::Vector3 minimum, maximum;
    ASSERT_TRUE(scene::ScriptColliderProxy::TryGetPrimitiveWorldBounds(&object, minimum, maximum));
    const math::Vector3 expectedMin{7, 18, 28.5f}, expectedMax{19, 22, 31.5f};
    EXPECT_VEC3_NEAR(minimum, expectedMin, 1.0e-4f);
    EXPECT_VEC3_NEAR(maximum, expectedMax, 1.0e-4f);
    EXPECT_VEC3_NEAR(box.collider->GetAABB().min, oldBounds.min, 1.0e-6f);
    EXPECT_VEC3_NEAR(box.collider->GetAABB().max, oldBounds.max, 1.0e-6f);
    scene::PrepareCollider(m_scene, object, box);
    EXPECT_VEC3_NEAR(minimum, box.collider->GetAABB().min, 1.0e-4f);
    EXPECT_VEC3_NEAR(maximum, box.collider->GetAABB().max, 1.0e-4f);
}

TEST_F(ScriptSceneQueryTest, BoundsMergeAuthoredPrimitivesAndPreserveOutputsWhenMissing)
{
    auto& object = Object(m_manager);
    math::Vector3 minimum{7, 8, 9}, maximum{10, 11, 12};
    EXPECT_FALSE(scene::ScriptColliderProxy::TryGetPrimitiveWorldBounds(&object, minimum, maximum));
    EXPECT_NEAR(minimum.x, 7.0f, 1.0e-6f);
    EXPECT_NEAR(maximum.z, 12.0f, 1.0e-6f);
    auto& sphere = object.AddComponent<scene::SphereColliderComponent>();
    sphere.radius = 1.0f;
    sphere.center = {-3, 0, 0};
    sphere.enabled = false;
    auto& capsule = object.AddComponent<scene::CapsuleColliderComponent>();
    capsule.radius = 0.5f;
    capsule.halfHeight = 2.0f;
    capsule.center = {3, 0, 0};
    ASSERT_TRUE(scene::ScriptColliderProxy::TryGetPrimitiveWorldBounds(&object, minimum, maximum));
    const math::Vector3 expectedMin{-4, -2.5f, -1}, expectedMax{3.5f, 2.5f, 1};
    EXPECT_VEC3_NEAR(minimum, expectedMin, 1.0e-5f);
    EXPECT_VEC3_NEAR(maximum, expectedMax, 1.0e-5f);
}

TEST_F(ScriptSceneQueryTest, CanvasSizeUsesViewportAndRenderModeWithoutACamera)
{
    auto& canvas = Object(m_manager).AddComponent<scene::UICanvas>();
    canvas.renderMode = scene::UIRenderMode::ScreenSpaceOverlay;
    canvas.scaleMode = scene::UICanvasScaleMode::ScaleWithScreenSize;
    canvas.referenceWidth = 1000.0f;
    canvas.referenceHeight = 500.0f;
    canvas.matchWidthOrHeight = 0.0f;
    Object(m_camera).GetComponent<scene::CameraComponent>()->enabled = false;
    math::Vector2 size;
    ASSERT_TRUE(m_probe.ui.TryGetCanvasSize(size));
    EXPECT_NEAR(size.x, 1000.0f, 1.0e-3f);
    EXPECT_NEAR(size.y, 562.5f, 1.0e-3f);
    canvas.matchWidthOrHeight = 1.0f;
    ASSERT_TRUE(m_probe.ui.TryGetCanvasSize(size));
    EXPECT_NEAR(size.x, 1600.0f / 1.8f, 1.0e-3f);
    EXPECT_NEAR(size.y, 500.0f, 1.0e-3f);
    canvas.renderMode = scene::UIRenderMode::WorldSpace;
    canvas.canvasWidth = 200.0f;
    canvas.canvasHeight = 100.0f;
    ASSERT_TRUE(m_probe.ui.TryGetCanvasSize(size));
    EXPECT_NEAR(size.x, 200.0f, 1.0e-5f);
    EXPECT_NEAR(size.y, 100.0f, 1.0e-5f);
}

/// @note スクリプト向けの検索は Unity と同じく既定で有効な物だけを返し、includeInactive で無効な物も探せる。
TEST_F(ScriptSceneQueryTest, FindSkipsInactiveHierarchyUnlessAsked)
{
    const auto group = Child(m_manager, "Group");
    const auto hidden = Child(group, "Hidden");
    Object(hidden).tag = "Target";
    Object(hidden).AddComponent<scene::CameraComponent>();
    Object(group).SetActive(false);

    EXPECT_EQ(m_probe.scene.Find("Hidden"), nullptr);
    EXPECT_EQ(m_probe.scene.FindWithTag("Target"), nullptr);
    EXPECT_EQ(m_probe.scene.Find("Hidden", true), &Object(hidden));
    EXPECT_EQ(m_probe.scene.FindWithTag("Target", true), &Object(hidden));

    const auto activeCameras = m_probe.scene.FindObjectsOfType<scene::CameraComponent>();
    const auto allCameras = m_probe.scene.FindObjectsOfType<scene::CameraComponent>(true);
    EXPECT_EQ(activeCameras.size(), 1u);
    EXPECT_EQ(allCameras.size(), 2u);

    /// @note 参照解決用の Scene::Find は無効な物も返す (保存・プレハブ・エディターが使う)。
    EXPECT_EQ(m_scene.Find("Hidden"), &Object(hidden));

    Object(group).SetActive(true);
    EXPECT_EQ(m_probe.scene.Find("Hidden"), &Object(hidden));
}

/// @note 無効なスクリプトには配らないが、購読は残るので有効に戻せばまた届く。
TEST_F(ScriptSceneQueryTest, EventBusSkipsInactiveOwnersAndResumes)
{
    scene::ScriptEventBus::Clear();
    int received = 0;
    scene::ScriptEventBus::SubscribeRaw(&m_probe, "Test.Ping", [&](const void*) { ++received; });

    scene::ScriptEventBus::PublishRaw("Test.Ping", nullptr);
    EXPECT_EQ(received, 1);

    Object(m_manager).SetActive(false);
    scene::ScriptEventBus::PublishRaw("Test.Ping", nullptr);
    EXPECT_EQ(received, 1);

    Object(m_manager).SetActive(true);
    m_probe.enabled = false;
    scene::ScriptEventBus::PublishRaw("Test.Ping", nullptr);
    EXPECT_EQ(received, 1);

    m_probe.enabled = true;
    scene::ScriptEventBus::PublishRaw("Test.Ping", nullptr);
    EXPECT_EQ(received, 2);
    scene::ScriptEventBus::Clear();
}

} // namespace fbzz::tests
