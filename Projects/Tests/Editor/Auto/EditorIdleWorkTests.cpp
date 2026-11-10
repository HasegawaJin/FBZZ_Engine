/// @file    EditorIdleWorkTests.cpp
/// @brief   一覧キャッシュの失効、Hierarchy の生成物表示、Terrain の変更前 Undo を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Panels/AssetBrowserPanel.hpp>
#include <Editor/Panels/DependencyViewPanel.hpp>
#include <Editor/Panels/SceneHierarchyPanel.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include "Panels/Inspector/InspectorTerrainWater.hpp"
#include "Panels/Viewport/ViewportCommon.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <fstream>
#include <functional>

namespace fbzz::tests {
namespace {

class IdleAssetBrowser final : public editor::AssetBrowserPanel {
public:
    explicit IdleAssetBrowser(const std::string& root) : AssetBrowserPanel(root, 1) {}
    void Draw(editor::EditorContext& context)
    {
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({1800, 8000});
        OnRender(context);
    }
};

class IdleHierarchy final : public editor::SceneHierarchyPanel {
public:
    void Draw(editor::EditorContext& context) { OnRenderContent(context); }
};

class EditorIdleWorkTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        AttachScene();
        Context().undoStack = &m_undo;
        m_previous = ImGui::GetCurrentContext();
        m_gui = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_gui);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1800, 8000};
        io.DeltaTime = 1.0f / 60;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    void TearDown() override
    {
        Context().undoStack = nullptr;
        ImGui::DestroyContext(m_gui);
        ImGui::SetCurrentContext(m_previous);
        EditorFixture::TearDown();
    }
    std::string Frame(const std::function<void()>& draw, const char* terrainAction = nullptr)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({1800, 8000});
        ImGui::Begin("Editor idle work test", nullptr,
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        if (terrainAction) {
            ImGui::PushID("Terrain");
            const ImGuiID id = ImGui::GetID(terrainAction);
            ImGui::PopID();
            m_gui->NavActivateId = id;
            m_gui->NavActivateDownId = id;
            m_gui->NavActivatePressedId = id;
            m_gui->NavActivateFlags = ImGuiActivateFlags_None;
        }
        ImGui::LogToBuffer();
        draw();
        const std::string text = m_gui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return text;
    }
    void TerrainFrame(scene::GameObject& object, const char* action = nullptr)
    {
        Frame([&] { editor::DrawTerrainWaterInspectors(&object, Context(), m_clipboard, m_clipboardType); }, action);
    }
    editor::UndoStack m_undo;
    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_gui = nullptr;
    std::any m_clipboard;
    const std::type_info* m_clipboardType = nullptr;
};

TEST_F(EditorIdleWorkTest, HierarchyRootsAndGeneratedBadgesRefreshAfterDirectSceneChanges)
{
    auto& world = *AttachedScene();
    auto& root = world.CreateGameObject("Badge root");
    auto& child = world.CreateGameObject("Hidden child");
    child.SetParent(root);
    IdleHierarchy panel;
    const auto draw = [&] { panel.Draw(Context()); };
    Frame(draw);
    child.SetRuntimeGenerated(true);
    EXPECT_NE(Frame(draw).find("+1"), std::string::npos);
    child.SetRuntimeGenerated(false);
    EXPECT_EQ(Frame(draw).find("+1"), std::string::npos);
    ASSERT_TRUE(child.ClearParent());
    EXPECT_NE(Frame(draw).find("Hidden child"), std::string::npos);
}

TEST_F(EditorIdleWorkTest, MeshRayIndexKeepsTriangleOrderAndRefreshesOnPublication)
{
    renderer::Mesh mesh;
    for (uint32_t i = 0; i < 256; ++i) {
        const float x = static_cast<float>(i) * 4;
        mesh.cpuVertices.push_back({{x, -1, 0}});
        mesh.cpuVertices.push_back({{x + 2, -1, 0}});
        mesh.cpuVertices.push_back({{x + 1, 1, 0}});
        mesh.cpuIndices.insert(mesh.cpuIndices.end(), {i * 3, i * 3 + 1, i * 3 + 2});
    }
    const auto world = math::Matrix4::Translate({0, 0, 5}) * math::Matrix4::Scale({-2, 3, 0.5f});
    const math::Ray ray{{-2, 0, 0}, math::Vector3::FORWARD};
    std::vector<size_t> offsets;
    EXPECT_FALSE(editor::CollectRayTriangleOffsets(mesh, false, world, ray, offsets));
    mesh.PublishCpuGeometry();
    ASSERT_TRUE(editor::CollectRayTriangleOffsets(mesh, false, world, ray, offsets));
    ASSERT_FALSE(offsets.empty());
    EXPECT_EQ(offsets.front(), 0u);
    EXPECT_LT(offsets.size(), 32u);
    EXPECT_TRUE(std::is_sorted(offsets.begin(), offsets.end()));
    for (auto& vertex : mesh.cpuVertices) vertex.position.y += 20;
    mesh.ComputeBounds();
    EXPECT_FALSE(editor::CollectRayTriangleOffsets(mesh, false, world, ray, offsets));
    mesh.PublishCpuGeometry();
    EXPECT_TRUE(editor::CollectRayTriangleOffsets(mesh, false, world, ray, offsets));
    EXPECT_TRUE(offsets.empty());
}

TEST_F(EditorIdleWorkTest, DependencyViewPublishesOnlyTheLatestSelection)
{
    auto& context = Context();
    const auto assets = ProjectRoot() / "Assets";
    const auto first = util::FileSystem::PathToUtf8(assets / "first.mat");
    const auto second = util::FileSystem::PathToUtf8(assets / "second.mat");
    { std::ofstream output(assets / "FirstOwner.scene"); output << "ref = 'Assets/first.mat'"; }
    { std::ofstream output(assets / "SecondOwner.scene"); output << "ref = 'Assets/second.mat'"; }
    context.projectRoot = util::FileSystem::PathToUtf8(ProjectRoot());
    asset::AssetDatabase::Init(util::FileSystem::PathToUtf8(assets));
    TaskSystem::Init(1);
    {
        editor::DependencyViewPanel panel;
        const auto draw = [&] {
            ImGui::SetNextWindowSize({900, 700});
            panel.OnRender(context);
        };
        context.selectedAssetPath = first;
        EXPECT_NE(Frame(draw).find("Scanning saved references"), std::string::npos);
        TaskSystem::Submit([] {}).get();
        context.selectedAssetPath = second;
        EXPECT_EQ(Frame(draw).find("FirstOwner.scene"), std::string::npos);
        TaskSystem::Submit([] {}).get();
        const auto result = Frame(draw);
        EXPECT_NE(result.find("SecondOwner.scene"), std::string::npos);
        EXPECT_EQ(result.find("FirstOwner.scene"), std::string::npos);
    }
    TaskSystem::Shutdown();
    asset::AssetDatabase::Shutdown();
}

TEST_F(EditorIdleWorkTest, TerrainIdleFramesDoNotCreateUndoAndClearHolesRestoresFullData)
{
    auto& object = AttachedScene()->CreateGameObject("Terrain");
    auto& terrain = object.AddComponent<scene::TerrainComponent>();
    terrain.columns = terrain.rows = 5;
    terrain.InitFlat(3);
    terrain.holeData.assign(16, 1);
    const auto heights = terrain.heightData;
    const auto holes = terrain.holeData;
    TerrainFrame(object);
    TerrainFrame(object);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    TerrainFrame(object, "Clear Holes");
    TerrainFrame(object);
    EXPECT_TRUE(terrain.holeData.empty());
    ASSERT_EQ(m_undo.GetHistorySize(), 1u);
    m_undo.Undo();
    EXPECT_EQ(terrain.holeData, holes);
    EXPECT_EQ(terrain.heightData, heights);
    m_undo.Redo();
    EXPECT_TRUE(terrain.holeData.empty());
}

TEST_F(EditorIdleWorkTest, InitializeFlatRecordsHeightChangeEvenWhenSettingsStayEqual)
{
    auto& object = AttachedScene()->CreateGameObject("Terrain");
    auto& terrain = object.AddComponent<scene::TerrainComponent>();
    terrain.columns = terrain.rows = 5;
    terrain.InitFlat(3);
    const auto heights = terrain.heightData;
    TerrainFrame(object);
    TerrainFrame(object, "Initialize Flat");
    TerrainFrame(object);
    ASSERT_EQ(m_undo.GetHistorySize(), 1u);
    EXPECT_NE(terrain.heightData, heights);
    m_undo.Undo();
    EXPECT_EQ(terrain.heightData, heights);
    m_undo.Redo();
    for (const auto height : terrain.heightData) EXPECT_FLOAT_EQ(height, 0);
}

TEST_F(EditorIdleWorkTest, AssetMetadataRemainsCachedUntilRefreshAndThenUpdates)
{
    const auto directory = File("Assets");
    std::filesystem::create_directories(directory);
    const auto asset = directory / "Idle.txt";
    { std::ofstream output(asset); output << "abc"; }
    const auto root = util::FileSystem::PathToUtf8(directory);
    IdleAssetBrowser browser(root);
    editor::EditorSettings settings;
    auto& state = settings.AssetBrowserPanelAt(1);
    state.viewMode = 1;
    state.currentFolder = root;
    browser.OnLoadSettings(settings);
    const auto draw = [&] { browser.Draw(Context()); };
    Frame(draw);
    EXPECT_NE(Frame(draw).find("3 B"), std::string::npos);
    { std::ofstream output(asset); output << "abcdef"; }
    EXPECT_NE(Frame(draw).find("3 B"), std::string::npos);
    ++Context().assetBrowserMetadataGeneration;
    EXPECT_NE(Frame(draw).find("6 B"), std::string::npos);
    { std::ofstream output(directory / "New.txt"); output << "new"; }
    Context().requestAssetBrowserRefresh = true;
    EXPECT_NE(Frame(draw).find("New.txt"), std::string::npos);
}

TEST_F(EditorIdleWorkTest, HierarchyHiddenCountsFollowGeneratedVisibilityEachFrame)
{
    auto& source = *AttachedScene();
    const auto root = source.CreateGameObject("VisibleRoot").GetID();
    const auto branch = source.CreateGameObject("VisibleBranch").GetID();
    const auto generated = source.CreateGameObject("GeneratedLeaf").GetID();
    ASSERT_TRUE(source.GetGameObject(branch)->SetParent(source.GetGameObject(root)));
    ASSERT_TRUE(source.GetGameObject(generated)->SetParent(source.GetGameObject(branch)));
    source.GetGameObject(generated)->SetRuntimeGenerated(true);
    IdleHierarchy hierarchy;
    const auto draw = [&] { hierarchy.Draw(Context()); };
    Context().showGeneratedObjects = false;
    Frame(draw);
    const auto hidden = Frame(draw);
    EXPECT_EQ(hidden.find("GeneratedLeaf"), std::string::npos);
    EXPECT_NE(hidden.find("(+1)"), std::string::npos);
    Context().showGeneratedObjects = true;
    const auto visible = Frame(draw);
    EXPECT_NE(visible.find("GeneratedLeaf"), std::string::npos);
    EXPECT_EQ(visible.find("(+1)"), std::string::npos);
}

} /// @note namespace
} /// @note namespace fbzz::tests
