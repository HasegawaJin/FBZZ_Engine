/// @file    ControlPointOperatorTests.cpp
/// @brief   共通点authoringを実OperatorとChecked Undoの境界で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Tools/ControlPointEditing.hpp>
#include <Engine/Scene/Components/ClothComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/ControlPointAdapters.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Systems/ClothSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Physics/World.hpp>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::tests {

class ControlPointOperatorTest : public testkit::EditorFixture {
protected:
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    int m_dirtyCount = 0;

    void SetUp() override
    {
        EditorFixture::SetUp();
        AttachScene();
        Context().operators = &m_registry; Context().undoStack = &m_undo;
        Context().markSceneDirty = [this] { ++m_dirtyCount; };
        editor::RegisterControlPointOperators(m_registry);
        editor::RegisterTargetPointMoveOperator(m_registry);
    }

    void TearDown() override
    {
        m_undo.Clear(); Context().markSceneDirty = {};
        EditorFixture::TearDown();
    }

    scene::Scene& World() { return *AttachedScene(); }
    scene::WaterComponent& Water(scene::EntityID id) { return *World().GetComponent<scene::WaterComponent>(id); }
    scene::SplineComponent& Spline(scene::EntityID id) { return *World().GetComponent<scene::SplineComponent>(id); }
    scene::ClothComponent& Cloth(scene::EntityID id) { return *World().GetComponent<scene::ClothComponent>(id); }

    editor::OpArgs OwnerArgs(scene::EntityID id, const char* kind = "water")
    {
        editor::OpArgs args;
        args.Set("node", World().GetGameObject(id)->instanceId); args.Set("kind", std::string(kind));
        return args;
    }

    editor::OpResult Invoke(const char* op, const editor::OpArgs& args)
    {
        return editor::InvokeOperator(Context(), op, args);
    }

    editor::OpResult Convert(scene::EntityID id, const char* kind = "water", const char* source = "POINT_OBJECTS")
    {
        auto args = OwnerArgs(id, kind); args.Set("source", std::string(source));
        return Invoke("control_points.convert", args);
    }

    scene::EntityID River(const char* name = "River")
    {
        auto& object = World().CreateGameObject(name);
        auto& water = object.AddComponent<scene::WaterComponent>();
        water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
        water.splinePoints = {{31, {-6.0f, 1.0f, 0.0f}, 4.0f, 2.0f, -1.0f},
                              {7, {0.0f, 1.0f, 0.0f}, 5.0f, 3.0f, 2.0f},
                              {99, {6.0f, 2.0f, 0.0f}, 6.0f, 4.0f, 3.0f}};
        return object.GetID();
    }

    scene::EntityID Lake()
    {
        auto& object = World().CreateGameObject("Lake");
        auto& water = object.AddComponent<scene::WaterComponent>();
        water.surfaceShape = scene::WaterSurfaceShape::SPLINE_LAKE;
        water.splinePoints = {{11, {-3.0f, 1.0f, -3.0f}, 0.0f, 2.0f, 0.0f},
                              {22, {3.0f, 1.0f, -3.0f}, 0.0f, 2.0f, 0.0f},
                              {33, {3.0f, 1.0f, 3.0f}, 0.0f, 2.0f, 0.0f},
                              {44, {-3.0f, 1.0f, 3.0f}, 0.0f, 2.0f, 0.0f}};
        return object.GetID();
    }

    scene::GameObject* WaterTarget(scene::EntityID owner, size_t index)
    {
        return World().FindByGuid(Water(owner).pointBindings[index].target.guid);
    }

    editor::OpResult Move(scene::GameObject& target, math::Vector3 localPosition)
    {
        auto rows = editor::OpData::MakeArray(); auto row = editor::OpData::MakeObject();
        row.Set("node", target.instanceId); row.Set("position", localPosition); rows.Push(std::move(row));
        editor::OpArgs args; args.Set("moves", std::move(rows));
        return Invoke("target_point.move", args);
    }
};

TEST_F(ControlPointOperatorTest, WaterConversionPreservesNumericRowsAndUsesOneUndo)
{
    const auto ownerId = River();
    auto* owner = World().GetGameObject(ownerId);
    owner->transform.position = {12.0f, 100000000.0f, -8.0f};
    owner->transform.rotation = math::Quaternion::FromEuler({0.0f, 0.4f, 0.0f});
    owner->transform.scale = {-2.0f, 1.0f, 3.0f};
    const auto original = Water(ownerId).splinePoints;
    const size_t count = World().GameObjectCount();
    scene::FlushWorldTransforms(World());
    scene::WaterSplineSnapshot inlineGeometry;
    ASSERT_TRUE(scene::BuildWaterSplineSnapshot(Water(ownerId).surfaceShape, original,
        Water(ownerId).splineSamplesPerSegment, owner->transform, inlineGeometry,
        Water(ownerId).buoyancyDepth, Water(ownerId).splineMeshCellSize));

    const auto converted = Convert(ownerId);

    ASSERT_TRUE(converted.ok) << converted.message;
    EXPECT_EQ(m_undo.GetHistorySize(), 1u); EXPECT_EQ(m_undo.GetCursor(), 1u);
    EXPECT_EQ(World().GameObjectCount(), count + original.size() + 1);
    EXPECT_EQ(Water(ownerId).pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    EXPECT_TRUE(Water(ownerId).splinePoints.empty());
    scene::FlushWorldTransforms(World());
    const auto resolved = scene::ResolveWaterControlPoints(World(), ownerId, Water(ownerId));
    ASSERT_TRUE(resolved.valid); ASSERT_EQ(resolved.points.size(), original.size());
    std::vector<std::string> guids;
    for (size_t index = 0; index < original.size(); ++index) {
        const auto& row = resolved.points[index];
        EXPECT_EQ(row.id, original[index].id);
        EXPECT_VEC3_NEAR(row.localPosition, original[index].localPosition, testkit::kTolerance);
        EXPECT_FLOAT_EQ(row.width, original[index].width); EXPECT_FLOAT_EQ(row.depth, original[index].depth);
        EXPECT_FLOAT_EQ(row.flowSpeed, original[index].flowSpeed);
        auto* target = WaterTarget(ownerId, index); ASSERT_NE(target, nullptr);
        EXPECT_NE(target->GetComponent<scene::TargetPointComponent>(), nullptr);
        guids.push_back(target->instanceId);
    }
    scene::WaterSplineSnapshot objectGeometry;
    ASSERT_TRUE(scene::BuildWaterSplineSnapshot(Water(ownerId).surfaceShape, resolved.points,
        Water(ownerId).splineSamplesPerSegment, owner->transform, objectGeometry,
        Water(ownerId).buoyancyDepth, Water(ownerId).splineMeshCellSize));
    scene::WaterSplineSample inlineSample, objectSample;
    ASSERT_TRUE(scene::TrySampleWaterSpline(inlineGeometry, owner->transform.worldPosition.x, owner->transform.worldPosition.z, inlineSample));
    ASSERT_TRUE(scene::TrySampleWaterSpline(objectGeometry, owner->transform.worldPosition.x, owner->transform.worldPosition.z, objectSample));
    EXPECT_NEAR(objectSample.baseHeightOffset, inlineSample.baseHeightOffset, testkit::kTolerance);
    EXPECT_NEAR(objectSample.depth, inlineSample.depth, testkit::kTolerance);
    EXPECT_VEC3_NEAR(objectSample.worldVelocity, inlineSample.worldVelocity, testkit::kTolerance);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(World().GameObjectCount(), count); EXPECT_EQ(Water(ownerId).splinePoints, original);
    EXPECT_EQ(Water(ownerId).pointSourceMode, scene::ControlPointSourceMode::INLINE);
    ASSERT_TRUE(m_undo.Redo());
    for (size_t index = 0; index < guids.size(); ++index) EXPECT_EQ(Water(ownerId).pointBindings[index].target.guid, guids[index]);
    EXPECT_EQ(World().GameObjectCount(), count + original.size() + 1);
}

TEST_F(ControlPointOperatorTest, SplineConversionAndNumericReturnPreserveTheOrderedPositions)
{
    auto& object = World().CreateGameObject("Path"); const auto owner = object.GetID();
    object.transform.position = {0.0f, 100000000.0f, 0.0f}; object.transform.scale = {-3.0f, 2.0f, 0.5f};
    auto& spline = object.AddComponent<scene::SplineComponent>();
    spline.points = {{-4.0f, 1.0f, 2.0f}, {0.0f, 2.0f, 4.0f}, {4.0f, 3.0f, 1.0f}};
    const auto original = spline.points;
    ASSERT_TRUE(Convert(owner, "spline").ok);
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    const auto points = scene::ResolveSplinePoints(World(), owner, Spline(owner));
    ASSERT_TRUE(points.valid); ASSERT_EQ(points.points.size(), original.size());
    for (size_t index = 0; index < original.size(); ++index) {
        EXPECT_EQ(points.points[index].id, index + 1);
        EXPECT_VEC3_NEAR(points.points[index].localPosition, original[index], testkit::kTolerance);
    }
    const size_t objectCount = World().GameObjectCount();
    ASSERT_TRUE(Convert(owner, "spline", "INLINE").ok);
    EXPECT_EQ(World().GameObjectCount(), objectCount); EXPECT_TRUE(Spline(owner).pointBindings.empty());
    ASSERT_EQ(Spline(owner).points.size(), original.size());
    for (size_t index = 0; index < original.size(); ++index) EXPECT_VEC3_NEAR(Spline(owner).points[index], original[index], testkit::kTolerance);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(Spline(owner).pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    EXPECT_EQ(World().GameObjectCount(), objectCount);
}

TEST_F(ControlPointOperatorTest, OversizedInputAndPrefabStructureFailBeforeCreatingActors)
{
    const auto owner = River();
    auto& points = Water(owner).splinePoints; points.clear();
    for (uint32_t index = 0; index <= scene::WATER_SPLINE_MAX_CONTROL_POINTS; ++index)
        points.push_back({index + 1, {static_cast<float>(index), 1.0f, 0.0f}, 4.0f, 2.0f, 0.0f});
    const auto original = points;
    EXPECT_FALSE(World().CanCreateGameObjects((std::numeric_limits<size_t>::max)()));
    EXPECT_FALSE(Convert(owner).ok);
    EXPECT_EQ(World().GameObjectCount(), 1u); EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    EXPECT_EQ(Water(owner).splinePoints, original);

    const auto prefabOwner = River("Prefab Child");
    auto& parent = World().CreateGameObject("Prefab Root"); parent.prefabAssetPath = "guid:prefab-source";
    World().GetGameObject(prefabOwner)->SetParent(parent);
    const auto before = Water(prefabOwner).splinePoints; const size_t count = World().GameObjectCount();
    EXPECT_FALSE(Convert(prefabOwner).ok);
    EXPECT_EQ(World().GameObjectCount(), count); EXPECT_EQ(Water(prefabOwner).splinePoints, before);
    editor::OpArgs create; create.Set("parent", parent.instanceId);
    EXPECT_FALSE(Invoke("target_point.create", create).ok);
    EXPECT_EQ(World().GameObjectCount(), count); EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(ControlPointOperatorTest, SharedBindingReorderUnbindAndMissingGuidKeepRowAttributes)
{
    const auto first = River("First"); const auto second = River("Second");
    ASSERT_TRUE(Convert(first).ok); ASSERT_TRUE(Convert(second).ok); m_undo.Clear();
    const std::string sharedGuid = WaterTarget(first, 0)->instanceId;
    const auto old = Water(second).pointBindings[1];
    auto bind = OwnerArgs(second); bind.Set("id", static_cast<int>(old.id)); bind.Set("target", sharedGuid);
    ASSERT_TRUE(Invoke("control_points.bind", bind).ok);
    auto reorder = OwnerArgs(second); reorder.Set("id", static_cast<int>(old.id)); reorder.Set("toIndex", 0);
    ASSERT_TRUE(Invoke("control_points.reorder", reorder).ok);
    EXPECT_EQ(Water(second).pointBindings.front().id, old.id);
    EXPECT_EQ(Water(second).pointBindings.front().target.guid, sharedGuid);
    EXPECT_FLOAT_EQ(Water(second).pointBindings.front().width, old.width);
    EXPECT_FLOAT_EQ(Water(second).pointBindings.front().depth, old.depth);
    EXPECT_FLOAT_EQ(Water(second).pointBindings.front().flowSpeed, old.flowSpeed);
    auto unbind = OwnerArgs(second); unbind.Set("id", static_cast<int>(old.id));
    ASSERT_TRUE(Invoke("control_points.unbind", unbind).ok);
    EXPECT_TRUE(Water(second).pointBindings.front().target.guid.empty());
    EXPECT_EQ(Water(second).pointBindings.front().id, old.id); EXPECT_NE(World().FindByGuid(sharedGuid), nullptr);
    ASSERT_TRUE(m_undo.Undo()); ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(Water(second).pointBindings[1].target.guid, sharedGuid);
    const auto targetId = World().FindByGuid(sharedGuid)->GetID();
    ASSERT_TRUE(World().DestroyGameObject(targetId));
    auto attributes = OwnerArgs(second); attributes.Set("id", static_cast<int>(old.id)); attributes.Set("width", 8.0f);
    ASSERT_TRUE(Invoke("control_points.set_water_attributes", attributes).ok);
    EXPECT_EQ(Water(second).pointBindings[1].target.guid, sharedGuid);
    EXPECT_FLOAT_EQ(Water(second).pointBindings[1].width, 8.0f);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_FLOAT_EQ(Water(second).pointBindings[1].width, old.width);
    EXPECT_EQ(Water(second).pointBindings[1].target.guid, sharedGuid);
}

TEST_F(ControlPointOperatorTest, ConversionUndoRefusesToDestroyALaterSharedPoint)
{
    const auto owner = River(); ASSERT_TRUE(Convert(owner).ok);
    const std::string sharedGuid = WaterTarget(owner, 0)->instanceId;
    auto& other = World().CreateGameObject("Other Consumer");
    auto& spline = other.AddComponent<scene::SplineComponent>();
    spline.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    scene::SplineControlPointBinding row; row.id = 1; row.target.guid = sharedGuid; spline.pointBindings.push_back(row);
    const size_t count = World().GameObjectCount(); const size_t cursor = m_undo.GetCursor();

    EXPECT_FALSE(m_undo.Undo());

    EXPECT_EQ(m_undo.GetCursor(), cursor); EXPECT_EQ(World().GameObjectCount(), count);
    EXPECT_NE(World().FindByGuid(sharedGuid), nullptr);
    EXPECT_EQ(Water(owner).pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
}

TEST_F(ControlPointOperatorTest, LakeHeightPreservesSmallLocalOffsetsWithNegativeScaleAtLargeOrigin)
{
    const auto ownerId = Lake(); auto* owner = World().GetGameObject(ownerId);
    owner->transform.position = {0.0f, 100000000.0f, 0.0f}; owner->transform.scale = {-2.0f, 1.0f, 3.0f};
    ASSERT_TRUE(Convert(ownerId).ok); m_undo.Clear();
    const auto before = scene::ResolveWaterControlPoints(World(), ownerId, Water(ownerId)); ASSERT_TRUE(before.valid);
    const auto originalWorldY = WaterTarget(ownerId, 0)->transform.worldPosition.y;
    auto args = OwnerArgs(ownerId); args.Set("height", 2.0f);

    ASSERT_TRUE(Invoke("control_points.set_lake_height", args).ok);

    EXPECT_EQ(m_undo.GetHistorySize(), 1u); EXPECT_FLOAT_EQ(Water(ownerId).lakePlaneHeight, 2.0f);
    const auto after = scene::ResolveWaterControlPoints(World(), ownerId, Water(ownerId)); ASSERT_TRUE(after.valid);
    EXPECT_NE(after.inputSignature, before.inputSignature);
    for (size_t index = 0; index < after.points.size(); ++index) {
        EXPECT_FLOAT_EQ(after.points[index].localPosition.y, 2.0f);
        EXPECT_FLOAT_EQ(after.points[index].localPosition.x, before.points[index].localPosition.x);
        EXPECT_FLOAT_EQ(after.points[index].localPosition.z, before.points[index].localPosition.z);
    }
    EXPECT_FLOAT_EQ(WaterTarget(ownerId, 0)->transform.worldPosition.y, originalWorldY);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_FLOAT_EQ(Water(ownerId).lakePlaneHeight, 1.0f);
    EXPECT_FLOAT_EQ(WaterTarget(ownerId, 0)->transform.position.y, 1.0f);
    ASSERT_TRUE(m_undo.Redo()); EXPECT_FLOAT_EQ(WaterTarget(ownerId, 0)->transform.position.y, 2.0f);
}

TEST_F(ControlPointOperatorTest, LakeHeightRejectsSharedAndExternalTargetsAtomically)
{
    const auto owner = Lake(); ASSERT_TRUE(Convert(owner).ok); m_undo.Clear();
    const auto targetId = WaterTarget(owner, 0)->GetID(); const std::string guid = WaterTarget(owner, 0)->instanceId;
    auto& other = World().CreateGameObject("Shared Consumer");
    auto& spline = other.AddComponent<scene::SplineComponent>();
    spline.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    scene::SplineControlPointBinding row; row.id = 1; row.target.guid = guid; spline.pointBindings.push_back(row);
    auto height = OwnerArgs(owner); height.Set("height", 4.0f);
    EXPECT_FALSE(Invoke("control_points.set_lake_height", height).ok);
    EXPECT_FLOAT_EQ(Water(owner).lakePlaneHeight, 1.0f); EXPECT_FLOAT_EQ(World().GetGameObject(targetId)->transform.position.y, 1.0f);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    other.RemoveComponent<scene::SplineComponent>();
    auto& external = World().CreateGameObject("External Point"); external.AddComponent<scene::TargetPointComponent>();
    auto bind = OwnerArgs(owner); bind.Set("id", static_cast<int>(Water(owner).pointBindings.front().id)); bind.Set("target", external.instanceId);
    ASSERT_TRUE(Invoke("control_points.bind", bind).ok); const size_t cursor = m_undo.GetCursor();
    EXPECT_FALSE(Invoke("control_points.set_lake_height", height).ok);
    EXPECT_EQ(m_undo.GetCursor(), cursor); EXPECT_FLOAT_EQ(Water(owner).lakePlaneHeight, 1.0f);
    EXPECT_FLOAT_EQ(World().GetGameObject(targetId)->transform.position.y, 1.0f);
}

TEST_F(ControlPointOperatorTest, BakeAndUndoPreservePublishedGeometryRipplesAndOtherSettings)
{
    const auto owner = River(); ASSERT_TRUE(Convert(owner).ok); m_undo.Clear();
    scene::PrepareWaterSimulationSurfaces(World());
    const auto published = Water(owner).simulationSplineSnapshot;
    ASSERT_NE(published, nullptr); ASSERT_TRUE(Water(owner).simulationSurfacePublished);
    auto args = OwnerArgs(owner); ASSERT_TRUE(Invoke("control_points.bake", args).ok);
    EXPECT_EQ(m_undo.GetHistorySize(), 1u); EXPECT_TRUE(Water(owner).bakeValid);
    EXPECT_EQ(Water(owner).pointRuntimeMode, scene::ControlPointRuntimeMode::BAKED_STATIC);
    const auto baked = scene::ResolveWaterControlPoints(World(), owner, Water(owner)); ASSERT_TRUE(baked.valid); EXPECT_FALSE(baked.bakeStale);
    scene::WaterRipple ripple;
    ripple.center = {3.0f, 0.0f}; ripple.radius = 2.0f; ripple.width = 0.5f;
    ripple.amplitude = 0.75f; ripple.meshFade = 1.0f; ripple.maxRadius = 20.0f;
    Water(owner).ripples.push_back(ripple);
    Water(owner).cellSize = {1.25f, 2.5f}; Water(owner).buoyancy = 42.0f;
    auto* target = WaterTarget(owner, 1); ASSERT_NE(target, nullptr);
    ASSERT_TRUE(Move(*target, target->transform.position + math::Vector3{0.0f, 1.0f, 0.0f}).ok);
    const auto stale = scene::ResolveWaterControlPoints(World(), owner, Water(owner));
    ASSERT_TRUE(stale.valid); EXPECT_TRUE(stale.bakeStale); EXPECT_EQ(stale.points, baked.points);
    EXPECT_EQ(Water(owner).simulationSplineSnapshot, published);
    ASSERT_TRUE(m_undo.Undo()); ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(Water(owner).pointRuntimeMode, scene::ControlPointRuntimeMode::LIVE); EXPECT_FALSE(Water(owner).bakeValid);
    EXPECT_EQ(Water(owner).simulationSplineSnapshot, published); EXPECT_TRUE(Water(owner).simulationSurfacePublished);
    ASSERT_EQ(Water(owner).ripples.size(), 1u); EXPECT_FLOAT_EQ(Water(owner).ripples.front().amplitude, 0.75f);
    EXPECT_FLOAT_EQ(Water(owner).ripples.front().radius, 2.0f);
    EXPECT_EQ(Water(owner).cellSize, (math::Vector2{1.25f, 2.5f})); EXPECT_FLOAT_EQ(Water(owner).buoyancy, 42.0f);
}

TEST_F(ControlPointOperatorTest, StaticSplineModeKeepsItsBakedClosureAndModeUndoRestoresThePolicy)
{
    auto& object = World().CreateGameObject("Spline"); const auto owner = object.GetID();
    object.AddComponent<scene::SplineComponent>().points = {{0.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 0.0f}, {4.0f, 0.0f, 1.0f}};
    auto args = OwnerArgs(owner, "spline");
    ASSERT_TRUE(Invoke("control_points.bake", args).ok); EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    const auto baked = scene::ResolveSplinePoints(World(), owner, Spline(owner));
    ASSERT_TRUE(baked.valid); EXPECT_FALSE(baked.bakeStale); EXPECT_FALSE(baked.closed);
    Spline(owner).closed = true;
    const auto stale = scene::ResolveSplinePoints(World(), owner, Spline(owner));
    ASSERT_TRUE(stale.valid); EXPECT_TRUE(stale.bakeStale); EXPECT_FALSE(stale.closed); EXPECT_EQ(stale.points, baked.points);
    args.Set("mode", std::string("LIVE")); ASSERT_TRUE(Invoke("control_points.set_runtime", args).ok);
    const auto live = scene::ResolveSplinePoints(World(), owner, Spline(owner));
    ASSERT_TRUE(live.valid); EXPECT_TRUE(live.closed);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(Spline(owner).pointRuntimeMode, scene::ControlPointRuntimeMode::BAKED_STATIC);
    EXPECT_FALSE(scene::ResolveSplinePoints(World(), owner, Spline(owner)).closed);
}

TEST_F(ControlPointOperatorTest, InlineLakeBakeUsesItsCommonHeightAndRemainsValidAfterSceneReload)
{
    const auto owner = Lake();
    EXPECT_EQ(Water(owner).pointSourceMode, scene::ControlPointSourceMode::INLINE);
    EXPECT_FLOAT_EQ(Water(owner).lakePlaneHeight, 0.0f);
    const std::string ownerGuid = World().GetGameObject(owner)->instanceId;
    const auto args = OwnerArgs(owner);

    ASSERT_TRUE(Invoke("control_points.bake", args).ok);

    EXPECT_FLOAT_EQ(Water(owner).bakedLakePlaneHeight, 1.0f);
    const auto snapshot = scene::ResolveWaterControlPoints(World(), owner, Water(owner));
    ASSERT_TRUE(snapshot.valid); EXPECT_FALSE(snapshot.bakeStale);
    EXPECT_FLOAT_EQ(snapshot.lakePlaneHeight, 1.0f);
    EXPECT_TRUE(scene::ValidateStaticControlPointBakes(World()));
    EXPECT_EQ(World().GameObjectCount(), 1u);
    const auto text = scene::SceneSerializer::SaveToText(World());
    ASSERT_FALSE(text.empty());
    auto reloaded = scene::SceneSerializer::LoadFromText(text, nullptr, {});
    ASSERT_NE(reloaded, nullptr);
    auto* reloadedOwner = reloaded->FindByGuid(ownerGuid);
    ASSERT_NE(reloadedOwner, nullptr);
    auto* water = reloadedOwner->GetComponent<scene::WaterComponent>();
    ASSERT_NE(water, nullptr);
    const auto restored = scene::ResolveWaterControlPoints(*reloaded, reloadedOwner->GetID(), *water);
    ASSERT_TRUE(restored.valid); EXPECT_FALSE(restored.bakeStale);
    EXPECT_FLOAT_EQ(restored.lakePlaneHeight, 1.0f);
    EXPECT_TRUE(scene::ValidateStaticControlPointBakes(*reloaded));
}

TEST_F(ControlPointOperatorTest, ClothLegacyRowsAreVerifiedOnlyByExplicitRebindAndUnbindRemovesTheConstraint)
{
    auto& ownerObject = World().CreateGameObject("Cloth"); const auto owner = ownerObject.GetID();
    auto& target = World().CreateGameObject("Target"); target.AddComponent<scene::TargetPointComponent>();
    auto& cloth = ownerObject.AddComponent<scene::ClothComponent>(); cloth.segments = 2;
    scene::ClothAttachment old; old.particle = 0; old.target = target.GetID(); old.localPosition = {0.25f, 0.0f, 0.0f}; old.maxDistance = 0.1f;
    cloth.attachments.push_back(old);
    physics::World physics;
    SystemContext system{World(), physics, nullptr, nullptr, 1.0f / 60.0f, 1.0f / 60.0f, true, true};
    scene::ClothSystem{}.Update(system);
    ASSERT_TRUE(Cloth(owner).runtime.initialized); EXPECT_EQ(Cloth(owner).attachments.front().topologySignature, 0u);
    const size_t particleCount = Cloth(owner).runtime.solver.Positions().size();
    auto bind = OwnerArgs(owner, "cloth"); bind.Set("particle", 0); bind.Set("target", target.instanceId); bind.Set("maxDistance", 1.25f);
    ASSERT_TRUE(Invoke("control_points.bind", bind).ok);
    const uint32_t id = Cloth(owner).attachments.front().bindingId;
    EXPECT_NE(id, 0u); EXPECT_EQ(Cloth(owner).attachments.front().targetReference.guid, target.instanceId);
    EXPECT_EQ(Cloth(owner).attachments.front().topologySignature, scene::ClothTopologySignature(nullptr, 2));
    EXPECT_FLOAT_EQ(Cloth(owner).attachments.front().maxDistance, 1.25f);
    EXPECT_VEC3_NEAR(Cloth(owner).attachments.front().localPosition, math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_EQ(Cloth(owner).runtime.solver.Positions().size(), particleCount);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(Cloth(owner).attachments.front().bindingId, 0u); EXPECT_EQ(Cloth(owner).attachments.front().topologySignature, 0u);
    EXPECT_EQ(Cloth(owner).attachments.front().target, target.GetID());
    EXPECT_VEC3_NEAR(Cloth(owner).attachments.front().localPosition, old.localPosition, testkit::kTolerance);
    ASSERT_TRUE(m_undo.Redo());
    auto unbind = OwnerArgs(owner, "cloth"); unbind.Set("id", static_cast<int>(id));
    ASSERT_TRUE(Invoke("control_points.unbind", unbind).ok);
    EXPECT_TRUE(Cloth(owner).attachments.empty()); EXPECT_TRUE(Cloth(owner).runtime.initialized);
    EXPECT_EQ(Cloth(owner).runtime.solver.Positions().size(), particleCount); EXPECT_TRUE(target.IsValid());
    ASSERT_TRUE(m_undo.Undo()); EXPECT_EQ(Cloth(owner).attachments.front().bindingId, id);
}

TEST_F(ControlPointOperatorTest, ClothBindRejectsInvalidParticlesAndUndoRejectsChangedTopology)
{
    auto& ownerObject = World().CreateGameObject("Cloth"); const auto owner = ownerObject.GetID();
    ownerObject.AddComponent<scene::ClothComponent>().segments = 2;
    auto& target = World().CreateGameObject("Ordinary Target");
    auto bind = OwnerArgs(owner, "cloth"); bind.Set("particle", 9); bind.Set("target", target.instanceId);
    EXPECT_FALSE(Invoke("control_points.bind", bind).ok);
    EXPECT_TRUE(Cloth(owner).attachments.empty()); EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    bind.Set("particle", 8); ASSERT_TRUE(Invoke("control_points.bind", bind).ok);
    const uint64_t topology = Cloth(owner).attachments.front().topologySignature;
    Cloth(owner).segments = 3; const size_t cursor = m_undo.GetCursor();
    EXPECT_FALSE(m_undo.Undo()); EXPECT_EQ(m_undo.GetCursor(), cursor);
    EXPECT_EQ(Cloth(owner).attachments.front().topologySignature, topology);
    EXPECT_EQ(Cloth(owner).attachments.front().targetReference.guid, target.instanceId);
}

TEST_F(ControlPointOperatorTest, MoveUsesOneCheckedUndoAndSamePositionIsANoOp)
{
    const auto owner = River(); ASSERT_TRUE(Convert(owner).ok); m_undo.Clear();
    auto* target = WaterTarget(owner, 0); ASSERT_NE(target, nullptr);
    const auto original = target->transform.position; const auto rotation = target->transform.rotation; const auto scale = target->transform.scale;
    ASSERT_TRUE(Move(*target, original).ok); EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    const auto desired = original + math::Vector3{1.0f, 2.0f, 3.0f};
    ASSERT_TRUE(Move(*target, desired).ok); EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    EXPECT_VEC3_NEAR(target->transform.position, desired, testkit::kTolerance);
    EXPECT_EQ(target->transform.rotation, rotation); EXPECT_EQ(target->transform.scale, scale);
    ASSERT_TRUE(m_undo.Undo()); EXPECT_VEC3_NEAR(target->transform.position, original, testkit::kTolerance);
    ASSERT_TRUE(m_undo.Redo()); EXPECT_VEC3_NEAR(target->transform.position, desired, testkit::kTolerance);
}

TEST_F(ControlPointOperatorTest, CreateClothAnchorConnectsOwnerLocalMarkerInOneUndoWithoutResettingSimulation)
{
    auto& object = World().CreateGameObject("Cloth"); const auto owner = object.GetID();
    object.AddComponent<scene::ClothComponent>().segments = 2;
    physics::World physics;
    SystemContext system{World(), physics, nullptr, nullptr, 1.0f / 60.0f, 1.0f / 60.0f, true, true};
    scene::ClothSystem{}.Update(system);
    ASSERT_TRUE(Cloth(owner).runtime.initialized);
    const auto positions = Cloth(owner).runtime.solver.Positions();
    object.transform.position = {100000000.0f, -20000000.0f, 30000000.0f}; object.transform.scale = {-2.0f, 3.0f, 0.5f};
    auto args = OwnerArgs(owner, "cloth"); args.Set("particle", 0);
    args.Set("markerPosition", math::Vector3{0.125f, 1.0f, -2.0f});
    args.Set("localPosition", math::Vector3{0.5f, 0.0f, 0.25f}); args.Set("maxDistance", 1.25f);
    ASSERT_TRUE(Invoke("control_points.create_cloth_anchor", args).ok);
    ASSERT_EQ(Cloth(owner).attachments.size(), 1u);
    const auto id = Cloth(owner).attachments[0].bindingId; const auto guid = Cloth(owner).attachments[0].targetReference.guid;
    EXPECT_NE(id, 0u); EXPECT_EQ(Cloth(owner).attachments[0].topologySignature, scene::ClothTopologySignature(nullptr, 2));
    EXPECT_VEC3_NEAR(Cloth(owner).attachments[0].localPosition, (math::Vector3{0.5f, 0.0f, 0.25f}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(Cloth(owner).attachments[0].maxDistance, 1.25f);
    auto* point = World().FindByGuid(guid); ASSERT_NE(point, nullptr); EXPECT_EQ(point->GetParent(), &object);
    EXPECT_NE(point->GetComponent<scene::TargetPointComponent>(), nullptr);
    EXPECT_VEC3_NEAR(point->transform.position, (math::Vector3{0.125f, 1.0f, -2.0f}), testkit::kTolerance);
    EXPECT_EQ(m_undo.GetHistorySize(), 1u); EXPECT_EQ(Cloth(owner).runtime.solver.Positions(), positions);
    Cloth(owner).enabled = false;
    ASSERT_TRUE(m_undo.Undo()); EXPECT_TRUE(Cloth(owner).attachments.empty()); EXPECT_EQ(World().FindByGuid(guid), nullptr);
    EXPECT_FALSE(Cloth(owner).enabled); EXPECT_TRUE(Cloth(owner).runtime.initialized);
    EXPECT_EQ(Cloth(owner).runtime.solver.Positions(), positions);
    ASSERT_TRUE(m_undo.Redo()); ASSERT_EQ(Cloth(owner).attachments.size(), 1u);
    EXPECT_EQ(Cloth(owner).attachments[0].bindingId, id); EXPECT_EQ(Cloth(owner).attachments[0].targetReference.guid, guid);
    EXPECT_NE(World().FindByGuid(guid), nullptr); EXPECT_FALSE(Cloth(owner).enabled);
}

TEST_F(ControlPointOperatorTest, CreateClothAnchorRejectsInvalidInputAndPrefabStructureWithoutLeavingObjects)
{
    auto& object = World().CreateGameObject("Cloth"); const auto owner = object.GetID(); object.AddComponent<scene::ClothComponent>().segments = 2;
    auto args = OwnerArgs(owner, "cloth"); args.Set("particle", 9);
    const auto count = World().GameObjectCount();
    EXPECT_FALSE(Invoke("control_points.create_cloth_anchor", args).ok);
    args.Set("particle", 0); args.Set("maxDistance", -1.0f);
    EXPECT_FALSE(Invoke("control_points.create_cloth_anchor", args).ok);
    args.Set("maxDistance", 0.0f); args.Set("id", 999);
    EXPECT_FALSE(Invoke("control_points.create_cloth_anchor", args).ok);
    args.Set("id", 0); object.prefabAssetPath = "guid:prefab-structure";
    EXPECT_FALSE(Invoke("control_points.create_cloth_anchor", args).ok);
    EXPECT_EQ(World().GameObjectCount(), count); EXPECT_TRUE(Cloth(owner).attachments.empty()); EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(ControlPointOperatorTest, CreateClothAnchorExplicitlyRebindsLegacyRowAndUndoRestoresItsOldExternalTarget)
{
    auto& object = World().CreateGameObject("Cloth"); const auto owner = object.GetID(); auto& cloth = object.AddComponent<scene::ClothComponent>(); cloth.segments = 2;
    auto& external = World().CreateGameObject("External"); const auto externalId = external.GetID();
    scene::ClothAttachment legacy; legacy.particle = 0; legacy.target = externalId; legacy.maxDistance = 0.25f;
    legacy.localPosition = {0.125f, 0.0f, 0.0f}; cloth.attachments.push_back(legacy);
    auto args = OwnerArgs(owner, "cloth"); args.Set("particle", 0); args.Set("markerPosition", math::Vector3{1.0f, 2.0f, 3.0f});
    ASSERT_TRUE(Invoke("control_points.create_cloth_anchor", args).ok);
    ASSERT_EQ(Cloth(owner).attachments.size(), 1u); const auto guid = Cloth(owner).attachments[0].targetReference.guid;
    EXPECT_NE(Cloth(owner).attachments[0].bindingId, 0u); EXPECT_NE(Cloth(owner).attachments[0].topologySignature, 0u);
    EXPECT_NE(World().FindByGuid(guid), nullptr); EXPECT_EQ(World().GetGameObject(externalId), &external);
    ASSERT_TRUE(m_undo.Undo()); ASSERT_EQ(Cloth(owner).attachments.size(), 1u);
    EXPECT_EQ(Cloth(owner).attachments[0].bindingId, 0u); EXPECT_EQ(Cloth(owner).attachments[0].topologySignature, 0u);
    EXPECT_EQ(Cloth(owner).attachments[0].target, externalId); EXPECT_EQ(World().FindByGuid(guid), nullptr);
    EXPECT_EQ(World().GetGameObject(externalId), &external);
    EXPECT_VEC3_NEAR(Cloth(owner).attachments[0].localPosition, legacy.localPosition, testkit::kTolerance);
}

TEST_F(ControlPointOperatorTest, CreatedClothAnchorUndoProtectsOtherConsumerOnTheSameOwnerAndCustomIkReference)
{
    auto& object = World().CreateGameObject("Cloth"); const auto owner = object.GetID(); object.AddComponent<scene::ClothComponent>().segments = 2;
    auto args = OwnerArgs(owner, "cloth"); args.Set("particle", 0);
    ASSERT_TRUE(Invoke("control_points.create_cloth_anchor", args).ok);
    const auto guid = Cloth(owner).attachments[0].targetReference.guid; const auto cursor = m_undo.GetCursor();
    auto& spline = object.AddComponent<scene::SplineComponent>(); spline.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    spline.pointBindings.push_back({1, {guid, scene::EntityID::INVALID, 0}});
    EXPECT_FALSE(m_undo.Undo()); EXPECT_EQ(m_undo.GetCursor(), cursor); EXPECT_NE(World().FindByGuid(guid), nullptr);
    object.RemoveComponent<scene::SplineComponent>();
    auto& ik = object.AddComponent<scene::IKSolverComponent>(); ik.chains.emplace_back().targetReference.guid = guid;
    EXPECT_FALSE(m_undo.Undo()); EXPECT_EQ(m_undo.GetCursor(), cursor); EXPECT_NE(World().FindByGuid(guid), nullptr);
    object.RemoveComponent<scene::IKSolverComponent>(); ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(World().FindByGuid(guid), nullptr); EXPECT_TRUE(Cloth(owner).attachments.empty());
}

TEST_F(ControlPointOperatorTest, CreatedClothAnchorUndoRejectsMarkerEditsChildrenAndReplacedClothLifetime)
{
    auto& object = World().CreateGameObject("Cloth"); const auto owner = object.GetID(); object.AddComponent<scene::ClothComponent>().segments = 2;
    auto args = OwnerArgs(owner, "cloth"); args.Set("particle", 0);
    ASSERT_TRUE(Invoke("control_points.create_cloth_anchor", args).ok);
    const auto guid = Cloth(owner).attachments[0].targetReference.guid; const auto cursor = m_undo.GetCursor();
    auto* point = World().FindByGuid(guid); ASSERT_NE(point, nullptr);
    point->transform.position.x = 1.0e-7f;
    EXPECT_FALSE(m_undo.Undo()); EXPECT_EQ(m_undo.GetCursor(), cursor);
    point->transform.position.x = 0.0f;
    auto& child = World().CreateGameObject("Extra child"); const auto childId = child.GetID(); child.SetParent(*point);
    EXPECT_FALSE(m_undo.Undo()); EXPECT_EQ(m_undo.GetCursor(), cursor); ASSERT_TRUE(World().DestroyGameObject(childId));
    const auto after = Cloth(owner).attachments;
    object.RemoveComponent<scene::ClothComponent>(); auto& replacement = object.AddComponent<scene::ClothComponent>();
    replacement.segments = 2; replacement.attachments = after;
    EXPECT_FALSE(m_undo.Undo()); EXPECT_EQ(m_undo.GetCursor(), cursor); EXPECT_NE(World().FindByGuid(guid), nullptr);
    ASSERT_EQ(Cloth(owner).attachments.size(), 1u); EXPECT_EQ(Cloth(owner).attachments[0].targetReference.guid, guid);
}

} /// @note namespace fbzz::tests
