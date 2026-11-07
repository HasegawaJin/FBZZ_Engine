/// @file    PlacementOperatorTests.cpp
/// @brief   制作配置の計画と適用、親子座標・静的底面・履歴競合の公開契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-06
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <limits>

namespace fbzz::tests {

class PlacementOperatorTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        (void)AttachScene();
        Context().projectRoot = ProjectRoot().generic_string();
        Context().markSceneDirty = [this]() { ++m_dirtyCalls; };
        editor::RegisterNodeOperators(m_registry);
    }
    void TearDown() override
    {
        m_undo.Clear();
        Context().playMode = nullptr;
        EditorFixture::TearDown();
    }
    editor::OpArgs Request(const char* operation, const std::vector<scene::GameObject*>& objects)
    {
        editor::OpArgs args;
        args.Set("operation", std::string(operation));
        auto nodes = editor::OpData::MakeArray();
        for (const auto* object : objects) nodes.Push(object->instanceId);
        args.Set("nodes", std::move(nodes));
        return args;
    }
    editor::OpResult Invoke(const char* id, const editor::OpArgs& args = {})
    {
        editor::OpContext context{Context(), m_undo};
        return m_registry.Invoke(id, context, args);
    }
    editor::OpResult Apply(editor::OpArgs args)
    {
        auto preview = Invoke("authoring.placement.preview", args);
        if (!preview.ok) return preview;
        args.Set("expected", preview.data.Find("expected")->AsString());
        return Invoke("authoring.placement.apply", args);
    }
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    int m_dirtyCalls = 0;
};

TEST_F(PlacementOperatorTest, PreviewComputesFromLocalWithoutChangingDerivedWorldOrHistory)
{
    auto& object = AttachedScene()->CreateGameObject("Rock");
    object.transform.position = {2, 3, 4};
    object.transform.worldPosition = {91, 92, 93};
    auto args = Request("align", {&object});
    args.Set("axis", std::string("x")); args.Set("value", 8.0f);
    auto result = Invoke("authoring.placement.preview", args);
    ASSERT_TRUE(result.ok) << result.message;
    ASSERT_EQ(result.data.Find("changes")->AsArray().size(), 1u);
    const auto& change = result.data.Find("changes")->AsArray().front();
    EXPECT_VEC3_NEAR(change.Find("beforeWorld")->AsVec3(), math::Vector3(2, 3, 4), 0.0001f);
    EXPECT_VEC3_NEAR(change.Find("afterWorld")->AsVec3(), math::Vector3(8, 3, 4), 0.0001f);
    EXPECT_VEC3_NEAR(object.transform.position, math::Vector3(2, 3, 4), 0.0001f);
    EXPECT_VEC3_NEAR(object.transform.worldPosition, math::Vector3(91, 92, 93), 0.0001f);
    EXPECT_EQ(m_dirtyCalls, 0); EXPECT_FALSE(Context().sceneDirty); EXPECT_FALSE(m_undo.CanUndo());
}

TEST_F(PlacementOperatorTest, WorldAlignmentUnderRotatedNonuniformParentPreservesPoseAndSynchronizesChildren)
{
    auto& parent = AttachedScene()->CreateGameObject("Parent");
    auto& object = AttachedScene()->CreateGameObject("Rock");
    auto& child = AttachedScene()->CreateGameObject("Decoration");
    ASSERT_TRUE(object.SetParent(&parent)); ASSERT_TRUE(child.SetParent(&object));
    parent.transform.position = {10, 2, 3}; parent.transform.scale = {2, 3, 4};
    parent.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, 0.7f);
    object.transform.position = {1, 2, 3}; object.transform.scale = {0.5f, 0.75f, 1.25f};
    object.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, 0.3f);
    child.transform.position = {0, 1, 0};
    scene::FlushWorldTransforms(*AttachedScene());
    const auto before = object.transform;
    const auto childBefore = child.transform.worldPosition;
    auto args = Request("align", {&object}); args.Set("axis", std::string("y")); args.Set("value", 30.0f);
    auto result = Apply(args); ASSERT_TRUE(result.ok) << result.message;
    EXPECT_NEAR(object.transform.worldPosition.y, 30.0f, 0.0001f);
    EXPECT_NEAR(object.transform.worldPosition.x, before.worldPosition.x, 0.0001f);
    EXPECT_NEAR(object.transform.worldPosition.z, before.worldPosition.z, 0.0001f);
    EXPECT_TRUE(object.transform.rotation == before.rotation);
    EXPECT_VEC3_NEAR(object.transform.scale, before.scale, 0.0001f);
    EXPECT_NEAR(child.transform.worldPosition.y - childBefore.y, 30.0f - before.worldPosition.y, 0.0001f);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_VEC3_NEAR(object.transform.position, before.position, 0.0001f);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, childBefore, 0.0001f);
    ASSERT_TRUE(m_undo.Redo()); EXPECT_NEAR(object.transform.worldPosition.y, 30.0f, 0.0001f);
}

TEST_F(PlacementOperatorTest, DistributionSortsWorldCoordinatesAndPreservesEndpointsAndOtherAxes)
{
    auto& left = AttachedScene()->CreateGameObject("Left");
    auto& middle = AttachedScene()->CreateGameObject("Middle");
    auto& right = AttachedScene()->CreateGameObject("Right");
    left.transform.position = {-4, 1, 2}; middle.transform.position = {-3, 7, 8}; right.transform.position = {8, 9, 10};
    auto result = Apply(Request("distribute", {&right, &left, &middle})); ASSERT_TRUE(result.ok) << result.message;
    EXPECT_VEC3_NEAR(left.transform.position, math::Vector3(-4, 1, 2), 0.0001f);
    EXPECT_VEC3_NEAR(middle.transform.position, math::Vector3(2, 7, 8), 0.0001f);
    EXPECT_VEC3_NEAR(right.transform.position, math::Vector3(8, 9, 10), 0.0001f);
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
}

TEST_F(PlacementOperatorTest, GroundUsesStaticMeshBottomIncludingRotationScaleAndOffset)
{
    auto& object = AttachedScene()->CreateGameObject("Rock");
    renderer::Mesh mesh;
    renderer::Vertex first, second;
    first.position = {-1, -2, -3}; second.position = {2, 4, 5};
    mesh.cpuVertices = {first, second};
    object.AddComponent<scene::MeshRenderer>().mesh = &mesh;
    object.transform.position = {4, 10, 6}; object.transform.scale = {2, -3, 4};
    object.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, 0.5f);
    auto args = Request("ground", {&object}); args.Set("planeHeight", 5.0f); args.Set("offset", 0.25f);
    auto result = Apply(args); ASSERT_TRUE(result.ok) << result.message;
    float minimum = (std::numeric_limits<float>::max)();
    for (const auto& vertex : mesh.cpuVertices) {
        const math::Vector3 scaled{vertex.position.x * object.transform.worldScale.x,
            vertex.position.y * object.transform.worldScale.y, vertex.position.z * object.transform.worldScale.z};
        minimum = (std::min)(minimum, (object.transform.worldPosition + object.transform.worldRotation * scaled).y);
    }
    EXPECT_NEAR(minimum, 5.25f, 0.0001f);
    EXPECT_NEAR(object.transform.worldPosition.x, 4.0f, 0.0001f);
    EXPECT_NEAR(object.transform.worldPosition.z, 6.0f, 0.0001f);
    EXPECT_EQ(result.data.Find("surface")->AsString(), "horizontal-plane");
}

TEST_F(PlacementOperatorTest, GroundRequiresExplicitPlaneAndSupportedAnchor)
{
    auto& object = AttachedScene()->CreateGameObject("Empty");
    object.transform.position = {2, 3, 4};
    auto args = Request("ground", {&object});
    EXPECT_EQ(Invoke("authoring.placement.preview", args).errorCode, "BAD_ARG");
    args.Set("planeHeight", 7.0f);
    EXPECT_EQ(Invoke("authoring.placement.preview", args).errorCode, "NO_STATIC_MESH");
    args.Set("anchor", std::string("origin"));
    ASSERT_TRUE(Apply(args).ok);
    EXPECT_VEC3_NEAR(object.transform.worldPosition, math::Vector3(2, 7, 4), 0.0001f);
}

TEST_F(PlacementOperatorTest, MissingDuplicateAndParentChildTargetsAreRejectedBeforeAnyEdit)
{
    auto& parent = AttachedScene()->CreateGameObject("Parent");
    auto& child = AttachedScene()->CreateGameObject("Child"); ASSERT_TRUE(child.SetParent(&parent));
    auto args = Request("align", {&parent, &child}); args.Set("value", 9.0f);
    EXPECT_EQ(Invoke("authoring.placement.preview", args).errorCode, "OVERLAPPING_HIERARCHY");
    args = Request("align", {&child, &child}); args.Set("value", 9.0f);
    EXPECT_EQ(Invoke("authoring.placement.preview", args).errorCode, "BAD_ARG");
    auto missing = editor::OpData::MakeArray(); missing.Push(child.instanceId); missing.Push("missing");
    args.Set("nodes", missing);
    EXPECT_EQ(Invoke("authoring.placement.preview", args).errorCode, "NO_NODE");
    EXPECT_EQ(m_dirtyCalls, 0); EXPECT_FALSE(m_undo.CanUndo());
}

TEST_F(PlacementOperatorTest, NonfiniteAndSingularParentAreRejected)
{
    auto& parent = AttachedScene()->CreateGameObject("Parent");
    auto& child = AttachedScene()->CreateGameObject("Child"); ASSERT_TRUE(child.SetParent(&parent));
    auto args = Request("align", {&child}); args.Set("value", std::numeric_limits<float>::infinity());
    EXPECT_FALSE(Invoke("authoring.placement.preview", args).ok);
    args.Set("value", 8.0f); parent.transform.scale.x = 0;
    EXPECT_EQ(Invoke("authoring.placement.preview", args).errorCode, "SINGULAR_PARENT");
    parent.transform.scale.x = 1; child.transform.position.z = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(Invoke("authoring.placement.preview", args).errorCode, "INVALID_TRANSFORM");
    EXPECT_FALSE(m_undo.CanUndo());
}

TEST_F(PlacementOperatorTest, ChangedArgumentsOrTargetInvalidatePreview)
{
    auto& object = AttachedScene()->CreateGameObject("Rock");
    auto args = Request("align", {&object}); args.Set("value", 8.0f);
    auto preview = Invoke("authoring.placement.preview", args); ASSERT_TRUE(preview.ok);
    args.Set("expected", preview.data.Find("expected")->AsString());
    args.Set("value", 9.0f);
    EXPECT_EQ(Invoke("authoring.placement.apply", args).errorCode, "PLACEMENT_CONFLICT");
    args.Set("value", 8.0f); object.transform.position.y = 3;
    EXPECT_EQ(Invoke("authoring.placement.apply", args).errorCode, "PLACEMENT_CONFLICT");
    EXPECT_FLOAT_EQ(object.transform.position.x, 0); EXPECT_EQ(m_dirtyCalls, 0);
}

TEST_F(PlacementOperatorTest, UndoConflictLeavesEveryTargetAndHistoryCursorUnchanged)
{
    auto& first = AttachedScene()->CreateGameObject("First");
    auto& second = AttachedScene()->CreateGameObject("Second");
    auto args = Request("align", {&first, &second}); args.Set("value", 8.0f);
    ASSERT_TRUE(Apply(args).ok);
    first.transform.position.x = 10;
    const auto cursor = m_undo.GetCursor();
    EXPECT_FALSE(m_undo.Undo()); EXPECT_EQ(m_undo.GetCursor(), cursor);
    EXPECT_FLOAT_EQ(first.transform.position.x, 10); EXPECT_FLOAT_EQ(second.transform.position.x, 8);
}

TEST_F(PlacementOperatorTest, UndoRejectsParentChangeAndReplacementWithSameGuid)
{
    auto& parent = AttachedScene()->CreateGameObject("Parent");
    auto& object = AttachedScene()->CreateGameObject("Rock"); ASSERT_TRUE(object.SetParent(&parent));
    auto args = Request("align", {&object}); args.Set("value", 8.0f); ASSERT_TRUE(Apply(args).ok);
    parent.transform.position.y = 5;
    EXPECT_FALSE(m_undo.Undo());
    parent.transform.position.y = 0;
    const auto guid = object.instanceId;
    ASSERT_TRUE(AttachedScene()->DestroyGameObject(object.GetID()));
    auto& replacement = AttachedScene()->CreateGameObject("Replacement"); replacement.instanceId = guid;
    replacement.transform.position.x = 8;
    EXPECT_FALSE(m_undo.Undo()); EXPECT_FLOAT_EQ(replacement.transform.position.x, 8);
}

TEST_F(PlacementOperatorTest, NoOpDoesNotDirtyOrAddHistoryAndDisabledHistoryCannotApply)
{
    auto& object = AttachedScene()->CreateGameObject("Rock");
    auto args = Request("align", {&object}); args.Set("value", 0.0f);
    auto result = Apply(args); ASSERT_TRUE(result.ok); EXPECT_TRUE(result.noChange);
    EXPECT_EQ(m_dirtyCalls, 0); EXPECT_FALSE(m_undo.CanUndo());
    m_undo.SetRecordingEnabled(false); args.Set("value", 2.0f);
    EXPECT_EQ(Apply(args).errorCode, "NOT_AVAILABLE"); EXPECT_FLOAT_EQ(object.transform.position.x, 0);
}

TEST_F(PlacementOperatorTest, ContextCanBeInspectedWithoutSceneAndDuringPlay)
{
    editor::PlayModeController play;
    Context().playMode = &play;
    const auto generation = std::to_string(AttachedScene()->GetRenderSceneGeneration());
    auto result = Invoke("authoring.context"); ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.data.Find("projectRoot")->AsString(), Context().projectRoot);
    EXPECT_EQ(result.data.Find("sceneGeneration")->AsString(), generation);
    EXPECT_EQ(result.data.Find("mode")->AsString(), "edit");
    EXPECT_FALSE(Context().sceneDirty); EXPECT_EQ(m_dirtyCalls, 0);
    DetachScene();
    result = Invoke("authoring.context"); ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.data.Find("sceneGeneration")->IsNull());
    EXPECT_EQ(result.data.Find("nodeCount")->AsInt(), 0);
}

} /// @note namespace fbzz::tests
