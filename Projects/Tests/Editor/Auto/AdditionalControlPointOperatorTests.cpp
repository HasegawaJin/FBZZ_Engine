/// @file    AdditionalControlPointOperatorTests.cpp
/// @brief   Patrol、Line、Off-Mesh Linkの追加制御点操作とChecked Undoを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Tools/AdditionalControlPointEditing.hpp>
#include <Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/NavigationControlPoints.hpp>
#include <Engine/Scene/PresentationControlPoints.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <string>

namespace fbzz::tests {

class AdditionalControlPointOperatorTest : public testkit::EditorFixture {
protected:
    editor::UndoStack m_undo;

    void SetUp() override
    {
        EditorFixture::SetUp();
        AttachScene();
        Context().undoStack = &m_undo;
    }

    void TearDown() override
    {
        m_undo.Clear();
        EditorFixture::TearDown();
    }

    scene::Scene& World() { return *AttachedScene(); }

    editor::OpArgs OwnerArgs(scene::EntityID owner, const char* kind)
    {
        editor::OpArgs args;
        args.Set("node", World().GetGameObject(owner)->instanceId);
        args.Set("kind", std::string(kind));
        return args;
    }

    editor::OpResult Invoke(const char* id, const editor::OpArgs& args)
    {
        editor::OpContext context{Context(), m_undo};
        auto result = editor::InvokeAdditionalControlPointOperation(context, id, args);
        if (result.ok && result.command) EXPECT_TRUE(m_undo.Push(std::move(result.command)));
        return result;
    }
};

TEST_F(AdditionalControlPointOperatorTest, PatrolConversionUsesWorldPositionsAndKeepsWaypointAttributes)
{
    auto& owner = World().CreateGameObject("Patrol");
    owner.transform.position = {120.0f, 8.0f, -35.0f};
    owner.transform.rotation = math::Quaternion::FromEuler({0.2f, 0.7f, -0.1f});
    owner.transform.scale = {-2.0f, 1.5f, 0.75f};
    auto& patrol = owner.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.waitTime = 0.25f;
    patrol.waypoints = {{110.0f, 11.0f, -32.0f}, {121.0f, 7.0f, -41.0f}, {135.0f, 9.0f, -27.0f}};
    patrol.waypointWaitTimes = {0.5f, 1.25f, 0.0f};
    patrol.waypointSpeeds = {2.0f, 4.0f, 3.0f};
    scene::FlushWorldTransforms(World());
    const auto original = scene::ResolveNavMeshPatrolPointAuthoring(World(), owner.GetID(), patrol,
        scene::ControlPointRuntimeMode::LIVE);
    ASSERT_TRUE(original.valid);
    const size_t beforeCount = World().GameObjectCount();
    auto args = OwnerArgs(owner.GetID(), "patrol"); args.Set("source", std::string("POINT_OBJECTS"));

    const auto converted = Invoke("control_points.convert", args);

    ASSERT_TRUE(converted.ok) << converted.message;
    EXPECT_EQ(World().GameObjectCount(), beforeCount + original.points.size() + 1);
    EXPECT_EQ(patrol.pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    EXPECT_TRUE(patrol.waypoints.empty());
    const auto resolved = scene::ResolveNavMeshPatrolPointAuthoring(World(), owner.GetID(), patrol,
        scene::ControlPointRuntimeMode::LIVE);
    ASSERT_TRUE(resolved.valid);
    ASSERT_EQ(resolved.points.size(), original.points.size());
    for (std::size_t index = 0; index < resolved.points.size(); ++index) {
        EXPECT_EQ(resolved.points[index].stableId, original.points[index].stableId);
        EXPECT_VEC3_NEAR(resolved.points[index].worldPosition, original.points[index].worldPosition, testkit::kTolerance);
        EXPECT_FLOAT_EQ(resolved.points[index].waitTime, original.points[index].waitTime);
        EXPECT_FLOAT_EQ(resolved.points[index].speed, original.points[index].speed);
        auto* target = World().FindByGuid(patrol.waypointBindings[index].target.guid);
        ASSERT_NE(target, nullptr);
        EXPECT_NE(target->GetComponent<scene::TargetPointComponent>(), nullptr);
        ASSERT_NE(target->GetParent(), nullptr);
        EXPECT_EQ(target->GetParent()->GetParent(), nullptr);
    }

    const math::Vector3 ownerPosition = owner.transform.position;
    owner.transform.position += math::Vector3{17.0f, -4.0f, 9.0f};
    scene::FlushWorldTransforms(World());
    const auto afterOwnerMove = scene::ResolveNavMeshPatrolPointAuthoring(World(), owner.GetID(), patrol,
        scene::ControlPointRuntimeMode::LIVE);
    ASSERT_TRUE(afterOwnerMove.valid);
    for (std::size_t index = 0; index < afterOwnerMove.points.size(); ++index)
        EXPECT_VEC3_NEAR(afterOwnerMove.points[index].worldPosition, original.points[index].worldPosition, testkit::kTolerance);
    owner.transform.position = ownerPosition;
    scene::FlushWorldTransforms(World());

    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(World().GameObjectCount(), beforeCount);
    EXPECT_EQ(patrol.pointSourceMode, scene::ControlPointSourceMode::INLINE);
    EXPECT_EQ(patrol.waypoints, (std::vector<math::Vector3>{{110.0f, 11.0f, -32.0f},
        {121.0f, 7.0f, -41.0f}, {135.0f, 9.0f, -27.0f}}));
    ASSERT_TRUE(m_undo.Redo());
    EXPECT_EQ(patrol.pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    EXPECT_EQ(patrol.waypointBindings.size(), original.points.size());
}

TEST_F(AdditionalControlPointOperatorTest, PatrolAttributeEditAndRemovalAreUndoableWithoutDeletingTargets)
{
    auto& owner = World().CreateGameObject("Patrol");
    auto& patrol = owner.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.waypoints = {{1.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}};
    auto convert = OwnerArgs(owner.GetID(), "patrol"); convert.Set("source", std::string("POINT_OBJECTS"));
    ASSERT_TRUE(Invoke("control_points.convert", convert).ok);
    m_undo.Clear();
    const std::string targetGuid = patrol.waypointBindings.front().target.guid;
    const auto id = patrol.waypointBindings.front().id;
    auto attributes = OwnerArgs(owner.GetID(), "patrol");
    attributes.Set("id", static_cast<int>(id)); attributes.Set("waitTime", 3.0f); attributes.Set("speed", 6.0f);

    ASSERT_TRUE(Invoke("control_points.set_patrol_attributes", attributes).ok);

    EXPECT_FLOAT_EQ(patrol.waypointBindings.front().waitTime, 3.0f);
    EXPECT_FLOAT_EQ(patrol.waypointBindings.front().speed, 6.0f);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_FLOAT_EQ(patrol.waypointBindings.front().waitTime, patrol.waitTime);
    EXPECT_FLOAT_EQ(patrol.waypointBindings.front().speed, 0.0f);
    auto remove = OwnerArgs(owner.GetID(), "patrol"); remove.Set("id", static_cast<int>(id));
    ASSERT_TRUE(Invoke("control_points.remove", remove).ok);
    EXPECT_EQ(patrol.waypointBindings.size(), 1u);
    EXPECT_NE(World().FindByGuid(targetGuid), nullptr);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(patrol.waypointBindings.size(), 2u);
    EXPECT_NE(World().FindByGuid(targetGuid), nullptr);
}

TEST_F(AdditionalControlPointOperatorTest, PatrolAddUsesWorldPositionAndDoesNotFollowOwner)
{
    auto& owner = World().CreateGameObject("Moving Patrol");
    owner.transform.position = {15.0f, 2.0f, -7.0f};
    owner.transform.rotation = math::Quaternion::FromEuler({0.2f, 0.6f, -0.1f});
    owner.transform.scale = {-2.0f, 1.5f, 0.75f};
    auto& patrol = owner.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    scene::FlushWorldTransforms(World());
    const math::Vector3 expectedWorld = owner.transform.worldPosition
        + owner.transform.worldRotation * math::Vector3{4.0f, 3.0f, -1.5f};
    auto add = OwnerArgs(owner.GetID(), "patrol");
    add.Set("worldPosition", expectedWorld); add.Set("waitTime", 0.25f); add.Set("speed", 2.0f);

    const auto added = Invoke("control_points.add", add);

    ASSERT_TRUE(added.ok) << added.message;
    const auto first = scene::ResolveNavMeshPatrolPointAuthoring(World(), owner.GetID(), patrol,
        scene::ControlPointRuntimeMode::LIVE);
    ASSERT_TRUE(first.valid); ASSERT_EQ(first.points.size(), 1u);
    EXPECT_VEC3_NEAR(first.points[0].worldPosition, expectedWorld, testkit::kTolerance);
    auto* firstTarget = World().FindByGuid(patrol.waypointBindings[0].target.guid);
    ASSERT_NE(firstTarget, nullptr);
    auto* container = firstTarget->GetParent();
    ASSERT_NE(container, nullptr);
    const size_t countBeforeSecondAdd = World().GameObjectCount();
    const math::Vector3 localInput{1.0f, 2.0f, 3.0f};
    const math::Vector3 secondWorld = owner.transform.worldPosition
        + owner.transform.worldRotation * math::Vector3{
            localInput.x * owner.transform.worldScale.x,
            localInput.y * owner.transform.worldScale.y,
            localInput.z * owner.transform.worldScale.z};
    auto addSecond = OwnerArgs(owner.GetID(), "patrol");
    addSecond.Set("localPosition", localInput);
    addSecond.Set("waitTime", 0.0f); addSecond.Set("speed", 1.0f);
    ASSERT_TRUE(Invoke("control_points.add", addSecond).ok);
    EXPECT_EQ(World().GameObjectCount(), countBeforeSecondAdd + 1);
    auto* secondTarget = World().FindByGuid(patrol.waypointBindings[1].target.guid);
    ASSERT_NE(secondTarget, nullptr);
    EXPECT_EQ(secondTarget->GetParent(), container);
    owner.transform.position += math::Vector3{5.0f, 1.0f, -3.0f};
    scene::FlushWorldTransforms(World());
    const auto moved = scene::ResolveNavMeshPatrolPointAuthoring(World(), owner.GetID(), patrol,
        scene::ControlPointRuntimeMode::LIVE);
    ASSERT_TRUE(moved.valid);
    ASSERT_EQ(moved.points.size(), 2u);
    EXPECT_VEC3_NEAR(moved.points[0].worldPosition, expectedWorld, testkit::kTolerance);
    EXPECT_VEC3_NEAR(moved.points[1].worldPosition, secondWorld, testkit::kTolerance);
}

TEST_F(AdditionalControlPointOperatorTest, LocalLineConversionKeepsMarkersUnderTheOwner)
{
    auto& owner = World().CreateGameObject("Local Line");
    owner.transform.position = {5.0f, 3.0f, -2.0f};
    owner.transform.rotation = math::Quaternion::FromEuler({0.1f, 0.4f, 0.0f});
    auto& line = owner.AddComponent<scene::LineRendererComponent>();
    line.space = scene::LineSpace::Local;
    line.points = {{-1.0f, 0.0f, 2.0f}, {3.0f, 1.0f, -4.0f}};
    const auto before = scene::ResolveLineControlPointAuthoring(World(), owner.GetID(), line,
        scene::ControlPointRuntimeMode::LIVE);
    ASSERT_TRUE(before.valid);
    auto convert = OwnerArgs(owner.GetID(), "line");
    convert.Set("source", std::string("POINT_OBJECTS"));

    ASSERT_TRUE(Invoke("control_points.convert", convert).ok);

    auto* firstPoint = World().FindByGuid(line.pointBindings.front().target.guid);
    ASSERT_NE(firstPoint, nullptr);
    auto* container = firstPoint->GetParent();
    ASSERT_NE(container, nullptr);
    EXPECT_EQ(container->GetParent(), &owner);
    scene::FlushWorldTransforms(World());
    const auto after = scene::ResolveLineControlPointAuthoring(World(), owner.GetID(), line,
        scene::ControlPointRuntimeMode::LIVE);
    ASSERT_TRUE(after.valid);
    EXPECT_EQ(after.points, before.points);
    owner.transform.position += math::Vector3{2.0f, -1.0f, 6.0f};
    scene::FlushWorldTransforms(World());
    const auto afterMove = scene::ResolveLineControlPointAuthoring(World(), owner.GetID(), line,
        scene::ControlPointRuntimeMode::LIVE);
    ASSERT_TRUE(afterMove.valid);
    EXPECT_EQ(afterMove.points, before.points);
}

TEST_F(AdditionalControlPointOperatorTest, LineWorldConversionAndStaticBakeKeepTheirCapturedSpace)
{
    auto& owner = World().CreateGameObject("Line");
    owner.transform.position = {7.0f, 3.0f, -4.0f};
    owner.transform.rotation = math::Quaternion::FromEuler({0.0f, 0.4f, 0.0f});
    owner.transform.scale = {-2.0f, 3.0f, 0.5f};
    auto& line = owner.AddComponent<scene::LineRendererComponent>();
    line.space = scene::LineSpace::World;
    line.points = {{5.0f, 4.0f, -2.0f}, {12.0f, 2.0f, 8.0f}, {19.0f, 9.0f, 3.0f}};
    scene::FlushWorldTransforms(World());
    const auto before = scene::ResolveLineControlPointAuthoring(World(), owner.GetID(), line,
        scene::ControlPointRuntimeMode::LIVE);
    ASSERT_TRUE(before.valid);
    auto convert = OwnerArgs(owner.GetID(), "line"); convert.Set("source", std::string("POINT_OBJECTS"));

    ASSERT_TRUE(Invoke("control_points.convert", convert).ok);

    const auto resolved = scene::ResolveLineControlPointAuthoring(World(), owner.GetID(), line,
        scene::ControlPointRuntimeMode::LIVE);
    ASSERT_TRUE(resolved.valid); ASSERT_EQ(resolved.points.size(), before.points.size());
    for (std::size_t index = 0; index < before.points.size(); ++index)
        EXPECT_VEC3_NEAR(resolved.points[index], before.points[index], testkit::kTolerance);
    const math::Vector3 ownerPosition = owner.transform.position;
    owner.transform.position += math::Vector3{3.0f, 2.0f, 4.0f};
    scene::FlushWorldTransforms(World());
    const auto afterOwnerMove = scene::ResolveLineControlPointAuthoring(World(), owner.GetID(), line,
        scene::ControlPointRuntimeMode::LIVE);
    ASSERT_TRUE(afterOwnerMove.valid);
    EXPECT_EQ(afterOwnerMove.points, before.points);
    owner.transform.position = ownerPosition;
    scene::FlushWorldTransforms(World());
    auto bake = OwnerArgs(owner.GetID(), "line");
    ASSERT_TRUE(Invoke("control_points.bake", bake).ok);
    EXPECT_EQ(line.pointRuntimeMode, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_EQ(line.bakedSpace, scene::LineSpace::World);
    auto* target = World().FindByGuid(line.pointBindings[1].target.guid);
    ASSERT_NE(target, nullptr);
    target->transform.position += math::Vector3{1.0f, 2.0f, 3.0f};
    scene::FlushWorldTransforms(World());
    const auto runtime = scene::ResolveLineControlPoints(World(), owner.GetID(), line,
        scene::ControlPointReadContext::RUNTIME);
    ASSERT_TRUE(runtime.valid); EXPECT_EQ(runtime.space, scene::LineSpace::World);
    EXPECT_VEC3_NEAR(runtime.points[1], line.bakedPoints[1].position, testkit::kTolerance);
    const auto authoring = scene::ResolveLineControlPoints(World(), owner.GetID(), line,
        scene::ControlPointReadContext::AUTHORING);
    ASSERT_TRUE(authoring.valid); EXPECT_TRUE(authoring.bakeStale);
}

TEST_F(AdditionalControlPointOperatorTest, OffMeshLinkConvertsBothWorldEndpointsAndRetainsSharedTargetsOnUndoConflict)
{
    auto& owner = World().CreateGameObject("Off-Mesh Link");
    owner.transform.position = {25.0f, 5.0f, -2.0f};
    owner.transform.rotation = math::Quaternion::FromEuler({0.15f, -0.3f, 0.2f});
    owner.transform.scale = {-1.5f, 2.0f, 0.8f};
    auto& link = owner.AddComponent<scene::NavMeshOffMeshLinkComponent>();
    link.startPoint = {21.0f, 3.0f, -1.0f}; link.endPoint = {31.0f, 8.0f, 7.0f};
    scene::FlushWorldTransforms(World());
    auto convert = OwnerArgs(owner.GetID(), "nav_link"); convert.Set("source", std::string("POINT_OBJECTS"));

    const auto converted = Invoke("control_points.convert", convert);

    ASSERT_TRUE(converted.ok) << converted.message;
    const auto resolved = scene::ResolveNavMeshOffMeshLinkEndpoints(World(), owner.GetID(), link);
    ASSERT_TRUE(resolved.valid);
    EXPECT_VEC3_NEAR(resolved.startPoint, link.startPoint, testkit::kTolerance);
    EXPECT_VEC3_NEAR(resolved.endPoint, link.endPoint, testkit::kTolerance);
    const math::Vector3 ownerPosition = owner.transform.position;
    owner.transform.position += math::Vector3{-8.0f, 3.0f, 2.0f};
    scene::FlushWorldTransforms(World());
    const auto afterOwnerMove = scene::ResolveNavMeshOffMeshLinkEndpoints(World(), owner.GetID(), link);
    ASSERT_TRUE(afterOwnerMove.valid);
    EXPECT_VEC3_NEAR(afterOwnerMove.startPoint, link.startPoint, testkit::kTolerance);
    EXPECT_VEC3_NEAR(afterOwnerMove.endPoint, link.endPoint, testkit::kTolerance);
    owner.transform.position = ownerPosition;
    scene::FlushWorldTransforms(World());
    auto* shared = World().FindByGuid(link.startTarget.guid);
    ASSERT_NE(shared, nullptr);
    auto& other = World().CreateGameObject("Other Consumer");
    auto& patrol = other.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    scene::NavMeshPatrolWaypointBinding row;
    row.id = 1; row.target.guid = shared->instanceId;
    patrol.waypointBindings.push_back(row);
    const size_t cursor = m_undo.GetCursor();

    EXPECT_FALSE(m_undo.Undo());

    EXPECT_EQ(m_undo.GetCursor(), cursor);
    EXPECT_NE(World().FindByGuid(shared->instanceId), nullptr);
    EXPECT_EQ(link.pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
}

TEST_F(AdditionalControlPointOperatorTest, StructuralEditsRejectPrefabInstancesAndCapacityFailureCreatesNothing)
{
    auto& prefabOwner = World().CreateGameObject("Prefab Line");
    prefabOwner.prefabAssetPath = "Assets/Prefabs/Line.fzp";
    auto& prefabLine = prefabOwner.AddComponent<scene::LineRendererComponent>();
    prefabLine.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    const size_t initialCount = World().GameObjectCount();
    auto convert = OwnerArgs(prefabOwner.GetID(), "line");
    convert.Set("source", std::string("INLINE"));

    const auto rejected = Invoke("control_points.convert", convert);

    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.errorCode, "PREFAB_STRUCTURE");
    EXPECT_EQ(prefabLine.pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    EXPECT_EQ(World().GameObjectCount(), initialCount);

    auto& lineOwner = World().CreateGameObject("Full Line");
    auto& line = lineOwner.AddComponent<scene::LineRendererComponent>();
    line.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    for (std::uint32_t id = 1; id <= scene::MAX_CONTROL_POINT_BINDINGS; ++id)
        line.pointBindings.push_back({id, {}});
    const size_t fullCount = World().GameObjectCount();
    auto add = OwnerArgs(lineOwner.GetID(), "line");
    add.Set("localPosition", math::Vector3{4.0f, 1.0f, -2.0f});

    const auto full = Invoke("control_points.add", add);

    EXPECT_FALSE(full.ok);
    EXPECT_EQ(full.errorCode, "LIMIT");
    EXPECT_EQ(line.pointBindings.size(), scene::MAX_CONTROL_POINT_BINDINGS);
    EXPECT_EQ(World().GameObjectCount(), fullCount);
}

TEST_F(AdditionalControlPointOperatorTest, ExistingTargetPoseChangesConflictWithBindingUndo)
{
    auto& owner = World().CreateGameObject("Off-Mesh Link");
    auto& link = owner.AddComponent<scene::NavMeshOffMeshLinkComponent>();
    link.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    auto& target = World().CreateGameObject("Start Marker");
    target.AddComponent<scene::TargetPointComponent>();
    const std::string targetGuid = target.instanceId;
    auto bind = OwnerArgs(owner.GetID(), "nav_link");
    bind.Set("id", 1);
    bind.Set("target", targetGuid);

    ASSERT_TRUE(Invoke("control_points.bind", bind).ok);

    const size_t cursor = m_undo.GetCursor();
    target.transform.position = {3.0f, 2.0f, 1.0f};
    scene::FlushWorldTransforms(World());
    EXPECT_FALSE(m_undo.Undo());
    EXPECT_EQ(m_undo.GetCursor(), cursor);
    EXPECT_EQ(link.startTarget.guid, targetGuid);
}

TEST_F(AdditionalControlPointOperatorTest, PatrolBindingRejectsOwnerDescendantTargets)
{
    auto& owner = World().CreateGameObject("Patrol");
    auto& patrol = owner.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    scene::NavMeshPatrolWaypointBinding row;
    row.id = 1;
    patrol.waypointBindings.push_back(row);
    auto& child = World().CreateGameObject("Self Target");
    ASSERT_TRUE(child.SetParent(&owner));
    child.AddComponent<scene::TargetPointComponent>();
    auto bind = OwnerArgs(owner.GetID(), "patrol");
    bind.Set("id", 1);
    bind.Set("target", child.instanceId);

    const auto rejected = Invoke("control_points.bind", bind);

    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(patrol.waypointBindings.front().target.guid, "");
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(AdditionalControlPointOperatorTest, NavLinkAddCreatesOneAbsoluteEndpointAndUnbindKeepsItsMarker)
{
    auto& owner = World().CreateGameObject("Off-Mesh Link");
    owner.transform.position = {9.0f, 4.0f, -6.0f};
    owner.transform.rotation = math::Quaternion::FromEuler({0.1f, -0.4f, 0.2f});
    owner.transform.scale = {-1.5f, 2.0f, 0.75f};
    auto& link = owner.AddComponent<scene::NavMeshOffMeshLinkComponent>();
    link.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    scene::FlushWorldTransforms(World());
    const math::Vector3 worldPosition{20.0f, 7.0f, 3.0f};
    auto add = OwnerArgs(owner.GetID(), "nav_link");
    add.Set("id", 1); add.Set("worldPosition", worldPosition);

    ASSERT_TRUE(Invoke("control_points.add", add).ok);

    ASSERT_FALSE(link.startTarget.guid.empty());
    EXPECT_TRUE(link.endTarget.guid.empty());
    auto* target = World().FindByGuid(link.startTarget.guid);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(target->GetParent(), nullptr);
    EXPECT_EQ(target->GetParent()->GetParent(), nullptr);
    EXPECT_VEC3_NEAR(target->transform.worldPosition, worldPosition, testkit::kTolerance);
    owner.transform.position += math::Vector3{2.0f, 5.0f, -1.0f};
    scene::FlushWorldTransforms(World());
    EXPECT_VEC3_NEAR(target->transform.worldPosition, worldPosition, testkit::kTolerance);

    const std::string targetGuid = target->instanceId;
    auto unbind = OwnerArgs(owner.GetID(), "nav_link"); unbind.Set("id", 1);
    ASSERT_TRUE(Invoke("control_points.unbind", unbind).ok);
    EXPECT_TRUE(link.startTarget.guid.empty());
    EXPECT_NE(World().FindByGuid(targetGuid), nullptr);
}

} /// @note namespace fbzz::tests
