/// @file    ControlPointEditingTests.cpp
/// @brief   TargetPoint 移動候補の確定・中断と Undo 契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>

#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Tools/ControlPointEditing.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/MotionWarpComponent.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace fbzz::tests {

namespace {

class ReferenceScript final : public scene::Script {
public:
    scene::ObjectReference target;
    std::string setting = "initial";

    void Reflect(scene::IReflector& reflector) override
    {
        reflector.ObjectReferenceField("target", target);
        reflector.Field("setting", setting);
    }

    const char* GetTypeName() const override { return "ControlPointReferenceScript"; }
};

} /// @note namespace

class ControlPointEditingTest : public testkit::EditorFixture {
protected:
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;

    void SetUp() override
    {
        EditorFixture::SetUp();
        (void)AttachScene();
        Context().operators = &m_registry;
        Context().undoStack = &m_undo;
        editor::RegisterTargetPointMoveOperator(m_registry);
    }

    void TearDown() override
    {
        m_undo.Clear();
        Context().operators = nullptr;
        Context().undoStack = nullptr;
        EditorFixture::TearDown();
    }

    scene::GameObject& MakePoint(const char* name, const math::Vector3& position,
                                 scene::GameObject* parent = nullptr)
    {
        auto& point = AttachedScene()->CreateGameObject(name);
        point.transform.position = position;
        if (parent) (void)point.SetParent(parent);
        point.AddComponent<scene::TargetPointComponent>();
        scene::FlushWorldTransforms(*AttachedScene());
        return point;
    }

    void Select(scene::GameObject& point)
    {
        Context().selectedEntities = {point.GetID()};
    }
};

TEST_F(ControlPointEditingTest, PreviewDoesNotMutateAndCommitCreatesOneUndoableMove)
{
    auto& point = MakePoint("Point", {1.0f, 2.0f, 3.0f});
    Select(point);
    editor::ControlPointMoveSession session;

    ASSERT_TRUE(session.Begin(Context()));
    ASSERT_TRUE(session.SetWorldDelta(Context(), {2.0f, -1.0f, 4.0f}));
    ASSERT_EQ(session.Preview().size(), 1u);
    EXPECT_VEC3_NEAR(point.transform.position, (math::Vector3{1.0f, 2.0f, 3.0f}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(session.Preview()[0].worldPosition,
                     (math::Vector3{3.0f, 1.0f, 7.0f}), testkit::kTolerance);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);

    ASSERT_TRUE(session.Commit(Context()));
    EXPECT_FALSE(session.Active());
    EXPECT_VEC3_NEAR(point.transform.position, (math::Vector3{3.0f, 1.0f, 7.0f}), testkit::kTolerance);
    ASSERT_EQ(m_undo.GetHistorySize(), 1u);
    EXPECT_EQ(m_undo.GetUndoDescription(), "Move Target Points");

    ASSERT_TRUE(m_undo.Undo());
    EXPECT_VEC3_NEAR(point.transform.position, (math::Vector3{1.0f, 2.0f, 3.0f}), testkit::kTolerance);
    ASSERT_TRUE(m_undo.Redo());
    EXPECT_VEC3_NEAR(point.transform.position, (math::Vector3{3.0f, 1.0f, 7.0f}), testkit::kTolerance);
}

TEST_F(ControlPointEditingTest, CancelAndNoOpCommitDoNotCreateHistory)
{
    auto& point = MakePoint("Point", {1.0f, 0.0f, 2.0f});
    Select(point);
    editor::ControlPointMoveSession session;

    ASSERT_TRUE(session.Begin(Context()));
    ASSERT_TRUE(session.SetWorldDelta(Context(), {4.0f, 0.0f, 0.0f}));
    session.Cancel();
    EXPECT_FALSE(session.Active());
    EXPECT_TRUE(session.Preview().empty());
    EXPECT_VEC3_NEAR(point.transform.position, (math::Vector3{1.0f, 0.0f, 2.0f}), testkit::kTolerance);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);

    ASSERT_TRUE(session.Begin(Context()));
    ASSERT_TRUE(session.SetWorldDelta(Context(), math::Vector3::ZERO));
    ASSERT_TRUE(session.Commit(Context()));
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    EXPECT_VEC3_NEAR(point.transform.position, (math::Vector3{1.0f, 0.0f, 2.0f}), testkit::kTolerance);
}

TEST_F(ControlPointEditingTest, WorldDeltaConvertsThroughSignedNonUniformParentScale)
{
    auto& parent = AttachedScene()->CreateGameObject("Parent");
    parent.transform.scale = {-2.0f, 3.0f, 0.5f};
    auto& point = MakePoint("Point", {1.0f, 2.0f, 3.0f}, &parent);
    Select(point);
    editor::ControlPointMoveSession session;

    ASSERT_TRUE(session.Begin(Context()));
    ASSERT_TRUE(session.SetWorldDelta(Context(), {2.0f, -3.0f, 0.5f}));
    EXPECT_VEC3_NEAR(point.transform.position, (math::Vector3{1.0f, 2.0f, 3.0f}), testkit::kTolerance);
    ASSERT_TRUE(session.Commit(Context()));
    EXPECT_VEC3_NEAR(point.transform.position, (math::Vector3{0.0f, 1.0f, 4.0f}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(point.transform.worldPosition, (math::Vector3{0.0f, 3.0f, 2.0f}), testkit::kTolerance);
}

TEST_F(ControlPointEditingTest, SmallDeltaSurvivesLargeWorldHeight)
{
    auto& parent = AttachedScene()->CreateGameObject("HighParent");
    parent.transform.position.y = 100000000.0f;
    auto& point = MakePoint("Point", {0.0f, 0.125f, 0.0f}, &parent);
    Select(point);
    editor::ControlPointMoveSession session;

    ASSERT_TRUE(session.Begin(Context()));
    ASSERT_TRUE(session.SetWorldDelta(Context(), {0.0f, 0.25f, 0.0f}));
    EXPECT_FLOAT_EQ(session.Preview()[0].worldDelta.y, 0.25f);
    EXPECT_FLOAT_EQ(point.transform.position.y, 0.125f);
    ASSERT_TRUE(session.Commit(Context()));
    EXPECT_FLOAT_EQ(point.transform.position.y, 0.375f);
    EXPECT_FLOAT_EQ(point.transform.worldPosition.y, 100000000.0f);
}

TEST_F(ControlPointEditingTest, ParentChangeAbortsCandidate)
{
    auto& parent = AttachedScene()->CreateGameObject("Parent");
    auto& point = MakePoint("Point", {1.0f, 0.0f, 0.0f}, &parent);
    Select(point);
    editor::ControlPointMoveSession session;

    ASSERT_TRUE(session.Begin(Context()));
    parent.transform.position.x = 5.0f;
    EXPECT_FALSE(session.SetWorldDelta(Context(), {1.0f, 0.0f, 0.0f}));
    EXPECT_FALSE(session.Active());
    EXPECT_TRUE(session.Preview().empty());
    EXPECT_FLOAT_EQ(point.transform.position.x, 1.0f);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(ControlPointEditingTest, SceneAndUndoRevisionChangesAbortCandidate)
{
    auto& point = MakePoint("Point", {0.0f, 0.0f, 0.0f});
    Select(point);
    editor::ControlPointMoveSession session;
    ASSERT_TRUE(session.Begin(Context()));

    scene::Scene replacementScene;
    scene::Scene* originalScene = Context().activeScene;
    Context().activeScene = &replacementScene;
    EXPECT_FALSE(session.SetWorldDelta(Context(), {1.0f, 0.0f, 0.0f}));
    EXPECT_FALSE(session.Active());
    Context().activeScene = originalScene;

    ASSERT_TRUE(session.Begin(Context()));
    ASSERT_TRUE(m_undo.Execute(std::make_unique<editor::LambdaCommand>("Unrelated edit", [] {}, [] {})));
    EXPECT_FALSE(session.SetWorldDelta(Context(), {1.0f, 0.0f, 0.0f}));
    EXPECT_FALSE(session.Active());
    EXPECT_FLOAT_EQ(point.transform.position.x, 0.0f);
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
}

TEST_F(ControlPointEditingTest, MarkerLifetimeAndSharedOwnerBindingChangesAbortCandidate)
{
    auto& target = MakePoint("Target", {0.0f, 0.0f, 0.0f});
    auto& replacement = MakePoint("Replacement", {2.0f, 0.0f, 0.0f});
    auto& owner = AttachedScene()->CreateGameObject("SplineOwner");
    scene::SplineComponent spline;
    spline.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    scene::SplineControlPointBinding binding;
    binding.id = 17;
    binding.target.guid = target.instanceId;
    spline.pointBindings.push_back(binding);
    owner.AddComponent<scene::SplineComponent>(std::move(spline));
    scene::FlushWorldTransforms(*AttachedScene());
    Select(target);
    editor::ControlPointMoveSession session;

    ASSERT_TRUE(session.Begin(Context()));
    owner.GetComponent<scene::SplineComponent>()->pointBindings[0].target.guid = replacement.instanceId;
    EXPECT_FALSE(session.SetWorldDelta(Context(), {1.0f, 0.0f, 0.0f}));
    EXPECT_FALSE(session.Active());
    EXPECT_FLOAT_EQ(target.transform.position.x, 0.0f);

    auto* splineAgain = owner.GetComponent<scene::SplineComponent>();
    ASSERT_NE(splineAgain, nullptr);
    splineAgain->pointBindings[0].target.guid = target.instanceId;
    splineAgain->pointBindings[0].target.id = scene::EntityID::INVALID;
    splineAgain->pointBindings[0].target.sceneGeneration = 0;
    ASSERT_TRUE(session.Begin(Context()));
    target.RemoveComponent<scene::TargetPointComponent>();
    target.AddComponent<scene::TargetPointComponent>();
    EXPECT_FALSE(session.SetWorldDelta(Context(), {1.0f, 0.0f, 0.0f}));
    EXPECT_FALSE(session.Active());
    EXPECT_FLOAT_EQ(target.transform.position.x, 0.0f);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(ControlPointEditingTest, MotionWarpReferenceToSelectedDescendantAbortsOnSamePoseGuidSwap)
{
    auto& selectedRoot = MakePoint("SelectedRoot", {0.0f, 0.0f, 0.0f});
    auto& child = MakePoint("ChildTarget", {1.0f, 0.0f, 0.0f}, &selectedRoot);
    auto& replacement = MakePoint("Replacement", {1.0f, 0.0f, 0.0f});
    auto& owner = AttachedScene()->CreateGameObject("MotionWarpOwner");
    scene::MotionWarpComponent warp;
    warp.targetReference.guid = child.instanceId;
    owner.AddComponent<scene::MotionWarpComponent>(std::move(warp));
    scene::FlushWorldTransforms(*AttachedScene());
    Select(selectedRoot);

    editor::ControlPointMoveSession session;
    ASSERT_TRUE(session.Begin(Context()));
    owner.GetComponent<scene::MotionWarpComponent>()->targetPointMode = scene::MotionWarpPointMode::LIVE;
    EXPECT_FALSE(session.SetWorldDelta(Context(), {0.0f, 1.0f, 0.0f}));
    EXPECT_FALSE(session.Active());

    owner.GetComponent<scene::MotionWarpComponent>()->targetPointMode = scene::MotionWarpPointMode::CAPTURED;
    ASSERT_TRUE(session.Begin(Context()));
    ASSERT_TRUE(session.SetWorldDelta(Context(), {0.0f, 2.0f, 0.0f}));
    const auto childPreview = std::find_if(session.Preview().begin(), session.Preview().end(), [&child](const auto& preview) {
        return preview.entity == child.GetID();
    });
    ASSERT_NE(childPreview, session.Preview().end());
    EXPECT_VEC3_NEAR(childPreview->worldPosition, (math::Vector3{1.0f, 2.0f, 0.0f}), testkit::kTolerance);

    owner.GetComponent<scene::MotionWarpComponent>()->targetReference.guid = replacement.instanceId;
    EXPECT_FALSE(session.SetWorldDelta(Context(), {0.0f, 3.0f, 0.0f}));
    EXPECT_FALSE(session.Active());
    EXPECT_VEC3_NEAR(selectedRoot.transform.position, math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);

    owner.GetComponent<scene::MotionWarpComponent>()->targetReference.guid = child.instanceId;
    ASSERT_TRUE(session.Begin(Context()));
    owner.RemoveComponent<scene::MotionWarpComponent>();
    scene::MotionWarpComponent replacementWarp;
    replacementWarp.targetReference.guid = child.instanceId;
    owner.AddComponent<scene::MotionWarpComponent>(std::move(replacementWarp));
    EXPECT_FALSE(session.SetWorldDelta(Context(), {0.0f, 1.0f, 0.0f}));
    EXPECT_FALSE(session.Active());
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(ControlPointEditingTest, ScriptObjectReferenceChangeAbortsCandidate)
{
    auto& target = MakePoint("ScriptTarget", {0.0f, 0.0f, 0.0f});
    auto& replacement = MakePoint("ScriptReplacement", {0.0f, 0.0f, 0.0f});
    auto& owner = AttachedScene()->CreateGameObject("ScriptOwner");
    auto script = std::make_unique<ReferenceScript>();
    auto* scriptPtr = script.get();
    scriptPtr->target.guid = target.instanceId;
    scriptPtr->SetContext(AttachedScene(), &owner);
    scene::ScriptEntry entry;
    entry.script = std::move(script);
    scene::ScriptComponent component;
    component.scripts.push_back(std::move(entry));
    owner.AddComponent<scene::ScriptComponent>(std::move(component));
    Select(target);

    editor::ControlPointMoveSession session;
    ASSERT_TRUE(session.Begin(Context()));
    scriptPtr->target.guid = replacement.instanceId;
    EXPECT_FALSE(session.SetWorldDelta(Context(), {1.0f, 0.0f, 0.0f}));
    EXPECT_FALSE(session.Active());
    EXPECT_VEC3_NEAR(target.transform.position, math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(ControlPointEditingTest, RejectsNonUnitQuaternion)
{
    auto& point = MakePoint("Point", {0.0f, 0.0f, 0.0f});
    point.transform.rotation = {0.0f, 0.0f, 0.0f, 2.0f};
    scene::FlushWorldTransforms(*AttachedScene());
    Select(point);
    editor::ControlPointMoveSession session;

    EXPECT_FALSE(session.Begin(Context()));
    EXPECT_FALSE(session.Active());
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(ControlPointEditingTest, InspectConnectionsFindsReferencesButDoesNotTreatChildrenAsConnected)
{
    auto& owner = AttachedScene()->CreateGameObject("Spline Owner");
    const auto ownerId = owner.GetID();
    owner.AddComponent<scene::SplineComponent>();
    const auto pointId = MakePoint("Connected Point", {1.0f, 2.0f, 3.0f}, &owner).GetID();
    const auto pointGuid = AttachedScene()->GetGameObject(pointId)->instanceId;
    auto& spline = *AttachedScene()->GetGameObject(ownerId)->GetComponent<scene::SplineComponent>();
    spline.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    spline.pointBindings.push_back({1, {pointGuid}});
    auto& scriptOwner = AttachedScene()->CreateGameObject("Script Owner");
    const auto scriptOwnerGuid = scriptOwner.instanceId;
    auto& scripts = scriptOwner.AddComponent<scene::ScriptComponent>();
    scene::ScriptEntry entry;
    auto script = std::make_unique<ReferenceScript>();
    script->target.guid = pointGuid;
    script->SetContext(AttachedScene(), &scriptOwner);
    entry.script = std::move(script);
    scripts.scripts.push_back(std::move(entry));
    const auto loosePointId = MakePoint("Unconnected Child", {}, AttachedScene()->GetGameObject(ownerId)).GetID();

    editor::OpArgs args; args.Set("target", pointGuid);
    const auto result = editor::InvokeOperator(Context(), "target_point.inspect_connections", args);
    ASSERT_TRUE(result.ok);
    const auto* owners = result.data.Find("owners");
    ASSERT_NE(owners, nullptr);
    ASSERT_TRUE(owners->IsArray());
    ASSERT_EQ(owners->AsArray().size(), 2u);
    const auto contains = [&](const std::string& guid) {
        return std::any_of(owners->AsArray().begin(), owners->AsArray().end(), [&](const auto& item) {
            const auto* value = item.Find("guid"); return value && value->AsString() == guid;
        });
    };
    EXPECT_TRUE(contains(AttachedScene()->GetGameObject(ownerId)->instanceId));
    EXPECT_TRUE(contains(scriptOwnerGuid));
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    EXPECT_VEC3_NEAR(AttachedScene()->GetGameObject(pointId)->transform.position,
        (math::Vector3{1.0f, 2.0f, 3.0f}), testkit::kTolerance);

    args.Set("target", AttachedScene()->GetGameObject(loosePointId)->instanceId);
    const auto looseResult = editor::InvokeOperator(Context(), "target_point.inspect_connections", args);
    ASSERT_TRUE(looseResult.ok);
    ASSERT_NE(looseResult.data.Find("owners"), nullptr);
    EXPECT_TRUE(looseResult.data.Find("owners")->AsArray().empty());
}

TEST_F(ControlPointEditingTest, InspectConnectionsUsesCanonicalGuidRatherThanStaleEntityCache)
{
    const auto pointId = MakePoint("Point", {}).GetID();
    const auto pointGuid = AttachedScene()->GetGameObject(pointId)->instanceId;
    auto& owner = AttachedScene()->CreateGameObject("Warp Owner");
    auto& warp = owner.AddComponent<scene::MotionWarpComponent>();
    warp.targetReference.guid = "missing-canonical-target";
    warp.targetReference.id = pointId;
    warp.targetReference.sceneGeneration = AttachedScene()->GetRenderSceneGeneration();
    editor::OpArgs args; args.Set("target", pointGuid);
    const auto result = editor::InvokeOperator(Context(), "target_point.inspect_connections", args);
    ASSERT_TRUE(result.ok);
    ASSERT_NE(result.data.Find("owners"), nullptr);
    EXPECT_TRUE(result.data.Find("owners")->AsArray().empty());
    EXPECT_EQ(warp.targetReference.guid, "missing-canonical-target");
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);

    args.Set("target", owner.instanceId);
    EXPECT_FALSE(editor::InvokeOperator(Context(), "target_point.inspect_connections", args).ok);
    args.Set("target", std::string{"missing-point"});
    EXPECT_FALSE(editor::InvokeOperator(Context(), "target_point.inspect_connections", args).ok);
}

} /// @note namespace fbzz::tests
