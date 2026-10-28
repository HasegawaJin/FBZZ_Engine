/// @file    ControlPointInspectorTests.cpp
/// @brief   Control-point inspector content, connection status, and headless ImGui frame safety.
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>

#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Tools/ControlPointEditing.hpp>
#include <Editor/Tools/TargetPointReferenceEditing.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/ClothComponent.hpp>
#include <Engine/Scene/Components/ConstraintComponents.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/MotionWarpComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/VFXBeamComponent.hpp>
#include <Engine/Scene/Components/VFXLineComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <cstdint>
#include <string>

namespace fbzz::tests {
namespace {

class ControlPointInspectorTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        (void)AttachScene();
        Context().operators = &m_registry;
        Context().undoStack = &m_undo;
        editor::RegisterControlPointOperators(m_registry);
        editor::RegisterClothOperators(m_registry);
        editor::RegisterTargetPointReferenceOperators(m_registry);
        editor::RegisterTargetPointMoveOperator(m_registry);

        m_previousContext = ImGui::GetCurrentContext();
        m_imgui = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_imgui);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1800.0f, 8000.0f};
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    void TearDown() override
    {
        m_play.Cancel();
        Context().playMode = nullptr;
        Context().operators = nullptr;
        Context().undoStack = nullptr;
        Context().controlPointOwnerGuid.clear();
        m_undo.Clear();
        ImGui::DestroyContext(m_imgui);
        ImGui::SetCurrentContext(m_previousContext);
        EditorFixture::TearDown();
    }

    std::string Draw(scene::GameObject& object, const char* activateScope = nullptr,
        const char* activateLabel = nullptr, bool* handled = nullptr)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.0f, 0.0f});
        ImGui::SetNextWindowSize({1800.0f, 8000.0f});
        ImGui::Begin("Control-point inspector test",
            nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        if (activateScope && activateLabel) {
            ImGui::PushID(activateScope);
            const ImGuiID id = ImGui::GetID(activateLabel);
            ImGui::PopID();
            m_imgui->NavActivateId = id;
            m_imgui->NavActivateDownId = id;
            m_imgui->NavActivatePressedId = id;
            m_imgui->NavActivateFlags = ImGuiActivateFlags_None;
        }
        ImGui::LogToBuffer();
        const bool didHandle = editor::DrawControlPointInspector(Context(), object);
        const std::string text = m_imgui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::EndFrame();
        if (handled) *handled = didHandle;
        EXPECT_FALSE(m_imgui->WithinFrameScope);
        EXPECT_EQ(m_imgui->CurrentWindowStack.Size, 0);
        return text;
    }

    scene::GameObject& MakeMarker(const char* name, const math::Vector3& position)
    {
        auto& marker = AttachedScene()->CreateGameObject(name);
        marker.transform.position = position;
        marker.AddComponent<scene::TargetPointComponent>();
        return marker;
    }

    void AddWaterBinding(scene::WaterComponent& water, scene::GameObject& marker,
        std::uint32_t id, float width = 2.0f, float depth = 3.0f)
    {
        scene::WaterControlPointBinding binding;
        binding.id = id;
        binding.target.id = marker.GetID();
        binding.target.guid = marker.instanceId;
        binding.target.sceneGeneration = AttachedScene()->GetRenderSceneGeneration();
        binding.width = width;
        binding.depth = depth;
        water.pointBindings.push_back(binding);
    }

    static void ExpectContains(const std::string& text, const char* fragment)
    {
        EXPECT_NE(text.find(fragment), std::string::npos) << text;
    }

    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    editor::PlayModeController m_play;
    ImGuiContext* m_previousContext = nullptr;
    ImGuiContext* m_imgui = nullptr;
};

TEST_F(ControlPointInspectorTest, OrdinaryGameObjectDoesNotShowAnEmptyPointPanel)
{
    auto& object = AttachedScene()->CreateGameObject("Ordinary");
    bool handled = true;
    const std::string text = Draw(object, nullptr, nullptr, &handled);

    EXPECT_FALSE(handled);
    EXPECT_EQ(text.find("制御点"), std::string::npos) << text;
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(ControlPointInspectorTest, WaterRectangleExplainsSplineWorkflowAndOnlyPointObjectsCanEnterPointEdit)
{
    auto& owner = AttachedScene()->CreateGameObject("Water Owner");
    auto& water = owner.AddComponent<scene::WaterComponent>();
    water.surfaceShape = scene::WaterSurfaceShape::RECTANGLE;
    const std::size_t originalObjectCount = AttachedScene()->GameObjectCount();
    bool handled = false;
    const std::string rectangleText = Draw(owner, nullptr, nullptr, &handled);
    EXPECT_FALSE(handled);
    ExpectContains(rectangleText, "Rectangleの水面");
    ExpectContains(rectangleText, "Spline LakeまたはSpline River");
    EXPECT_EQ(rectangleText.find("点を編集"), std::string::npos) << rectangleText;

    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.splinePoints = {
        {1, {-2.0f, 0.0f, 0.0f}, 2.0f, 3.0f, 0.0f},
        {2, {2.0f, 0.0f, 0.0f}, 2.0f, 3.0f, 0.0f},
    };
    scene::FlushWorldTransforms(*AttachedScene());
    const std::string inlineText = Draw(owner, "water", "点を編集", &handled);
    EXPECT_FALSE(handled);
    ExpectContains(inlineText, "数値点");
    EXPECT_TRUE(Context().controlPointOwnerGuid.empty());

    auto& first = MakeMarker("River Point A", {-2.0f, 0.0f, 0.0f});
    auto& second = MakeMarker("River Point B", {2.0f, 0.0f, 0.0f});
    water.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    AddWaterBinding(water, first, 1);
    AddWaterBinding(water, second, 2);
    scene::FlushWorldTransforms(*AttachedScene());
    const auto beforeBindings = water.pointBindings;
    const std::size_t beforeHistory = m_undo.GetHistorySize();
    const std::string objectText = Draw(owner, "water", "点を編集", &handled);
    EXPECT_TRUE(handled);
    ExpectContains(objectText, "TargetPoint");
    EXPECT_EQ(Context().controlPointOwnerGuid, owner.instanceId);
    EXPECT_EQ(AttachedScene()->GameObjectCount(), originalObjectCount + 2);
    EXPECT_EQ(water.pointBindings, beforeBindings);
    EXPECT_EQ(m_undo.GetHistorySize(), beforeHistory);
}

TEST_F(ControlPointInspectorTest, TargetPointShowsAndConfirmsItsConnectedConsumer)
{
    auto& marker = MakeMarker("Camera Target", {1.0f, 2.0f, 3.0f});
    auto& camera = AttachedScene()->CreateGameObject("Camera Rig");
    auto& follow = camera.AddComponent<scene::CameraFollowComponent>();
    follow.target = scene::EntityRef{marker.GetID(), marker.instanceId,
        AttachedScene()->GetRenderSceneGeneration()};

    const auto originalObjectCount = AttachedScene()->GameObjectCount();
    bool handled = false;
    const std::string before = Draw(marker, nullptr, nullptr, &handled);
    EXPECT_FALSE(handled);
    ExpectContains(before, "接続先を確認");
    ExpectContains(before, "参照して初めて接続されます");

    const std::size_t history = m_undo.GetHistorySize();
    const std::string clicked = Draw(marker, "MarkerConnections", "接続先を確認", &handled);
    EXPECT_TRUE(handled);
    const std::string confirmed = Draw(marker, nullptr, nullptr, &handled);
    ExpectContains(confirmed, "参照を持つオブジェクト");
    ExpectContains(confirmed, "Camera Rig");
    EXPECT_EQ(AttachedScene()->GameObjectCount(), originalObjectCount);
    EXPECT_EQ(m_undo.GetHistorySize(), history);
    ExpectContains(clicked, "接続先を確認");
}

TEST_F(ControlPointInspectorTest, ClothShowsTheFixedTargetCreationAndConnectionPath)
{
    auto& owner = AttachedScene()->CreateGameObject("Cloth Owner");
    auto& cloth = owner.AddComponent<scene::ClothComponent>();
    cloth.segments = 2;
    bool handled = false;
    const std::string before = Draw(owner, nullptr, nullptr, &handled);

    EXPECT_FALSE(handled);
    ExpectContains(before, "Cloth 固定先");
    ExpectContains(before, "Cloth 揺れ方");
    ExpectContains(before, "曲がりやすさ");
    ExpectContains(before, "揺れ方を適用");
    ExpectContains(before, "固定点を画面で編集");
    ExpectContains(before, "TargetPointを作成して固定");
    ExpectContains(before, "既存のTargetPoint、Bone、通常のGameObject");
    ExpectContains(before, "すべての物理質点を子オブジェクトにする必要はありません");
    EXPECT_TRUE(cloth.attachments.empty());
    EXPECT_EQ(AttachedScene()->GameObjectCount(), 1u);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);

    const std::string clicked = Draw(owner, "cloth", "TargetPointを作成して固定", &handled);
    ASSERT_TRUE(handled) << clicked;
    ASSERT_EQ(owner.GetChildCount(), 1);
    ASSERT_EQ(AttachedScene()->GameObjectCount(), 2u);
    ASSERT_EQ(m_undo.GetHistorySize(), 1u);
    ASSERT_EQ(m_undo.GetCursor(), 1u);

    auto* marker = owner.GetChild(0);
    ASSERT_NE(marker, nullptr);
    EXPECT_NE(marker->GetComponent<scene::TargetPointComponent>(), nullptr);
    auto* boundCloth = owner.GetComponent<scene::ClothComponent>();
    ASSERT_NE(boundCloth, nullptr);
    ASSERT_EQ(boundCloth->attachments.size(), 1u);
    EXPECT_EQ(boundCloth->attachments.front().particle, 0);
    EXPECT_EQ(boundCloth->attachments.front().targetReference.guid, marker->instanceId);
    EXPECT_NE(boundCloth->attachments.front().topologySignature, 0u);

    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_TRUE(owner.GetComponent<scene::ClothComponent>()->attachments.empty());
    EXPECT_EQ(owner.GetChildCount(), 0);
    EXPECT_EQ(AttachedScene()->GameObjectCount(), 1u);
    EXPECT_EQ(m_undo.GetCursor(), 0u);
}

TEST_F(ControlPointInspectorTest, ClothMotionDraftAppliesOnceAndUndoResynchronizesThePanel)
{
    auto& owner = AttachedScene()->CreateGameObject("Adjustable Cloth");
    auto& cloth = owner.AddComponent<scene::ClothComponent>();
    cloth.settings.bendCompliance = 0.02f;
    cloth.settings.damping = 7.0f;
    cloth.settings.windVelocity = {0.0f, 0.0f, 5.0f};
    bool handled = false;
    Draw(owner);
    Draw(owner, "clothMotion", "標準値を下書きに戻す", &handled);
    EXPECT_FALSE(handled);
    EXPECT_FLOAT_EQ(cloth.settings.bendCompliance, 0.02f);
    EXPECT_FLOAT_EQ(cloth.settings.damping, 7.0f);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    Draw(owner, "clothMotion", "揺れ方を適用", &handled);
    EXPECT_TRUE(handled);
    EXPECT_FLOAT_EQ(cloth.settings.bendCompliance, physics::ClothSettings{}.bendCompliance);
    EXPECT_FLOAT_EQ(cloth.settings.damping, physics::ClothSettings{}.damping);
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    Draw(owner, "clothMotion", "揺れ方を適用", &handled);
    EXPECT_FALSE(handled);
    EXPECT_FLOAT_EQ(cloth.settings.bendCompliance, 0.02f);
    EXPECT_FLOAT_EQ(cloth.settings.damping, 7.0f);
    EXPECT_EQ(m_undo.GetCursor(), 0u);
}

TEST_F(ControlPointInspectorTest, ReferenceAndAdditionalConsumersRenderTheirAuthoringSections)
{
    auto& owner = AttachedScene()->CreateGameObject("Consumers");
    owner.AddComponent<scene::CameraFollowComponent>();
    owner.AddComponent<scene::TransformConstraintComponent>();
    owner.AddComponent<scene::VFXBeamComponent>();
    owner.AddComponent<scene::VFXLineComponent>();
    owner.AddComponent<scene::NavMeshAgentComponent>();
    owner.AddComponent<scene::IKSolverComponent>().chains.push_back(scene::IKChain{});
    owner.AddComponent<scene::MotionWarpComponent>();
    owner.AddComponent<scene::LODGroupComponent>();
    owner.AddComponent<scene::NavMeshPatrolComponent>();
    owner.AddComponent<scene::LineRendererComponent>();
    owner.AddComponent<scene::NavMeshOffMeshLinkComponent>();
    bool handled = false;
    const std::string text = Draw(owner, nullptr, nullptr, &handled);

    EXPECT_FALSE(handled);
    for (const char* section : {
        "Camera追従先", "Transform制約の目標", "Beam始点", "Beam終点", "VFX始点", "VFX終点",
        "追跡対象", "IK目標", "IK Pole", "MotionWarp目標", "LOD距離判定の中心",
        "Line Renderer", "NavMesh巡回経路", "NavMesh Off-Mesh Link"}) ExpectContains(text, section);

    auto& anchorOwner = AttachedScene()->CreateGameObject("Anchors");
    anchorOwner.AddComponent<scene::LightComponent>();
    anchorOwner.AddComponent<scene::FlowField>();
    const std::string anchorText = Draw(anchorOwner, nullptr, nullptr, &handled);
    EXPECT_FALSE(handled);
    ExpectContains(anchorText, "Lightの配置基準");
    ExpectContains(anchorText, "FlowFieldの配置基準");
    ExpectContains(anchorText, "TransformConstraintのTarget");
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(ControlPointInspectorTest, PlayDisablesPointEditingAndLeavesTheImGuiFrameBalanced)
{
    auto& owner = AttachedScene()->CreateGameObject("Live Water");
    auto& water = owner.AddComponent<scene::WaterComponent>();
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    auto& first = MakeMarker("Point A", {-2.0f, 0.0f, 0.0f});
    auto& second = MakeMarker("Point B", {2.0f, 0.0f, 0.0f});
    AddWaterBinding(water, first, 1);
    AddWaterBinding(water, second, 2);
    scene::FlushWorldTransforms(*AttachedScene());
    const auto beforeBindings = water.pointBindings;
    const auto beforeObjects = AttachedScene()->GameObjectCount();

    m_play.Play(*AttachedScene());
    ASSERT_TRUE(m_play.IsPlaying()) << m_play.Diagnostic();
    Context().playMode = &m_play;
    bool handled = true;
    const std::string text = Draw(owner, "water", "点を編集", &handled);

    EXPECT_FALSE(handled);
    ExpectContains(text, "Play");
    EXPECT_TRUE(Context().controlPointOwnerGuid.empty());
    EXPECT_EQ(AttachedScene()->GameObjectCount(), beforeObjects);
    EXPECT_EQ(water.pointBindings, beforeBindings);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    EXPECT_FALSE(m_imgui->WithinFrameScope);
    EXPECT_EQ(m_imgui->CurrentWindowStack.Size, 0);
}

} /// @note namespace
} /// @note namespace fbzz::tests
