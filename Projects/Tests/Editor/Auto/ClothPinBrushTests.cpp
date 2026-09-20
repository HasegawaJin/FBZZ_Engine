/// @file    ClothPinBrushTests.cpp
/// @brief   布ブラシの確定、中断と Operator の Undo 契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Tools/ClothPinBrush.hpp>
#include <Engine/Scene/Components/ClothComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Asset/ClothAsset.hpp>
#include <Engine/Scene/Systems/ClothSystem.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Physics/World.hpp>
#include <imgui.h>

namespace fbzz::tests {
class ClothPinBrushTest : public testkit::EditorFixture {
protected:
    asset::Model m_exportModel;
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    scene::EntityID m_id;
    renderer::Camera m_camera;
    editor::ClothPinBrush m_brush;
    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_imgui = nullptr;
    void SetUp() override
    {
        EditorFixture::SetUp();
        auto& scene = AttachScene();
        auto& go = scene.CreateGameObject("Cloth");
        m_id = go.GetID();
        auto& cloth = go.AddComponent<scene::ClothComponent>();
        cloth.segments = 2;
        physics::World world;
        SystemContext systemContext{scene, world, nullptr, nullptr, 1.0f/60, 1.0f/60, true, true};
        scene::ClothSystem{}.Update(systemContext);
        ASSERT_TRUE(cloth.runtime.initialized);
        Context().selectedEntities = {m_id};
        Context().operators = &m_registry;
        Context().undoStack = &m_undo;
        Context().editorCamera = &m_camera;
        editor::RegisterClothOperators(m_registry);
        m_previous = ImGui::GetCurrentContext();
        m_imgui = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {800,600};
        io.DeltaTime = 1.0f/60;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        m_camera.m_projection = renderer::ProjectionMode::Orthographic;
        m_camera.m_orthoHeight = 6;
        m_camera.m_aspect = 800.0f/600;
    }
    void TearDown() override
    {
        m_undo.Clear();
        ImGui::DestroyContext(m_imgui);
        ImGui::SetCurrentContext(m_previous);
        EditorFixture::TearDown();
    }
    scene::ClothComponent& Cloth() { return *AttachedScene()->GetComponent<scene::ClothComponent>(m_id); }
    editor::OpResult Paint(const char* particles, bool pin = true)
    {
        editor::OpArgs args;
        args.Set("particles", std::string(particles));
        args.Set("pin", pin);
        return editor::InvokeOperator(Context(), "cloth.paint_pins", args);
    }
    void Frame(bool down, bool editable = true)
    {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(400,400);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0,0});
        ImGui::SetNextWindowSize({800,600});
        ImGui::Begin("Cloth test", nullptr, ImGuiWindowFlags_NoTitleBar);
        editor::DrawClothPinBrush(Context(), m_brush, {0,0}, {800,600}, true, editable);
        ImGui::End();
        ImGui::Render();
    }
};

TEST_F(ClothPinBrushTest, ExportSkinnedSubmeshTransfersWeightsAndRejectsInvalidSlot)
{
    Context().projectRoot = ProjectRoot().generic_string();
    m_exportModel.skeleton = std::make_unique<asset::Skeleton>();
    m_exportModel.skeleton->bones = {{"Bone", 0, math::Matrix4::Identity()}};
    m_exportModel.skeleton->referencePose = {math::Matrix4::Identity()};
    m_exportModel.meshes.push_back(std::make_unique<renderer::Mesh>());
    auto mesh = std::make_unique<renderer::Mesh>();
    mesh->isSkinned = true;
    const math::Vector3 positions[]{{0,0,0}, {1,0,0}, {0,1,0}};
    for (const auto& p : positions) {
        renderer::SkinnedVertex vertex;
        vertex.position = p;
        vertex.normal = {0,0,1};
        vertex.tangent = {1,0,0};
        vertex.boneWeights[0] = 1.0f;
        mesh->cpuSkinnedVertices.push_back(vertex);
    }
    mesh->cpuIndices = {0,1,2};
    m_exportModel.meshes.push_back(std::move(mesh));
    auto& owner = AttachedScene()->CreateGameObject("Skin");
    auto& skin = owner.AddComponent<scene::SkinnedMeshRenderer>();
    skin.model = &m_exportModel;
    skin.submeshIndices = {1};
    Context().selectedEntities = {owner.GetID()};
    editor::OpArgs args;
    args.Set("submesh", 99);
    EXPECT_FALSE(editor::InvokeOperator(Context(), "cloth.export_mesh", args).ok);
    args.Set("submesh", 0);
    const auto result = editor::InvokeOperator(Context(), "cloth.export_mesh", args);
    ASSERT_TRUE(result.ok) << result.message;
    asset::ClothAsset loaded;
    ASSERT_TRUE(asset::LoadClothAssetFromFile(Context().requestRevealAssetPath, loaded));
    ASSERT_EQ(loaded.skinBones.size(), 1u);
    EXPECT_EQ(loaded.skinBones[0].name, "Bone");
    EXPECT_EQ(loaded.skinWeights.size(), 3u);
    EXPECT_FLOAT_EQ(loaded.skinWeights[0].weights[0], 1.0f);
    const std::string lowPath = Context().requestRevealAssetPath;
    args.Set("simulationAsset",lowPath);
    args.Set("maxBindDistance",0.1f);
    const auto boundResult = editor::InvokeOperator(Context(),"cloth.export_mesh",args);
    ASSERT_TRUE(boundResult.ok) << boundResult.message;
    EXPECT_NE(Context().requestRevealAssetPath,lowPath);
    ASSERT_TRUE(asset::LoadClothAssetFromFile(Context().requestRevealAssetPath,loaded));
    EXPECT_EQ(loaded.renderBindings.size(),3u);
    EXPECT_EQ(loaded.simulationIndices,(std::vector<uint32_t>{0,1,2}));
}

TEST_F(ClothPinBrushTest, StrokeUsesOneUndoAndRestoresInheritedPins)
{
    ASSERT_TRUE(Paint("4 4 8").ok);
    EXPECT_EQ(Cloth().pinnedParticles, (std::vector<int>{0,1,2,4,8}));
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    Cloth().materialPath = "custom.mat";
    m_undo.Undo();
    EXPECT_FALSE(Cloth().overridePins);
    EXPECT_TRUE(Cloth().pinnedParticles.empty());
    EXPECT_EQ(Cloth().materialPath, "custom.mat");
    m_undo.Redo();
    EXPECT_TRUE(Cloth().overridePins);
    EXPECT_EQ(Cloth().pinnedParticles, (std::vector<int>{0,1,2,4,8}));
}

TEST_F(ClothPinBrushTest, ReleaseUsesCurrentAuthoringBeforeRuntimeRebuild)
{
    ASSERT_TRUE(Paint("4").ok);
    ASSERT_TRUE(Paint("0 4", false).ok);
    EXPECT_EQ(Cloth().pinnedParticles, (std::vector<int>{1,2}));
    m_undo.Undo();
    EXPECT_EQ(Cloth().pinnedParticles, (std::vector<int>{0,1,2,4}));
}

TEST_F(ClothPinBrushTest, InvalidInputDoesNotPartiallyApply)
{
    for (const char* text : {"4 -1", "4 9", "4 xyz", "4 2x", "9999999999999999999999999"}) {
        EXPECT_FALSE(Paint(text).ok);
        EXPECT_FALSE(Cloth().overridePins);
        EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    }
}

TEST_F(ClothPinBrushTest, NoChangeKeepsInheritanceAndUndoEmpty)
{
    EXPECT_TRUE(Paint("0 1 2").noChange);
    EXPECT_TRUE(Paint("").noChange);
    EXPECT_FALSE(Cloth().overridePins);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(ClothPinBrushTest, TopologyEditRequiresReinitialization)
{
    Cloth().segments = 4;
    EXPECT_FALSE(Paint("4").ok);
    Cloth().segments = 2;
    Cloth().pinTop = false;
    EXPECT_FALSE(Paint("4").ok);
    EXPECT_FALSE(Cloth().overridePins);
}

TEST_F(ClothPinBrushTest, SceneSwitchDoesNotApplyUndoToAnotherScene)
{
    ASSERT_TRUE(Paint("4").ok);
    DetachScene();
    m_undo.Undo();
    EXPECT_TRUE(Cloth().overridePins);
}

TEST_F(ClothPinBrushTest, MouseReleaseCommitsPreviewOnce)
{
    Context().clothPinPainting = true;
    Frame(false);
    Frame(true);
    ASSERT_TRUE(m_brush.dragging);
    EXPECT_FALSE(Cloth().overridePins);
    Frame(true);
    Frame(false);
    EXPECT_FALSE(m_brush.dragging);
    EXPECT_EQ(Cloth().pinnedParticles, (std::vector<int>{0,1,2,4}));
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
}

TEST_F(ClothPinBrushTest, PlayTransitionCancelsPreview)
{
    Context().clothPinPainting = true;
    Frame(false);
    Frame(true);
    ASSERT_TRUE(m_brush.dragging);
    Frame(true, false);
    Frame(false, false);
    EXPECT_FALSE(m_brush.dragging);
    EXPECT_FALSE(Cloth().overridePins);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}
TEST_F(ClothPinBrushTest, DistanceStrokeCommitsOnceAndCancelsOnSettingChange)
{
    Context().clothPinPainting = true;
    m_brush.distanceMode = true;
    m_brush.distance = 0.3f;
    Frame(false); Frame(true);
    ASSERT_TRUE(m_brush.dragging);
    EXPECT_TRUE(Cloth().maxDistances.empty());
    Frame(false);
    ASSERT_EQ(Cloth().maxDistances.size(),1u);
    EXPECT_EQ(Cloth().maxDistances[0].particle,4);
    EXPECT_FLOAT_EQ(Cloth().maxDistances[0].distance,0.3f);
    EXPECT_EQ(m_undo.GetHistorySize(),1u);
    m_undo.Undo();
    EXPECT_TRUE(Cloth().maxDistances.empty());
    Frame(true);
    Cloth().skinMaxDistance = 0.5f;
    Frame(false);
    EXPECT_TRUE(Cloth().maxDistances.empty());
}
TEST_F(ClothPinBrushTest, DistanceOperatorRestoresInheritanceAndRejectsPartialInvalidStroke)
{
    editor::OpArgs args;
    args.Set("particles",std::string("4 8 4")); args.Set("distance",0.2f);
    ASSERT_TRUE(editor::InvokeOperator(Context(),"cloth.paint_max_distance",args).ok);
    const auto before = Cloth().maxDistances;
    EXPECT_EQ(before.size(),2u);
    EXPECT_TRUE(editor::InvokeOperator(Context(),"cloth.paint_max_distance",args).noChange);
    args.Set("particles",std::string("0 9"));
    EXPECT_FALSE(editor::InvokeOperator(Context(),"cloth.paint_max_distance",args).ok);
    EXPECT_EQ(Cloth().maxDistances,before);
    args.Set("particles",std::string("4")); args.Set("inherit",true);
    ASSERT_TRUE(editor::InvokeOperator(Context(),"cloth.paint_max_distance",args).ok);
    ASSERT_EQ(Cloth().maxDistances.size(),1u);
    EXPECT_EQ(Cloth().maxDistances[0].particle,8);
    m_undo.Undo(); EXPECT_EQ(Cloth().maxDistances,before);
    m_undo.Redo(); EXPECT_EQ(Cloth().maxDistances.size(),1u);
}
}
