/// @file    SceneEditReferenceCloneTests.cpp
/// @brief   複製 forest の型付き object reference 再対応を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>

#include <Editor/Util/SceneEditUtils.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/WaterZoneComponent.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <memory>
#include <utility>

namespace fbzz::tests {

class SceneEditReferenceCloneTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        Context().undoStack = &m_undo;
    }

    void TearDown() override
    {
        m_undo.Clear();
        Context().undoStack = nullptr;
        EditorFixture::TearDown();
    }

    editor::UndoStack m_undo;
};

TEST_F(SceneEditReferenceCloneTest, DuplicateSelectionRemapsReferencesAcrossRootObjects)
{
    scene::Scene& scene = AttachScene();
    auto& owner = scene.CreateGameObject("Owner");
    auto& target = scene.CreateGameObject("Target");
    target.AddComponent<scene::TargetPointComponent>();

    scene::SplineComponent spline;
    spline.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    scene::SplineControlPointBinding binding;
    binding.id = 53;
    binding.target.guid = target.instanceId;
    binding.target.id = target.GetID();
    binding.target.sceneGeneration = scene.GetRenderSceneGeneration();
    spline.pointBindings.push_back(binding);
    owner.AddComponent<scene::SplineComponent>(std::move(spline));
    Context().selectedEntities = {owner.GetID(), target.GetID()};

    const auto command = editor::MakeDuplicateSelectedCommand(Context());

    ASSERT_NE(command, nullptr);
    scene::GameObject* clonedOwner = scene.Find("Owner (Clone)");
    scene::GameObject* clonedTarget = scene.Find("Target (Clone)");
    ASSERT_NE(clonedOwner, nullptr);
    ASSERT_NE(clonedTarget, nullptr);
    const auto* clonedSpline = clonedOwner->GetComponent<scene::SplineComponent>();
    ASSERT_NE(clonedSpline, nullptr);
    ASSERT_EQ(clonedSpline->pointBindings.size(), 1u);
    EXPECT_EQ(clonedSpline->pointBindings[0].id, 53u);
    EXPECT_EQ(clonedSpline->pointBindings[0].target.guid, clonedTarget->instanceId);
    EXPECT_EQ(clonedSpline->pointBindings[0].target.id, clonedTarget->GetID());
    EXPECT_EQ(clonedSpline->pointBindings[0].target.sceneGeneration,
              scene.GetRenderSceneGeneration());

    const auto* sourceSpline = owner.GetComponent<scene::SplineComponent>();
    ASSERT_NE(sourceSpline, nullptr);
    EXPECT_EQ(sourceSpline->pointBindings[0].target.guid, target.instanceId);
    EXPECT_EQ(sourceSpline->pointBindings[0].target.id, target.GetID());
}

TEST_F(SceneEditReferenceCloneTest, MissingScriptWithSavedFieldsRejectsCloneBeforeCreatingObjects)
{
    scene::Scene& scene = AttachScene();
    auto& owner = scene.CreateGameObject("OwnerWithMissingScript");
    scene::ScriptComponent scripts;
    scene::ScriptEntry entry;
    entry.serialized = std::make_unique<scene::SerializedScriptData>();
    entry.serialized->type = "Missing.ControlPointConsumer";
    entry.serialized->fieldsToml = "target = \"target-guid\"\n";
    scripts.scripts.push_back(std::move(entry));
    owner.AddComponent<scene::ScriptComponent>(std::move(scripts));
    Context().selectedEntities = {owner.GetID()};
    const std::size_t objectCount = scene.GameObjectCount();

    const auto command = editor::MakeDuplicateSelectedCommand(Context());

    EXPECT_EQ(command, nullptr);
    EXPECT_EQ(scene.GameObjectCount(), objectCount);
    EXPECT_EQ(scene.Find("OwnerWithMissingScript (Clone)"), nullptr);
    const auto* originalScripts = owner.GetComponent<scene::ScriptComponent>();
    ASSERT_NE(originalScripts, nullptr);
    ASSERT_EQ(originalScripts->scripts.size(), 1u);
    EXPECT_EQ(originalScripts->scripts[0].serialized->fieldsToml,
              "target = \"target-guid\"\n");
}

TEST_F(SceneEditReferenceCloneTest, MissingScriptWithoutFieldsRemainsCloneableMetadata)
{
    scene::Scene& scene = AttachScene();
    auto& owner = scene.CreateGameObject("MetadataOnlyOwner");
    scene::ScriptComponent scripts;
    scene::ScriptEntry entry;
    entry.serialized = std::make_unique<scene::SerializedScriptData>();
    entry.serialized->type = "Missing.FieldlessMetadata";
    scripts.scripts.push_back(std::move(entry));
    owner.AddComponent<scene::ScriptComponent>(std::move(scripts));
    Context().selectedEntities = {owner.GetID()};

    const auto command = editor::MakeDuplicateSelectedCommand(Context());

    ASSERT_NE(command, nullptr);
    scene::GameObject* clone = scene.Find("MetadataOnlyOwner (Clone)");
    ASSERT_NE(clone, nullptr);
    const auto* clonedScripts = clone->GetComponent<scene::ScriptComponent>();
    ASSERT_NE(clonedScripts, nullptr);
    ASSERT_EQ(clonedScripts->scripts.size(), 1u);
    EXPECT_EQ(clonedScripts->scripts[0].serialized->type, "Missing.FieldlessMetadata");
    EXPECT_TRUE(clonedScripts->scripts[0].serialized->fieldsToml.empty());
}

TEST_F(SceneEditReferenceCloneTest, PasteSynchronizesWorldTransformsForDestinationHierarchy)
{
    scene::Scene& scene = AttachScene();
    auto& source = scene.CreateGameObject("Source");
    source.transform.position = {2.0f, 0.0f, 0.0f};
    Context().selectedEntities = {source.GetID()};
    editor::CopySelectedToClipboard(Context());

    auto& destinationParent = scene.CreateGameObject("DestinationParent");
    destinationParent.transform.position = {100.0f, 0.0f, 0.0f};
    scene::FlushWorldTransforms(scene);

    const auto command = editor::MakePasteClipboardCommand(Context(), destinationParent.GetID());

    ASSERT_NE(command, nullptr);
    const scene::GameObject* pasted = scene.Find("Source (Copy)");
    ASSERT_NE(pasted, nullptr);
    EXPECT_VEC3_NEAR(pasted->transform.worldPosition,
                     (math::Vector3{102.0f, 0.0f, 0.0f}), testkit::kTolerance);
}

TEST_F(SceneEditReferenceCloneTest, CloneClearsWaterRuntimeStateButKeepsZoneAuthoring)
{
    scene::Scene& scene = AttachScene();
    auto& owner = scene.CreateGameObject("WaterOwner");
    scene::WaterComponent water;
    water.zoneSurfaceSelected = true;
    water.simulationSurfacePublished = true;
    water.simulationReadEnabled = true;
    water.zoneOwner = owner.GetID();
    water.splineSnapshotAttempted = true;
    water.splineSnapshotAttemptSignature = 73;
    owner.AddComponent<scene::WaterComponent>(std::move(water));
    scene::WaterZoneComponent zone;
    zone.ownerPriority = 4;
    zone.meshCellSize = 3.5f;
    zone.simulationPublished = true;
    zone.attempted = true;
    zone.attemptSignature = 91;
    zone.diagnostic = "stale runtime diagnostic";
    owner.AddComponent<scene::WaterZoneComponent>(std::move(zone));
    Context().selectedEntities = {owner.GetID()};

    const auto command = editor::MakeDuplicateSelectedCommand(Context());

    ASSERT_NE(command, nullptr);
    scene::GameObject* clone = scene.Find("WaterOwner (Clone)");
    ASSERT_NE(clone, nullptr);
    const auto* clonedWater = clone->GetComponent<scene::WaterComponent>();
    ASSERT_NE(clonedWater, nullptr);
    EXPECT_FALSE(clonedWater->zoneSurfaceSelected);
    EXPECT_FALSE(clonedWater->simulationSurfacePublished);
    EXPECT_FALSE(clonedWater->simulationReadEnabled);
    EXPECT_FALSE(clonedWater->zoneOwner.IsValid());
    EXPECT_FALSE(clonedWater->splineSnapshotAttempted);
    const auto* clonedZone = clone->GetComponent<scene::WaterZoneComponent>();
    ASSERT_NE(clonedZone, nullptr);
    EXPECT_EQ(clonedZone->ownerPriority, 4);
    EXPECT_FLOAT_EQ(clonedZone->meshCellSize, 3.5f);
    EXPECT_FALSE(clonedZone->simulationPublished);
    EXPECT_FALSE(clonedZone->attempted);
    EXPECT_EQ(clonedZone->attemptSignature, 0u);
    EXPECT_TRUE(clonedZone->diagnostic.empty());
}

} /// @note namespace fbzz::tests
