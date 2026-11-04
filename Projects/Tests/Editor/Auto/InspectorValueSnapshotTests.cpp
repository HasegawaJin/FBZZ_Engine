/// @file    InspectorValueSnapshotTests.cpp
/// @brief   編集前値の復元・構造体配列・観測非評価と最初の操作の Undo を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/Components/SpringBoneComponent.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/VolumetricCloudComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/OwnedResources.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Util/InspectorValueSnapshot.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include "Panels/Inspector/InspectorCommon.hpp"
#include "Util/MaterialAuthoringEditing.hpp"
#include <imgui_internal.h>
#include <bit>
#include <cstdint>

namespace fbzz::tests {
namespace {
struct AuthoredState {
    float value = 1;
    std::vector<std::string> names{"first", "second"};
    std::vector<math::Vector3> points{{1, 2, 3}, {4, 5, 6}};
    int observationCalls = 0;
    void Reflect(scene::IReflector& reflector)
    {
        reflector.Field("value", value);
        reflector.ListField("names", names);
        points.resize(reflector.BeginObjectList("points", points.size()));
        for (size_t index = 0; index < points.size(); ++index) {
            reflector.BeginObjectElement(index);
            reflector.Field("point", points[index]);
            reflector.EndObjectElement();
        }
        (void)reflector.EndObjectList();
        if (reflector.BeginObservation("runtime", "int")) ++observationCalls;
    }
};

class InspectorValueSnapshotTest : public testkit::Fixture {};

TEST_F(InspectorValueSnapshotTest, RestoresPreEditListsAndNestedObjectCount)
{
    editor::InspectorValueSnapshot snapshot;
    AuthoredState state;
    snapshot.BeginCapture();
    state.Reflect(snapshot.Reflector());
    snapshot.EndCapture();
    state.value = 9;
    state.names = {"changed"};
    state.points = {{7, 8, 9}};
    snapshot.BeginApply();
    state.Reflect(snapshot.Reflector());
    EXPECT_FLOAT_EQ(state.value, 1);
    EXPECT_EQ(state.names, (std::vector<std::string>{"first", "second"}));
    ASSERT_EQ(state.points.size(), 2u);
    EXPECT_VEC3_NEAR(state.points[0], math::Vector3(1, 2, 3), 0.0001f);
    EXPECT_VEC3_NEAR(state.points[1], math::Vector3(4, 5, 6), 0.0001f);
    EXPECT_EQ(state.observationCalls, 0);
}

TEST_F(InspectorValueSnapshotTest, ReusedCaptureKeepsExactFloatBitsAndNewExternalValues)
{
    editor::InspectorValueSnapshot snapshot;
    AuthoredState state;
    snapshot.BeginCapture(); state.Reflect(snapshot.Reflector()); snapshot.EndCapture();
    state.value = std::bit_cast<float>(uint32_t{0x3F800001});
    state.names[0] = "external";
    snapshot.BeginCapture(); state.Reflect(snapshot.Reflector()); snapshot.EndCapture();
    state.value = 42;
    state.names[0] = "edited";
    snapshot.BeginApply(); state.Reflect(snapshot.Reflector());
    EXPECT_EQ(std::bit_cast<uint32_t>(state.value), uint32_t{0x3F800001});
    EXPECT_EQ(state.names[0], "external");
}

TEST_F(InspectorValueSnapshotTest, UndeclaredReflectionCoverageKeepsTheFullCopyContract)
{
    static_assert(!editor::ComponentUndoReflects<scene::ParticleEmitter>::value);
    editor::InspectorValueSnapshot snapshot;
    scene::ParticleEmitter emitter;
    emitter.settings.bursts = {{0.25f, 17, 3, 0.5f, 0.75f}};
    emitter.runtime.playTime = 12;
    snapshot.BeginCapture();
    editor::InspectorUndoValues(emitter, snapshot);
    snapshot.EndCapture();
    emitter.settings.bursts.clear();
    emitter.settings.emitRate = 999;
    snapshot.BeginApply();
    editor::InspectorUndoValues(emitter, snapshot);
    ASSERT_EQ(emitter.settings.bursts.size(), 1u);
    const auto& burst = emitter.settings.bursts[0];
    EXPECT_FLOAT_EQ(burst.time, 0.25f);
    EXPECT_EQ(burst.count, 17);
    EXPECT_EQ(burst.cycles, 3);
    EXPECT_FLOAT_EQ(burst.interval, 0.5f);
    EXPECT_FLOAT_EQ(burst.probability, 0.75f);
    EXPECT_FLOAT_EQ(emitter.settings.emitRate, 30);
    EXPECT_FLOAT_EQ(emitter.runtime.playTime, 0);
}

TEST_F(InspectorValueSnapshotTest, SpringBoneChainRemovalAndAdditionRestoreAuthoredValuesOnly)
{
    editor::InspectorValueSnapshot snapshot;
    scene::SpringBoneComponent spring;
    spring.chains.resize(2);
    spring.chains[0].rootBoneName = "Hair";
    spring.chains[0].stiffness = 0.25f;
    spring.chains[0].nodes.resize(4);
    spring.chains[1].rootBoneName = "Cloth";
    spring.chains[1].weight = 0.75f;
    spring.chains[1].nodes.resize(3);
    spring.colliders.resize(1);
    spring.colliders[0].boneName = "Head";
    spring.teleportResetDistance = 2;
    snapshot.BeginCapture();
    editor::InspectorUndoValues(spring, snapshot);
    snapshot.EndCapture();
    spring.chains.resize(1);
    spring.chains[0].rootBoneName = "Changed";
    spring.chains[0].nodes.resize(1);
    spring.chains[0].nodes[0].nodeIndex = 42;
    spring.colliders.clear();
    spring.teleportResetDistance = 8;
    snapshot.BeginApply();
    editor::InspectorUndoValues(spring, snapshot);
    ASSERT_EQ(spring.chains.size(), 2u);
    EXPECT_EQ(spring.chains[0].rootBoneName, "Hair");
    EXPECT_FLOAT_EQ(spring.chains[0].stiffness, 0.25f);
    EXPECT_EQ(spring.chains[1].rootBoneName, "Cloth");
    EXPECT_FLOAT_EQ(spring.chains[1].weight, 0.75f);
    ASSERT_EQ(spring.chains[0].nodes.size(), 1u);
    EXPECT_EQ(spring.chains[0].nodes[0].nodeIndex, 42);
    EXPECT_TRUE(spring.chains[1].nodes.empty());
    ASSERT_EQ(spring.colliders.size(), 1u);
    EXPECT_EQ(spring.colliders[0].boneName, "Head");
    EXPECT_FLOAT_EQ(spring.teleportResetDistance, 2);
    snapshot.BeginCapture();
    editor::InspectorUndoValues(spring, snapshot);
    snapshot.EndCapture();
    spring.chains.emplace_back();
    snapshot.BeginApply();
    editor::InspectorUndoValues(spring, snapshot);
    EXPECT_EQ(spring.chains.size(), 2u);
}

TEST_F(InspectorValueSnapshotTest, WaterCaptureDoesNotNormalizeOrChangeRuntimeAndDirtyState)
{
    editor::InspectorValueSnapshot snapshot;
    scene::WaterComponent water;
    water.resolutionX = 0;
    water.lodPatchResolution = 3;
    water.nearCellSize = 0;
    water.splinePoints.resize(1);
    water.splinePoints[0].id = 0;
    water.ripples.resize(2);
    water.meshDirty = water.foamDirty = water.texDirty = false;
    snapshot.BeginCapture();
    editor::InspectorUndoValues(water, snapshot);
    snapshot.EndCapture();
    EXPECT_EQ(water.resolutionX, 0u);
    EXPECT_EQ(water.lodPatchResolution, 3u);
    EXPECT_FLOAT_EQ(water.nearCellSize, 0);
    EXPECT_EQ(water.splinePoints[0].id, 0u);
    EXPECT_FALSE(water.meshDirty);
    EXPECT_FALSE(water.foamDirty);
    EXPECT_FALSE(water.texDirty);
    EXPECT_EQ(water.ripples.size(), 2u);
    water.resolutionX = 64;
    water.splinePoints[0].id = 17;
    water.ripples.resize(3);
    snapshot.BeginApply();
    editor::InspectorUndoValues(water, snapshot);
    EXPECT_EQ(water.resolutionX, 0u);
    EXPECT_EQ(water.lodPatchResolution, 3u);
    EXPECT_EQ(water.splinePoints[0].id, 0u);
    EXPECT_EQ(water.ripples.size(), 3u);
    EXPECT_FALSE(water.meshDirty);
    EXPECT_FALSE(water.foamDirty);
    EXPECT_FALSE(water.texDirty);
}

TEST_F(InspectorValueSnapshotTest, CaptureDoesNotRunScalarComponentNormalization)
{
    editor::InspectorValueSnapshot snapshot;
    scene::CharacterControllerComponent controller;
    controller.groundingMode = static_cast<decltype(controller.groundingMode)>(99);
    controller.isGrounded = false;
    scene::LightComponent light;
    light.type = static_cast<decltype(light.type)>(99);
    light.flickerSeed = 0x80000000u;
    scene::SkyRenderer sky;
    sky.dayAltitude = 0;
    sky.cloudShadowSize = 0;
    scene::VolumetricCloudComponent cloud;
    cloud.coverage = 2;
    cloud.stepCount = 2;
    scene::FlowField flow;
    flow.forces[0].fieldType = static_cast<decltype(flow.forces[0].fieldType)>(99);
    flow.forces[0].space = static_cast<decltype(flow.forces[0].space)>(99);

    snapshot.BeginCapture();
    editor::InspectorUndoValues(controller, snapshot);
    editor::InspectorUndoValues(light, snapshot);
    editor::InspectorUndoValues(sky, snapshot);
    editor::InspectorUndoValues(cloud, snapshot);
    editor::InspectorUndoValues(flow, snapshot);
    snapshot.EndCapture();

    EXPECT_EQ(static_cast<int>(controller.groundingMode), 99);
    EXPECT_FALSE(controller.isGrounded);
    EXPECT_EQ(static_cast<int>(light.type), 99);
    EXPECT_EQ(light.flickerSeed, 0x80000000u);
    EXPECT_FLOAT_EQ(sky.dayAltitude, 0);
    EXPECT_FLOAT_EQ(sky.cloudShadowSize, 0);
    EXPECT_FLOAT_EQ(cloud.coverage, 2);
    EXPECT_EQ(cloud.stepCount, 2);
    EXPECT_EQ(static_cast<int>(flow.forces[0].fieldType), 99);
    EXPECT_EQ(static_cast<int>(flow.forces[0].space), 99);
    cloud.coverage = 0.5f;
    light.flickerSeed = 0;
    snapshot.BeginApply();
    editor::InspectorUndoValues(controller, snapshot);
    editor::InspectorUndoValues(light, snapshot);
    editor::InspectorUndoValues(sky, snapshot);
    editor::InspectorUndoValues(cloud, snapshot);
    editor::InspectorUndoValues(flow, snapshot);
    EXPECT_FLOAT_EQ(cloud.coverage, 2);
    EXPECT_EQ(light.flickerSeed, 0x80000000u);
}

TEST_F(InspectorValueSnapshotTest, ColliderCaptureDoesNotClampCombineModesOrReplaceTheLiveShape)
{
    editor::InspectorValueSnapshot snapshot;
    scene::BoxColliderComponent collider;
    collider.material.restitutionCombine = static_cast<decltype(collider.material.restitutionCombine)>(99);
    collider.material.frictionCombine = static_cast<decltype(collider.material.frictionCombine)>(98);
    const auto* liveShape = collider.collider.get();

    snapshot.BeginCapture();
    editor::InspectorUndoValues(collider, snapshot);
    snapshot.EndCapture();

    EXPECT_EQ(collider.collider.get(), liveShape);
    EXPECT_EQ(static_cast<int>(collider.material.restitutionCombine), 99);
    EXPECT_EQ(static_cast<int>(collider.material.frictionCombine), 98);
    collider.material.restitutionCombine = physics::PhysicsMaterialCombine::Maximum;
    snapshot.BeginApply();
    editor::InspectorUndoValues(collider, snapshot);
    EXPECT_EQ(static_cast<int>(collider.material.restitutionCombine), 99);
}

TEST_F(InspectorValueSnapshotTest, SkinnedProjectionRestoresAuthoredValuesWithoutRewindingRuntimeArrays)
{
    editor::InspectorValueSnapshot snapshot;
    scene::SkinnedMeshRenderer skinned;
    skinned.modelPath = "Assets/Old.fbx";
    skinned.submeshIndices = {4, 2};
    skinned.nodeEntities = {{3, 1}, {4, 1}};
    skinned.skinnedVertexBuffers = testkit::MakeOwnedResources<renderer::BufferTag>({{7, 2}});
    skinned.gpuSkinningContent = {1, 2, 3};
    const auto* nodeData = skinned.nodeEntities.data();
    snapshot.BeginCapture();
    editor::InspectorUndoValues(skinned, snapshot);
    snapshot.EndCapture();
    EXPECT_EQ(skinned.nodeEntities.data(), nodeData);
    skinned.modelPath = "Assets/New.fbx";
    skinned.submeshIndices.clear();
    skinned.castShadows = false;
    skinned.nodeEntities = {{9, 1}};
    skinned.skinnedVertexBuffers = testkit::MakeOwnedResources<renderer::BufferTag>({{8, 3}});
    skinned.gpuSkinningContent = {4, 5};

    snapshot.BeginApply();
    editor::InspectorUndoValues(skinned, snapshot);

    EXPECT_EQ(skinned.modelPath, "Assets/Old.fbx");
    EXPECT_EQ(skinned.submeshIndices, (std::vector<std::uint32_t>{4, 2}));
    EXPECT_TRUE(skinned.castShadows);
    EXPECT_EQ(skinned.nodeEntities, (std::vector<scene::EntityID>{{9, 1}}));
    EXPECT_EQ(testkit::BorrowResources(skinned.skinnedVertexBuffers),
        (std::vector<renderer::ResourceHandle<renderer::BufferTag>>{{8, 3}}));
    EXPECT_EQ(skinned.gpuSkinningContent, (std::vector<std::uint32_t>{4, 5}));
}

TEST_F(InspectorValueSnapshotTest, AnimatorProjectionDoesNotCaptureRuntimeClipsAndPalettes)
{
    editor::InspectorValueSnapshot snapshot;
    scene::AnimatorComponent animator;
    animator.controllerPath = "Assets/Old.controller";
    animator.speed = 2;
    animator.rootMotion.mode = static_cast<decltype(animator.rootMotion.mode)>(99);
    animator.clips.resize(1);
    animator.clips[0].name = "Previous runtime clip";
    animator.boneMatrices.resize(3);
    const auto* clips = animator.clips.data();
    snapshot.BeginCapture();
    editor::InspectorUndoValues(animator, snapshot);
    snapshot.EndCapture();
    EXPECT_EQ(animator.clips.data(), clips);
    EXPECT_EQ(static_cast<int>(animator.rootMotion.mode), 99);
    animator.controllerPath = "Assets/New.controller";
    animator.speed = 3;
    animator.rootMotion.mode = scene::RootMotionMode::None;
    animator.clips[0].name = "Current runtime clip";
    animator.boneMatrices.resize(5);

    snapshot.BeginApply();
    editor::InspectorUndoValues(animator, snapshot);

    EXPECT_EQ(animator.controllerPath, "Assets/Old.controller");
    EXPECT_FLOAT_EQ(animator.speed, 2);
    EXPECT_EQ(static_cast<int>(animator.rootMotion.mode), 99);
    EXPECT_EQ(animator.clips[0].name, "Current runtime clip");
    EXPECT_EQ(animator.boneMatrices.size(), 5u);
}

TEST_F(InspectorValueSnapshotTest, TrailProjectionKeepsRawEnumsAndCurrentRuntimePointBuffers)
{
    editor::InspectorValueSnapshot snapshot;
    scene::TrailComponent trail;
    trail.widthEasing = static_cast<decltype(trail.widthEasing)>(99);
    trail.alignment = static_cast<decltype(trail.alignment)>(98);
    trail.uvMode = static_cast<decltype(trail.uvMode)>(97);
    trail.pointBuffer.resize(8);
    const auto* points = trail.pointBuffer.data();
    snapshot.BeginCapture();
    editor::InspectorUndoValues(trail, snapshot);
    snapshot.EndCapture();
    EXPECT_EQ(trail.pointBuffer.data(), points);
    EXPECT_EQ(static_cast<int>(trail.widthEasing), 99);
    EXPECT_EQ(static_cast<int>(trail.alignment), 98);
    EXPECT_EQ(static_cast<int>(trail.uvMode), 97);
    trail.widthEasing = renderer::TrailWidthEasing::Linear;
    trail.pointBuffer.resize(3);
    trail.ringCount = 2;

    snapshot.BeginApply();
    editor::InspectorUndoValues(trail, snapshot);

    EXPECT_EQ(static_cast<int>(trail.widthEasing), 99);
    EXPECT_EQ(trail.pointBuffer.size(), 3u);
    EXPECT_EQ(trail.ringCount, 2);
}

TEST_F(InspectorValueSnapshotTest, NavMeshSurfaceProjectionDoesNotClampCostsOrCaptureBakeResults)
{
    editor::InspectorValueSnapshot snapshot;
    scene::NavMeshSurfaceComponent surface;
    surface.collectObjects = static_cast<decltype(surface.collectObjects)>(99);
    surface.areaCosts[0] = 0.5f;
    surface.bakeDebug.cells.resize(64);
    const auto* cells = surface.bakeDebug.cells.data();
    snapshot.BeginCapture();
    editor::InspectorUndoValues(surface, snapshot);
    snapshot.EndCapture();
    EXPECT_EQ(surface.bakeDebug.cells.data(), cells);
    EXPECT_EQ(static_cast<int>(surface.collectObjects), 99);
    EXPECT_FLOAT_EQ(surface.areaCosts[0], 0.5f);
    EXPECT_FALSE(surface.needsBake);
    surface.collectObjects = scene::NavMeshCollectObjects::ThisObject;
    surface.areaCosts[0] = 1;
    surface.bakeDebug.cells.resize(3);
    surface.needsBake = true;

    snapshot.BeginApply();
    editor::InspectorUndoValues(surface, snapshot);

    EXPECT_EQ(static_cast<int>(surface.collectObjects), 99);
    EXPECT_FLOAT_EQ(surface.areaCosts[0], 0.5f);
    EXPECT_EQ(surface.bakeDebug.cells.size(), 3u);
    EXPECT_TRUE(surface.needsBake);
}

TEST_F(InspectorValueSnapshotTest, PatrolFallbackCapturePreservesUnsignedPointIds)
{
    editor::InspectorValueSnapshot snapshot;
    scene::NavMeshPatrolComponent patrol;
    patrol.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    patrol.waypointBindings.resize(1);
    patrol.waypointBindings[0].id = 0x80000000u;
    patrol.bakedWaypoints.resize(1);
    patrol.bakedWaypoints[0].id = 0x80000001u;

    snapshot.BeginCapture();
    editor::InspectorUndoValues(patrol, snapshot);
    snapshot.EndCapture();

    EXPECT_EQ(patrol.waypointBindings[0].id, 0x80000000u);
    EXPECT_EQ(patrol.bakedWaypoints[0].id, 0x80000001u);
    patrol.waypointBindings[0].id = 1;
    snapshot.BeginApply();
    editor::InspectorUndoValues(patrol, snapshot);
    EXPECT_EQ(patrol.waypointBindings[0].id, 0x80000000u);
}

class InspectorUndoStartTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp(); AttachScene();
        Context().undoStack = &m_undo;
        m_previous = ImGui::GetCurrentContext();
        m_gui = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_gui);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.DisplaySize = {800, 600}; io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr; int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    void TearDown() override
    {
        Context().undoStack = nullptr;
        ImGui::DestroyContext(m_gui); ImGui::SetCurrentContext(m_previous);
        EditorFixture::TearDown();
    }
    void Frame(scene::GameObject& object, bool change, bool end)
    {
        ImGui::NewFrame(); ImGui::Begin("Undo start test");
        auto draw = [change, end](scene::LightComponent& light, editor::EditorContext&) {
            if (change) {
                ImGui::SetActiveID(ImGui::GetID("ValueEdit"), ImGui::GetCurrentWindow());
                light.intensity = 7.5f;
            }
            if (end) ImGui::ClearActiveID();
        };
        editor::DrawGenericUndoableComponentBody(object, Context(), "Light", *object.GetComponent<scene::LightComponent>(),
            &editor::InvokeComponentDraw<scene::LightComponent, decltype(draw)>, &draw);
        ImGui::End(); ImGui::EndFrame();
    }
    template<typename T, typename ChangeFn>
    void SpecializedFrame(scene::GameObject& object, const char* label,
                          bool change, bool end, ChangeFn changeFn)
    {
        ImGui::NewFrame(); ImGui::Begin("Undo specialized test");
        auto draw = [change, end, changeFn](T& component, editor::EditorContext&) {
            if (change) {
                ImGui::SetActiveID(ImGui::GetID("ValueEdit"), ImGui::GetCurrentWindow());
                changeFn(component);
            }
            if (end) ImGui::ClearActiveID();
        };
        editor::DrawUndoableComponentBody(object, Context(), label, *object.GetComponent<T>(), draw);
        ImGui::End(); ImGui::EndFrame();
    }
    editor::UndoStack m_undo;
    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_gui = nullptr;
};

TEST_F(InspectorUndoStartTest, FirstInputMutationRestoresTheValueBeforeUiAndRedoKeepsTheChange)
{
    auto& object = AttachedScene()->CreateGameObject("Light");
    auto& light = object.AddComponent<scene::LightComponent>();
    light.intensity = 3.25f;
    Frame(object, false, false);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    Frame(object, true, false);
    Frame(object, false, true);
    ASSERT_EQ(m_undo.GetHistorySize(), 1u);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_FLOAT_EQ(light.intensity, 3.25f);
    ASSERT_TRUE(m_undo.Redo());
    EXPECT_FLOAT_EQ(light.intensity, 7.5f);
}

TEST_F(InspectorUndoStartTest, MaterialSessionKeepsExtraSlotAssignmentsAcrossUndoAndRedo)
{
    auto& object = AttachedScene()->CreateGameObject("Material");
    auto& material = object.AddComponent<scene::MaterialComponent>();
    material.ResizeSlots(2);
    material.RawSlotAt(1).SetMaterialPath("Assets/Before.mat");
    const auto change = [](scene::MaterialComponent& value) {
        value.RawSlotAt(1).SetMaterialPath("Assets/After.mat");
        value.RawSlotAt(1).visible = false;
    };
    SpecializedFrame<scene::MaterialComponent>(object, "Material", false, false, change);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    SpecializedFrame<scene::MaterialComponent>(object, "Material", true, false, change);
    SpecializedFrame<scene::MaterialComponent>(object, "Material", false, true, change);
    ASSERT_EQ(m_undo.GetHistorySize(), 1u);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(material.RawSlotAt(1).GetMaterialPath(), "Assets/Before.mat");
    EXPECT_TRUE(material.RawSlotAt(1).visible);
    ASSERT_TRUE(m_undo.Redo());
    EXPECT_EQ(material.RawSlotAt(1).GetMaterialPath(), "Assets/After.mat");
    EXPECT_FALSE(material.RawSlotAt(1).visible);
}

TEST_F(InspectorUndoStartTest, RigidBodySessionRestoresTheOwnedBodyWithoutReplacingItsPointer)
{
    auto& object = AttachedScene()->CreateGameObject("Body");
    auto& component = object.AddComponent<scene::RigidBodyComponent>();
    component.rigidBody = std::make_unique<physics::RigidBody>();
    component.rigidBody->SetMass(4);
    component.rigidBody->m_gravityScale = 0.75f;
    const auto* body = component.rigidBody.get();
    const auto change = [](scene::RigidBodyComponent& value) {
        value.rigidBody->SetMass(9);
        value.rigidBody->m_gravityScale = 1.5f;
    };
    SpecializedFrame<scene::RigidBodyComponent>(object, "Body", false, false, change);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    SpecializedFrame<scene::RigidBodyComponent>(object, "Body", true, false, change);
    SpecializedFrame<scene::RigidBodyComponent>(object, "Body", false, true, change);
    ASSERT_EQ(m_undo.GetHistorySize(), 1u);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(component.rigidBody.get(), body);
    EXPECT_FLOAT_EQ(body->GetMass(), 4);
    EXPECT_FLOAT_EQ(body->m_gravityScale, 0.75f);
    ASSERT_TRUE(m_undo.Redo());
    EXPECT_EQ(component.rigidBody.get(), body);
    EXPECT_FLOAT_EQ(body->GetMass(), 9);
    EXPECT_FLOAT_EQ(body->m_gravityScale, 1.5f);
}

TEST_F(InspectorValueSnapshotTest, EditSessionKeepsItsOwnerWhileTheSameInputContinues)
{
    using Transition = editor::InspectorEditTransition;
    editor::InspectorEditSession session;
    const scene::EntityID first{1, 1};
    const scene::EntityID second{2, 1};
    EXPECT_EQ(session.Observe(first, 0, 11), Transition::BEGIN);
    EXPECT_EQ(session.Observe(second, 11, 11), Transition::NONE);
    EXPECT_TRUE(session.Active());
    EXPECT_EQ(session.Observe(first, 11, 0), Transition::END);
    session.Cancel();
    EXPECT_FALSE(session.Active());
    EXPECT_EQ(session.Observe(second, 0, 12), Transition::BEGIN);
    EXPECT_EQ(session.Observe(first, 12, 0), Transition::NONE);
    EXPECT_FALSE(session.Active());
}
} /// @note namespace

TEST_F(InspectorUndoStartTest, RemovedMaterialOwnerRejectsPendingSlotDraft)
{
    auto& object = AttachedScene()->CreateGameObject("MaterialReplacement");
    auto& initial = object.AddComponent<scene::MaterialComponent>();
    initial.SetMaterialPath("Assets/Initial.mat");
    editor::material_edit_detail::SlotTarget target(Context(), object, 0);
    auto edit = initial.BeginAuthoringEdit();
    edit.Draft().materialPath = "Assets/Stale.mat";
    object.RemoveComponent<scene::MaterialComponent>();
    auto& replacement = object.AddComponent<scene::MaterialComponent>();
    replacement.SetMaterialPath("Assets/Replacement.mat");
    EXPECT_EQ(target.Finish(edit), asset::detail::MaterialAuthoringCommitResult::CONFLICT);
    EXPECT_EQ(replacement.GetMaterialPath(), "Assets/Replacement.mat");
}

} /// @note namespace fbzz::tests
