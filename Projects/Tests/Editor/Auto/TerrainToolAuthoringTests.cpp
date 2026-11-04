/// @file    TerrainToolAuthoringTests.cpp
/// @brief   地形制作パネルの対象選択を実際の ImGui 操作から検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Tools/TerrainTool.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>

namespace fbzz::tests {
namespace {

class TerrainToolAuthoringTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        AttachScene();
        Context().undoStack = &m_undo;
        Context().markSceneDirty = [] {};
        m_previous = ImGui::GetCurrentContext();
        m_imgui = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = { 1000, 1400 };
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    void TearDown() override
    {
        m_undo.Clear();
        Context().undoStack = nullptr;
        ImGui::DestroyContext(m_imgui);
        ImGui::SetCurrentContext(m_previous);
        EditorFixture::TearDown();
    }

    scene::EntityID AddTerrain(const char* name, int columns, int rows)
    {
        auto& object = AttachedScene()->CreateGameObject(name);
        auto& terrain = object.AddComponent<scene::TerrainComponent>();
        terrain.Resize(columns, rows);
        terrain.cellSize = 4.0f;
        terrain.maxHeight = 640.0f;
        return object.GetID();
    }

    void Frame(bool openTarget = false, scene::EntityID choose = scene::EntityID::INVALID)
    {
        ImGuiWindow* popup = !m_imgui->OpenPopupStack.empty()
            ? m_imgui->OpenPopupStack.back().Window : nullptr;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 1000, 1400 });
        ImGui::Begin("Terrain authoring test", nullptr,
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        ImGuiID activate = 0;
        if (openTarget) {
            ImGui::PushID("TerrainAuthoring");
            activate = ImGui::GetID("##HeightMapTarget");
            ImGui::PopID();
        } else if (popup && AttachedScene()->IsValid(choose)) {
            const auto* object = AttachedScene()->GetGameObject(choose);
            const std::string label = object->name + " [" + object->instanceId.substr(0, 8) + "]";
            const ImGuiID seed = ImHashStr(object->instanceId.c_str(), 0, popup->IDStack.back());
            activate = ImHashStr(label.c_str(), 0, seed);
        }
        if (activate) {
            m_imgui->NavActivateId = activate;
            m_imgui->NavActivateDownId = activate;
            m_imgui->NavActivatePressedId = activate;
            m_imgui->NavActivateFlags = ImGuiActivateFlags_None;
        }
        ImGui::LogToBuffer();
        m_tool.DrawAuthoringSection(Context());
        m_text = m_imgui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
    }

    void Choose(scene::EntityID id)
    {
        Frame(true);
        ASSERT_FALSE(m_imgui->OpenPopupStack.empty()) << m_text;
        ASSERT_NE(m_imgui->OpenPopupStack.back().Window, nullptr);
        Frame(false, id);
        Frame();
    }

    void BrushFrame(bool held)
    {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(500.0f, 700.0f);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, held);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 1000, 1400 });
        ImGui::Begin("Terrain stroke test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        renderer::Camera camera;
        camera.m_position = { 8.0f, 50.0f, -20.0f };
        camera.m_aspect = 1000.0f / 1400.0f;
        camera.LookAt({ 8.0f, 0.0f, 8.0f });
        m_tool.Update(Context(), camera, 1.0f, true, { 0, 0 }, { 1000, 1400 });
        ImGui::End();
        ImGui::Render();
    }

    void Stroke(editor::TerrainTool::Mode mode)
    {
        m_tool.SetActive(true);
        m_tool.SetMode(mode);
        m_tool.SetPaintLayer(1);
        m_tool.SetBrush(5.0f, 1.0f, editor::TerrainFalloff::Linear);
        BrushFrame(false);
        BrushFrame(true);
        BrushFrame(false);
        ASSERT_EQ(m_undo.GetHistorySize(), 1u);
    }

    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_imgui = nullptr;
    editor::TerrainTool m_tool;
    editor::UndoStack m_undo;
    std::string m_text;
};

TEST_F(TerrainToolAuthoringTest, MultipleTerrainsRequireAnExplicitChoice)
{
    AddTerrain("First", 3, 4);
    AddTerrain("Second", 5, 6);
    Frame();
    EXPECT_NE(m_text.find("Choose the terrain to edit."), std::string::npos) << m_text;
    EXPECT_EQ(m_text.find("Resolution:"), std::string::npos) << m_text;
}

TEST_F(TerrainToolAuthoringTest, OneTerrainShowsItsDimensionsWithoutAnExtraChoice)
{
    AddTerrain("Mountain", 5, 6);
    Frame();
    EXPECT_NE(m_text.find("Resolution: 5 x 6"), std::string::npos) << m_text;
    EXPECT_NE(m_text.find("Cell Size: 4.000 m"), std::string::npos) << m_text;
    EXPECT_NE(m_text.find("Max Height: 640.0 m"), std::string::npos) << m_text;
    EXPECT_NE(m_text.find("Local Size: 16.0 x 20.0 m"), std::string::npos) << m_text;
}

TEST_F(TerrainToolAuthoringTest, ExplicitChoiceSurvivesHierarchyReordering)
{
    AddTerrain("First", 3, 4);
    const auto second = AddTerrain("Second", 5, 6);
    Frame();
    Choose(second);
    ASSERT_NE(m_text.find("Resolution: 5 x 6"), std::string::npos) << m_text;
    ASSERT_TRUE(AttachedScene()->MoveGameObjectToIndex(second, 0));
    Frame();
    EXPECT_NE(m_text.find("Resolution: 5 x 6"), std::string::npos) << m_text;
}

TEST_F(TerrainToolAuthoringTest, DeletedChoiceDoesNotSelectTheRemainingTerrain)
{
    AddTerrain("First", 3, 4);
    const auto second = AddTerrain("Second", 5, 6);
    Frame();
    Choose(second);
    ASSERT_TRUE(AttachedScene()->DestroyGameObject(second));
    Frame();
    EXPECT_NE(m_text.find("Choose the terrain to edit."), std::string::npos) << m_text;
    EXPECT_EQ(m_text.find("Resolution:"), std::string::npos) << m_text;
}

TEST_F(TerrainToolAuthoringTest, SceneClearResetsAnEarlierChoice)
{
    AddTerrain("Original", 3, 4);
    Frame();
    AttachedScene()->Clear();
    AddTerrain("New first", 5, 6);
    AddTerrain("New second", 7, 8);
    Frame();
    EXPECT_NE(m_text.find("Choose the terrain to edit."), std::string::npos) << m_text;
    EXPECT_EQ(m_text.find("Resolution:"), std::string::npos) << m_text;
}

TEST_F(TerrainToolAuthoringTest, PaintUndoPreservesGeometryRevisionsAndSavedReference)
{
    const auto id = AddTerrain("Painted terrain", 5, 5);
    auto& terrain = *AttachedScene()->GetComponent<scene::TerrainComponent>(id);
    const auto originalWeights = terrain.splatWeights;
    Stroke(editor::TerrainTool::Mode::Paint);
    const auto paintedWeights = terrain.splatWeights;
    ASSERT_NE(paintedWeights, originalWeights);
    terrain.surfaceRevision = 50;
    terrain.vegetationRevision = 60;
    terrain.heightDirty = terrain.colliderDirty = terrain.splatDirty = false;
    terrain.terrainAssetPath = "guid:saved-after-stroke";
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(terrain.splatWeights, originalWeights);
    EXPECT_EQ(terrain.surfaceRevision, 50u);
    EXPECT_EQ(terrain.vegetationRevision, 60u);
    EXPECT_FALSE(terrain.heightDirty);
    EXPECT_FALSE(terrain.colliderDirty);
    EXPECT_TRUE(terrain.splatDirty);
    EXPECT_EQ(terrain.terrainAssetPath, "guid:saved-after-stroke");
    terrain.vegetationRevision = 61;
    terrain.splatDirty = false;
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(terrain.splatWeights, paintedWeights);
    EXPECT_EQ(terrain.surfaceRevision, 50u);
    EXPECT_EQ(terrain.vegetationRevision, 61u);
    EXPECT_FALSE(terrain.heightDirty);
    EXPECT_FALSE(terrain.colliderDirty);
    EXPECT_TRUE(terrain.splatDirty);
}

TEST_F(TerrainToolAuthoringTest, SculptUndoInvalidatesGeometryWithANewRevision)
{
    const auto id = AddTerrain("Sculpted terrain", 5, 5);
    auto& terrain = *AttachedScene()->GetComponent<scene::TerrainComponent>(id);
    const auto originalHeight = terrain.heightData;
    Stroke(editor::TerrainTool::Mode::Sculpt);
    ASSERT_NE(terrain.heightData, originalHeight);
    terrain.surfaceRevision = 100;
    terrain.heightDirty = terrain.colliderDirty = false;
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(terrain.heightData, originalHeight);
    EXPECT_EQ(terrain.surfaceRevision, 101u);
    EXPECT_TRUE(terrain.heightDirty);
    EXPECT_TRUE(terrain.colliderDirty);
}

TEST_F(TerrainToolAuthoringTest, StrokeUndoRejectsAReplacedComponent)
{
    const auto id = AddTerrain("Painted terrain", 5, 5);
    Stroke(editor::TerrainTool::Mode::Paint);
    auto* object = AttachedScene()->GetGameObject(id);
    object->RemoveComponent<scene::TerrainComponent>();
    auto& replacement = object->AddComponent<scene::TerrainComponent>();
    replacement.InitFlat(0.25f);
    const auto height = replacement.heightData;
    EXPECT_FALSE(m_undo.Undo());
    EXPECT_EQ(m_undo.GetCursor(), 1u);
    EXPECT_EQ(replacement.heightData, height);
}

TEST_F(TerrainToolAuthoringTest, StrokeUndoRejectsAuthoredConflictsAndDetachedScene)
{
    const auto id = AddTerrain("Painted terrain", 5, 5);
    Stroke(editor::TerrainTool::Mode::Paint);
    Context().activeScene = nullptr;
    EXPECT_FALSE(m_undo.Undo());
    EXPECT_EQ(m_undo.GetCursor(), 1u);
    Context().activeScene = AttachedScene();
    auto& terrain = *AttachedScene()->GetComponent<scene::TerrainComponent>(id);
    terrain.heightData[0] = 0.25f;
    const auto weights = terrain.splatWeights;
    EXPECT_FALSE(m_undo.Undo());
    EXPECT_EQ(m_undo.GetCursor(), 1u);
    EXPECT_FLOAT_EQ(terrain.heightData[0], 0.25f);
    EXPECT_EQ(terrain.splatWeights, weights);
}

TEST_F(TerrainToolAuthoringTest, SceneClearDuringStrokeDoesNotRecordAnEditOnTheNewScene)
{
    AddTerrain("Original terrain", 5, 5);
    m_tool.SetActive(true);
    m_tool.SetMode(editor::TerrainTool::Mode::Paint);
    m_tool.SetPaintLayer(1);
    BrushFrame(false);
    BrushFrame(true);
    AttachedScene()->Clear();
    const auto id = AddTerrain("New terrain", 5, 5);
    auto& terrain = *AttachedScene()->GetComponent<scene::TerrainComponent>(id);
    const auto weights = terrain.splatWeights;
    BrushFrame(false);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    EXPECT_EQ(terrain.splatWeights, weights);
}

} /// @note namespace
} /// @note namespace fbzz::tests
