/// @file    TerrainToolAuthoringTests.cpp
/// @brief   地形制作パネルの対象選択を実際の ImGui 操作から検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Tools/TerrainTool.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/TerrainHeightMapLoader.hpp>
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
        Context().operators = &m_registry;
        editor::RegisterTerrainOperators(m_registry);
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
        Context().operators = nullptr;
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

    void BrushFrame(bool held, const math::Vector3& aim = {8.0f, 0.0f, 8.0f})
    {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(500.0f, 700.0f);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, held);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 1000, 1400 });
        ImGui::Begin("Terrain stroke test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        renderer::Camera camera;
        camera.m_position = aim + math::Vector3{0.0f, 50.0f, -28.0f};
        camera.m_aspect = 1000.0f / 1400.0f;
        camera.LookAt(aim);
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

    void PaintFrame(const char* activate = nullptr, bool input = false)
    {
        auto* popup = !m_imgui->OpenPopupStack.empty() ? m_imgui->OpenPopupStack.back().Window : nullptr;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({1000, 1400});
        ImGui::Begin("Terrain paint test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        if (activate) {
            /// @note ImGui::Combo の選択肢IDには表示ラベルだけでなく配列の添字も含まれる。
            const int heightMapItem = 3;
            const auto seed = popup ? ImHashData(&heightMapItem, sizeof(heightMapItem), popup->IDStack.back()) : 0;
            const auto id = popup ? ImHashStr(activate, 0, seed) : ImGui::GetID(activate);
            m_imgui->NavActivateId = id;
            m_imgui->NavActivateDownId = id;
            m_imgui->NavActivatePressedId = id;
            m_imgui->NavActivateFlags = input ? ImGuiActivateFlags_PreferInput : ImGuiActivateFlags_None;
        }
        ImGui::LogToBuffer();
        m_tool.DrawPaintContent(Context());
        m_tool.DrawBrushSettings();
        m_text = m_imgui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
    }

    void ChooseHeightStamp(const std::string& path)
    {
        m_tool.SetMode(editor::TerrainTool::Mode::Paint);
        PaintFrame();
        PaintFrame("Paint");
        ASSERT_FALSE(m_imgui->OpenPopupStack.empty());
        PaintFrame("HeightMap Stamp");
        PaintFrame();
        ASSERT_NE(m_text.find("Stamp Strength"), std::string::npos) << m_text;
        PaintFrame("##HeightStampPath", true);
        ASSERT_NE(m_imgui->InputTextState.ID, 0u);
        ImGui::GetIO().AddInputCharactersUTF8(path.c_str());
        PaintFrame();
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
        PaintFrame();
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
        PaintFrame();
        ASSERT_NE(m_text.find(path), std::string::npos) << m_text;
    }

    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_imgui = nullptr;
    editor::TerrainTool m_tool;
    editor::OperatorRegistry m_registry;
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
    EXPECT_TRUE(terrain.HasPendingSplatUpdate());
    EXPECT_EQ(terrain.terrainAssetPath, "guid:saved-after-stroke");
    terrain.vegetationRevision = 61;
    terrain.splatDirty = false;
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(terrain.splatWeights, paintedWeights);
    EXPECT_EQ(terrain.surfaceRevision, 50u);
    EXPECT_EQ(terrain.vegetationRevision, 61u);
    EXPECT_FALSE(terrain.heightDirty);
    EXPECT_FALSE(terrain.colliderDirty);
    EXPECT_TRUE(terrain.HasPendingSplatUpdate());
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
    EXPECT_FALSE(terrain.heightDirty);
    EXPECT_TRUE(terrain.heightPreview.pending);
    EXPECT_TRUE(terrain.colliderDirty);
}

TEST_F(TerrainToolAuthoringTest, DiagonalSculptDabsUndoAndRedoTogether)
{
    const auto id = AddTerrain("Diagonal stroke", 9, 9);
    auto& terrain = *AttachedScene()->GetComponent<scene::TerrainComponent>(id);
    const auto originalHeight = terrain.heightData;
    m_tool.SetActive(true);
    m_tool.SetMode(editor::TerrainTool::Mode::Sculpt);
    m_tool.SetBrush(5.0f, 1.0f, editor::TerrainFalloff::Linear);
    BrushFrame(false);
    BrushFrame(true);
    BrushFrame(true, {24.0f, 0.0f, 24.0f});
    BrushFrame(false, {24.0f, 0.0f, 24.0f});

    const auto sculptedHeight = terrain.heightData;
    EXPECT_GT(sculptedHeight[2 * 9 + 2], originalHeight[2 * 9 + 2]);
    EXPECT_GT(sculptedHeight[6 * 9 + 6], originalHeight[6 * 9 + 6]);
    EXPECT_FLOAT_EQ(sculptedHeight[4 * 9 + 4], originalHeight[4 * 9 + 4]);
    ASSERT_EQ(m_undo.GetHistorySize(), 1u);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(terrain.heightData, originalHeight);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(terrain.heightData, sculptedHeight);
}

TEST_F(TerrainToolAuthoringTest, SaturatedPaintDoesNotInvalidateTerrainOrRecordHistory)
{
    const auto id = AddTerrain("Fully painted terrain", 5, 5);
    auto& terrain = *AttachedScene()->GetComponent<scene::TerrainComponent>(id);
    terrain.splatDirty = false;
    m_tool.SetActive(true);
    m_tool.SetMode(editor::TerrainTool::Mode::Paint);
    m_tool.SetPaintLayer(0);
    m_tool.SetBrush(5.0f, 1.0f, editor::TerrainFalloff::Linear);
    BrushFrame(false);
    BrushFrame(true);
    BrushFrame(false);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    EXPECT_FALSE(terrain.HasPendingSplatUpdate());
}

TEST_F(TerrainToolAuthoringTest, BrushRayPassesThroughHoleToNegativeHeightTerrain)
{
    const auto upper = AddTerrain("Upper terrain", 5, 5);
    const auto lower = AddTerrain("Lower terrain", 5, 5);
    auto& upperTerrain = *AttachedScene()->GetComponent<scene::TerrainComponent>(upper);
    auto& lowerTerrain = *AttachedScene()->GetComponent<scene::TerrainComponent>(lower);
    upperTerrain.holeData.assign(upperTerrain.CellCount(), 1u);
    lowerTerrain.InitFlat(-10.0f);

    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(500.0f, 700.0f);
    ImGui::NewFrame();
    renderer::Camera camera;
    camera.m_position = {6.0f, 50.0f, 0.0f};
    camera.m_aspect = 1000.0f / 1400.0f;
    camera.LookAt({6.0f, 0.0f, 6.0f});
    math::Vector3 hit;
    scene::GameObject* target = nullptr;
    const bool picked = m_tool.RaycastTerrain(*AttachedScene(), camera, {0, 0}, {1000, 1400}, hit, target);
    ImGui::EndFrame();
    ASSERT_TRUE(picked);
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->GetID(), lower);
    EXPECT_NEAR(hit.y, -10.0f, 0.001f);
}

TEST_F(TerrainToolAuthoringTest, BrushRayHitsSmallCellsAtShallowAngle)
{
    const auto id = AddTerrain("Fine terrain", 17, 17);
    auto& terrain = *AttachedScene()->GetComponent<scene::TerrainComponent>(id);
    terrain.cellSize = 0.01f;

    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(500.0f, 700.0f);
    ImGui::NewFrame();
    renderer::Camera camera;
    camera.m_position = {-0.05f, 0.0006f, 0.075f};
    camera.m_aspect = 1000.0f / 1400.0f;
    camera.LookAt({0.07f, 0.0f, 0.075f});
    math::Vector3 hit;
    scene::GameObject* target = nullptr;
    const bool picked = m_tool.RaycastTerrain(*AttachedScene(), camera, {0, 0}, {1000, 1400}, hit, target);
    ImGui::EndFrame();
    ASSERT_TRUE(picked);
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->GetID(), id);
    EXPECT_NEAR(hit.x, 0.07f, 0.001f);
    EXPECT_NEAR(hit.y, 0.0f, 0.00001f);
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

TEST_F(TerrainToolAuthoringTest, RepeatedStrokesValidateSharedAndChangedBlocks)
{
    const auto id = AddTerrain("Shared terrain history", 129, 129);
    auto& terrain = *AttachedScene()->GetComponent<scene::TerrainComponent>(id);
    const auto originalWeights = terrain.splatWeights;
    Stroke(editor::TerrainTool::Mode::Paint);
    const auto firstWeights = terrain.splatWeights;
    ASSERT_NE(firstWeights, originalWeights);

    const auto last = terrain.VertexCount() - 1;
    terrain.heightData[last] = 0.25f;
    BrushFrame(true, {24.0f, 0.0f, 24.0f});
    terrain.heightData[0] = 0.5f;
    BrushFrame(false, {24.0f, 0.0f, 24.0f});
    const auto secondWeights = terrain.splatWeights;
    ASSERT_NE(secondWeights, firstWeights);
    ASSERT_EQ(m_undo.GetHistorySize(), 2u);
    EXPECT_FALSE(m_undo.Undo());
    EXPECT_EQ(m_undo.GetCursor(), 2u);
    EXPECT_EQ(terrain.splatWeights, secondWeights);

    terrain.heightData[0] = -0.0f;
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(terrain.splatWeights, firstWeights);
    EXPECT_FLOAT_EQ(terrain.heightData[last], 0.25f);
    EXPECT_FALSE(m_undo.Undo());
    terrain.heightData[last] = 0.0f;
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(terrain.splatWeights, originalWeights);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(terrain.splatWeights, firstWeights);
    terrain.heightData[last] = 0.25f;
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(terrain.splatWeights, secondWeights);
}

TEST_F(TerrainToolAuthoringTest, HoleDabPublishesRegionalTopologyAndDefersCollision)
{
    const auto id = AddTerrain("Terrain with cutout", 9, 9);
    auto& terrain = *AttachedScene()->GetComponent<scene::TerrainComponent>(id);
    terrain.holeData.assign(terrain.CellCount(), 0);
    terrain.heightDirty = terrain.colliderDirty = false;
    m_tool.SetActive(true);
    m_tool.SetMode(editor::TerrainTool::Mode::Hole);
    m_tool.SetBrush(5.0f, 1.0f, editor::TerrainFalloff::Linear);
    BrushFrame(false);
    BrushFrame(true);
    EXPECT_GT(terrain.CountHoles(), 0u);
    EXPECT_TRUE(terrain.heightPreview.topologyChanged);
    EXPECT_FALSE(terrain.heightDirty);
    EXPECT_FALSE(terrain.colliderDirty);
    BrushFrame(false);
    EXPECT_TRUE(terrain.colliderDirty);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(terrain.CountHoles(), 0u);
    EXPECT_FALSE(terrain.heightDirty);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    EXPECT_GT(terrain.CountHoles(), 0u);
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

TEST_F(TerrainToolAuthoringTest, HeightMapPaintPastesOncePerClickAndUndoesAsOneEdit)
{
    const auto id = AddTerrain("Stamped terrain", 5, 5);
    auto& terrain = *AttachedScene()->GetComponent<scene::TerrainComponent>(id);
    terrain.heightDirty = terrain.splatDirty = terrain.colliderDirty = false;
    const auto original = terrain.heightData;
    auto image = terrain;
    image.heightData.assign(image.VertexCount(), 1.0f);
    const auto path = File("PaintStamp.png").string();
    ASSERT_TRUE(scene::SaveHeightMapToFile(path, image));
    ChooseHeightStamp(path);
    EXPECT_EQ(m_tool.StatusLabel(), "HeightMap Stamp");
    m_tool.SetActive(true);
    BrushFrame(false);
    BrushFrame(true);
    const auto stamped = terrain.heightData;
    ASSERT_NE(stamped, original);
    EXPECT_TRUE(terrain.heightPreview.pending);
    EXPECT_FALSE(terrain.heightDirty);
    EXPECT_FALSE(terrain.splatDirty);
    EXPECT_TRUE(terrain.colliderDirty);
    EXPECT_NEAR(terrain.GetHeightAt(8.0f, 8.0f), 10.0f, 0.001f);
    BrushFrame(true);
    BrushFrame(true);
    EXPECT_EQ(terrain.heightData, stamped);
    BrushFrame(false);
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(terrain.heightData, original);
    EXPECT_FALSE(terrain.heightDirty);
    EXPECT_FALSE(terrain.splatDirty);
    ASSERT_TRUE(m_undo.Redo());
    EXPECT_EQ(terrain.heightData, stamped);
    EXPECT_FALSE(terrain.heightDirty);
    EXPECT_FALSE(terrain.splatDirty);
}

TEST_F(TerrainToolAuthoringTest, MissingStampImageDoesNotFallThroughToMaterialPainting)
{
    const auto id = AddTerrain("Unchanged terrain", 5, 5);
    auto& terrain = *AttachedScene()->GetComponent<scene::TerrainComponent>(id);
    const auto original = terrain;
    ChooseHeightStamp(File("MissingStamp.png").string());
    m_tool.SetActive(true);
    BrushFrame(false);
    BrushFrame(true);
    BrushFrame(false);
    EXPECT_EQ(terrain.heightData, original.heightData);
    EXPECT_EQ(terrain.splatWeights, original.splatWeights);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    PaintFrame();
    EXPECT_NE(m_text.find("Error:"), std::string::npos) << m_text;
}

} /// @note namespace
} /// @note namespace fbzz::tests
