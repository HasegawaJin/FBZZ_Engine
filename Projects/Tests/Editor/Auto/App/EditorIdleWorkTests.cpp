/// @file    EditorIdleWorkTests.cpp
/// @brief   一覧キャッシュの失効、Hierarchy の生成物表示、Terrain の変更前 Undo を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Script/Managed/ManagedScriptEditor.hpp>
#include <Editor/Panels/AssetBrowser/AssetBrowserPanel.hpp>
#include <Editor/Panels/BuildOutput/BuildOutputPanel.hpp>
#include <Editor/Panels/Diagnostics/ConsolePanel.hpp>
#include <Editor/Panels/AssetBrowser/DependencyViewPanel.hpp>
#include <Editor/Panels/Scene/SceneHierarchyPanel.hpp>
#include <Editor/Panels/Workspace/StatusBar.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Project/EditorSettings.hpp>
#include <Editor/UI/Localization.hpp>
#include <Editor/Scene/Editing/Selection.hpp>
#include <Editor/Scene/Editing/UndoStack.hpp>
#include <Engine/Scene/Components/Terrain/TerrainComponent.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <Engine/Util/Files/FileSystem.hpp>
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

/// @brief Console を閉じていてもソース確認中の C# 要求が見え、同時に動くシェーダーの進捗も残る。
TEST_F(EditorIdleWorkTest, StatusBarShowsManagedBuildProgressWithConsoleClosed)
{
    editor::StatusBar bar;
    bool consoleVisible = false;
    ImVec2 start;
    const auto draw = [&] {
        start = ImGui::GetCursorScreenPos();
        bar.Draw(Context(), nullptr, &consoleVisible);
    };
    EXPECT_EQ(Frame(draw).find("C#"), std::string::npos);

    ASSERT_TRUE(util::FileSystem::WriteText(File("Fixture.Scripts.csproj"), "<Project />"));
    editor::ManagedScriptEditor scripts;
    scripts.Init(ProjectRoot(), FBZZ_SOURCE_DIR);
    ASSERT_TRUE(scripts.IsAvailable()) << scripts.AvailabilityDiagnostic();
    Context().managedScripts = &scripts;
    Context().managedBuildBusy = scripts.IsBusy();
    Context().hotReloadState = editor::EditorContext::HotReloadState::Compiling;
    EXPECT_NE(Frame(draw).find("ソース確認中"), std::string::npos);

    std::string request;
    ASSERT_TRUE(scripts.RequestBuild(request));
    const auto text = Frame(draw);
    EXPECT_NE(text.find("C#"), std::string::npos) << text;
    EXPECT_NE(text.find("ソース確認中"), std::string::npos) << text;
    EXPECT_NE(text.find("Compiling shaders"), std::string::npos) << text;
    ImGui::GetIO().AddMousePosEvent(start.x + 12.0f, start.y + ImGui::GetFrameHeight() * 0.5f);
    Frame(draw);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    Frame(draw);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    Frame(draw);
    EXPECT_TRUE(Context().requestOpenBuildOutput);
    EXPECT_FALSE(consoleVisible);
    editor::ConsoleSink sink;
    editor::ConsolePanel console(sink);
    editor::BuildOutputPanel output;
    const auto drawPanel = [&](editor::IPanel& panel) {
        return Frame([&] {
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({900, 600});
            panel.OnRender(Context());
        });
    };
    const auto consoleText = drawPanel(console);
    EXPECT_NE(consoleText.find(LOCT("No logs yet. Engine and script messages will appear here.")), std::string::npos) << consoleText;
    EXPECT_EQ(consoleText.find("C# ビルド"), std::string::npos);
    EXPECT_EQ(consoleText.find(LOCT("Select a log line to see the full message.")), std::string::npos);
    EXPECT_NE(drawPanel(output).find("ソース確認中"), std::string::npos);
    scripts.Shutdown(nullptr);
    Context().managedScripts = nullptr;
}

/// @brief Hierarchy roots and generated badges refresh after direct scene changes.
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

/// @brief 検索の有無に関係なく、同じ行を右クリックして名前変更と画面へのフォーカスができる。
TEST_F(EditorIdleWorkTest, HierarchyTreeAndSearchShowTheSameObjectActions)
{
    auto& object = AttachedScene()->CreateGameObject("Context target");
    IdleHierarchy panel;
    editor::OperatorRegistry registry;
    panel.OnRegisterOperators(registry);
    editor::RegisterNodeOperators(registry);
    for (const char* id : { "edit.rename", "view.frame_selected", "edit.paste_as_child" }) {
        editor::EditorOperator operation;
        operation.id = id;
        operation.exec = [](editor::OpContext&, const editor::OpArgs&) { return editor::OpResult::Ok(); };
        registry.Register(std::move(operation));
    }
    Context().operators = &registry;
    ImVec2 rowCenter;
    const auto draw = [&] {
        panel.Draw(Context());
        const auto lo = ImGui::GetItemRectMin();
        const auto hi = ImGui::GetItemRectMax();
        rowCenter = { lo.x + 40.0f, (lo.y + hi.y) * 0.5f };
    };
    const auto openMenu = [&] {
        Frame(draw);
        ImGui::GetIO().AddMousePosEvent(rowCenter.x, rowCenter.y);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        Frame(draw);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        /// @note 初回はポップアップの寸法計測中なので、表示が確定した次フレームを読む。
        Frame(draw);
        return Frame(draw);
    };

    const auto tree = openMenu();
    EXPECT_EQ(Context().PrimarySelected(), object.GetID());
    EXPECT_NE(tree.find(LOCT("Rename")), std::string::npos) << tree;
    EXPECT_NE(tree.find(LOCT("Frame Selected")), std::string::npos) << tree;
    EXPECT_NE(tree.find(LOCT("Paste As Child")), std::string::npos) << tree;

    ImGui::ClosePopupToLevel(0, false);
    editor::OpArgs search;
    search.Set("search", std::string("Context target"));
    editor::OpContext operation{Context(), m_undo};
    ASSERT_TRUE(registry.Invoke("hierarchy.view.configure", operation, search).ok);
    const auto results = openMenu();
    EXPECT_NE(results.find(LOCT("Rename")), std::string::npos) << results;
    EXPECT_NE(results.find(LOCT("Frame Selected")), std::string::npos) << results;

    ImGui::ClosePopupToLevel(0, false);
    const auto sibling = AttachedScene()->CreateGameObject("Other selected object").GetID();
    const std::vector selected{ sibling, object.GetID() };
    editor::SelectEntities(Context(), selected, editor::SelectionReveal::Skip);
    const auto multiple = openMenu();
    EXPECT_EQ(Context().selectedEntities, selected);
    EXPECT_NE(multiple.find(LOCT("Group Selection")), std::string::npos) << multiple;
    EXPECT_EQ(multiple.find(LOCT("Create Child")), std::string::npos) << multiple;
    EXPECT_EQ(multiple.find(LOCT("Paste As Child")), std::string::npos) << multiple;
    Context().operators = nullptr;
}

/// @brief 選択が残っていても空白の右クリックはシーンルートを対象とする。
TEST_F(EditorIdleWorkTest, HierarchyBackgroundMenuUsesSceneRootWithASelection)
{
    auto& object = AttachedScene()->CreateGameObject("Selected parent");
    editor::SelectEntity(Context(), object.GetID(), editor::SelectionReveal::Skip);
    IdleHierarchy panel;
    const auto draw = [&] { panel.Draw(Context()); };
    Frame(draw);
    ImGui::GetIO().AddMousePosEvent(400.0f, 400.0f);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    Frame(draw);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    const auto menu = Frame(draw);
    EXPECT_NE(menu.find(LOCT("Scene Root")), std::string::npos) << menu;
    EXPECT_EQ(menu.find("Create under"), std::string::npos) << menu;
    EXPECT_EQ(Context().PrimarySelected(), object.GetID());
}

/// @brief ルートの「下へ」は子が間に並ぶシーンでも隣のルートと入れ替える。
TEST_F(EditorIdleWorkTest, HierarchyReorderMovesBetweenSiblingRows)
{
    auto& world = *AttachedScene();
    auto& first = world.CreateGameObject("First");
    auto& child = world.CreateGameObject("Child");
    child.SetParent(first);
    world.CreateGameObject("Second");
    editor::OperatorRegistry registry;
    editor::RegisterNodeOperators(registry);
    editor::OpContext operation{Context(), m_undo};
    editor::OpArgs args;
    args.Set("node", first.instanceId);
    args.Set("direction", std::string("down"));

    const auto result = registry.Invoke("node.reorder", operation, args);
    ASSERT_TRUE(result.ok) << result.message;
    ASSERT_EQ(world.GetRootGameObjects().size(), 2u);
    EXPECT_EQ(world.GetRootGameObjects()[0]->name, "Second");
    EXPECT_EQ(child.GetParent(), &first);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(world.GetRootGameObjects()[0]->name, "First");
}

/// @brief 親子をまとめてグループ化しても子は親の下に残り、一度の Undo と Redo で往復できる。
TEST_F(EditorIdleWorkTest, HierarchyGroupKeepsSelectedDescendantsAndSupportsRedo)
{
    auto& world = *AttachedScene();
    auto& first = world.CreateGameObject("Parent");
    auto& child = world.CreateGameObject("Child");
    child.SetParent(first);
    const auto childGuid = child.instanceId;
    const auto sibling = world.CreateGameObject("Sibling").GetID();
    editor::SelectEntities(Context(), {first.GetID(), child.GetID(), sibling}, editor::SelectionReveal::Skip);
    editor::OperatorRegistry registry;
    editor::RegisterNodeOperators(registry);
    editor::OpContext operation{Context(), m_undo};

    const auto result = registry.Invoke("node.group_selection", operation);
    ASSERT_TRUE(result.ok) << result.message;
    ASSERT_NE(Context().GetSelectedGO(), nullptr);
    const auto groupGuid = Context().GetSelectedGO()->instanceId;
    EXPECT_EQ(child.GetParent(), &first);
    EXPECT_EQ(first.GetParent(), Context().GetSelectedGO());
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(world.FindByGuid(groupGuid), nullptr);
    EXPECT_FALSE(m_undo.CanUndo());
    ASSERT_TRUE(m_undo.Redo());
    const auto* restoredChild = world.FindByGuid(childGuid);
    ASSERT_TRUE(restoredChild && restoredChild->GetParent());
    EXPECT_EQ(restoredChild->GetParent()->GetParent(), world.FindByGuid(groupGuid));
    EXPECT_NE(world.FindByGuid(groupGuid), nullptr);
}

/// @brief Mesh ray index keeps triangle order and refreshes on publication.
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

/// @brief Dependency view publishes only the latest selection.
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

/// @brief Terrain idle frames do not create undo and clear holes restores full data.
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

/// @brief Initialize flat records height change even when settings stay equal.
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

/// @brief Asset metadata remains cached until refresh and then updates.
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

/// @brief ファイルではそのファイルの操作、フォルダと空白では各フォルダへの操作を表示する。
TEST_F(EditorIdleWorkTest, AssetBrowserMenusDistinguishFileFolderAndBackground)
{
    const auto directory = File("Assets/Menu");
    std::filesystem::create_directories(directory / "Destination");
    std::ofstream(directory / "Source.txt") << "data";
    const auto root = util::FileSystem::PathToUtf8(directory);
    IdleAssetBrowser browser(root);
    editor::EditorSettings settings;
    auto& state = settings.AssetBrowserPanelAt(1);
    state.viewMode = 1;
    state.currentFolder = root;
    browser.OnLoadSettings(settings);
    Context().projectRoot = ProjectRoot().generic_string();
    Context().scriptsSourceDir = root;
    Context().hlslSourceDir = root;
    const auto draw = [&] { browser.Draw(Context()); };
    Frame(draw);
    Frame(draw);

    ImGuiWindow* content = nullptr;
    for (auto* window : m_gui->Windows) {
        const std::string_view name(window->Name);
        const auto separator = name.rfind('/');
        if (separator != std::string_view::npos && name.substr(separator + 1).starts_with("##content_")) {
            content = window;
            break;
        }
    }
    ASSERT_NE(content, nullptr);
    const auto* table = ImGui::TableFindByID(content->GetID("##list"));
    ASSERT_NE(table, nullptr);
    const float x = table->OuterRect.Min.x + 30.0f;
    const float fileY = (table->RowPosY1 + table->RowPosY2) * 0.5f;
    const float folderY = fileY - (table->RowPosY2 - table->RowPosY1);
    const auto menuAt = [&](float y) {
        ImGui::GetIO().AddMousePosEvent(x, y);
        Frame(draw);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        Frame(draw);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        Frame(draw);
        return Frame(draw);
    };
    const auto activateMenuItem = [&](const char* label) {
        ASSERT_FALSE(m_gui->OpenPopupStack.empty());
        ImGuiWindow* popup = m_gui->OpenPopupStack.back().Window;
        ASSERT_NE(popup, nullptr);
        const auto id = popup->GetID(LOC(label));
        Frame([&] {
            m_gui->NavActivateId = id;
            m_gui->NavActivateDownId = id;
            m_gui->NavActivatePressedId = id;
            m_gui->NavActivateFlags = ImGuiActivateFlags_None;
            draw();
        });
    };

    const auto file = menuAt(fileY);
    EXPECT_NE(file.find(LOCT("Find References...")), std::string::npos) << file;
    EXPECT_EQ(file.find(LOCT("Paste Here")), std::string::npos) << file;
    activateMenuItem("Copy");
    ASSERT_TRUE(m_gui->OpenPopupStack.empty());

    const auto folder = menuAt(folderY);
    ASSERT_NE(folder.find("Assets/Menu/Destination"), std::string::npos) << folder;
    EXPECT_NE(folder.find(LOCT("Open Folder")), std::string::npos) << folder;
    EXPECT_NE(folder.find(LOCT("Create Here")), std::string::npos) << folder;
    EXPECT_NE(folder.find(LOCT("Paste Here")), std::string::npos) << folder;
    activateMenuItem("Paste Here");
    ASSERT_TRUE(m_gui->OpenPopupStack.empty()) << folder;
    EXPECT_TRUE(std::filesystem::is_regular_file(directory / "Destination" / "Source.txt"))
        << "Selection: " << Context().selectedAssetPath << '\n' << folder;
    EXPECT_FALSE(std::filesystem::exists(directory / "Source(2).txt"));

    const auto background = menuAt(fileY + 150.0f);
    EXPECT_NE(background.find(LOCT("Paste Here")), std::string::npos) << background;
    EXPECT_NE(background.find(LOCT("Create Here")), std::string::npos) << background;
    EXPECT_EQ(background.find(LOCT("Find References...")), std::string::npos) << background;
}

/// @brief Hierarchy hidden counts follow generated visibility each frame.
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
