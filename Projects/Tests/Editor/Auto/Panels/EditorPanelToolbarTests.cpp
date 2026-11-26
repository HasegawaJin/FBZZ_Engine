/// @file    EditorPanelToolbarTests.cpp
/// @brief   エディターの検索操作と狭いドックでのツールバー配置を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-23
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Panels/AssetBrowser/AssetBrowserPanel.hpp>
#include <Editor/Panels/Diagnostics/ConsolePanel.hpp>
#include <Editor/Project/EditorSettings.hpp>
#include <Engine/Util/Files/FileSystem.hpp>
#include <algorithm>
#include <fstream>
#include <Editor/Panels/Project/HotkeyEditorPanel.hpp>
#include <Editor/Panels/Scene/SceneHierarchyPanel.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Op/HotkeyManager.hpp>
#include <Editor/Scene/Editing/Selection.hpp>
#include <Editor/Scene/Editing/UndoStack.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/Camera/CameraComponent.hpp>
#include <Editor/Panels/AssetBrowser/AssetBrowserThumbnail.hpp>
#include <imgui_internal.h>

namespace fbzz::tests {

class EditorPanelToolbarTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        m_previous = ImGui::GetCurrentContext();
        m_gui = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_gui);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = { 1024, 720 };
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_gui);
        ImGui::SetCurrentContext(m_previous);
        EditorFixture::TearDown();
    }

    std::string Frame(editor::IPanel& panel, float width = 400.0f)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ width, 420 });
        ImGui::LogToBuffer();
        panel.OnRender(Context());
        const std::string rendered = m_gui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::Render();
        return rendered;
    }

    std::string Search(editor::IPanel& panel, const char* query)
    {
        Frame(panel);
        Frame(panel);
        auto& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_F, true);
        Frame(panel);
        io.AddKeyEvent(ImGuiKey_F, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        Frame(panel);
        io.AddInputCharactersUTF8(query);
        return Frame(panel);
    }

private:
    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_gui = nullptr;
};

/// @brief Hierarchy の Ctrl+F で検索でき、Esc で未一致状態から元の一覧へ戻れる。
TEST_F(EditorPanelToolbarTest, SearchesFocusedHierarchyAndClearsQueryWithEscape)
{
    AttachScene().CreateGameObject("Main camera");
    editor::SceneHierarchyPanel panel;
    editor::OperatorRegistry registry;
    editor::UndoStack undo;
    editor::OpContext operation{ Context(), undo };
    panel.OnRegisterOperators(registry);
    const auto query = [&] {
        const auto result = registry.Invoke("hierarchy.view.inspect", operation);
        return std::string(result.data.Find("search")->AsString());
    };
    const auto filtered = Search(panel, "missing");
    EXPECT_EQ(query(), "missing");
    EXPECT_NE(filtered.find(LOCT("No matching objects")), std::string::npos);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    const auto cleared = Frame(panel);
    EXPECT_TRUE(query().empty());
    EXPECT_NE(cleared.find("Main camera"), std::string::npos);
}

/// @brief クリアボタンを押した後は、そのまま次の検索語を入力できる。
TEST_F(EditorPanelToolbarTest, ClearButtonReturnsFocusToSearch)
{
    AttachScene().CreateGameObject("River");
    editor::SceneHierarchyPanel panel;
    Search(panel, "missing");
    Frame(panel);
    auto* window = ImGui::FindWindowByName("Scene Hierarchy###Scene Hierarchy");
    ASSERT_NE(window, nullptr);
    const ImGuiID field = window->GetID("##hierarchy_search");
    ImGui::ActivateItemByID(ImHashStr("##clear", 0, field));
    EXPECT_NE(Frame(panel).find("River"), std::string::npos);
    Frame(panel);
    ImGui::GetIO().AddInputCharactersUTF8("another");
    EXPECT_NE(Frame(panel).find(LOCT("No matching objects")), std::string::npos);
}

/// @brief 横幅 240 px の Console で検索・ログ操作がパネルの右端を越えない。
TEST_F(EditorPanelToolbarTest, KeepsConsoleActionsInsideANarrowDock)
{
    editor::ConsoleSink sink;
    editor::ConsolePanel panel(sink);
    Frame(panel, 240);
    Frame(panel, 240);
    const auto* window = ImGui::FindWindowByName("Console###Console");
    ASSERT_NE(window, nullptr);
    EXPECT_LE(window->ContentSize.x, window->InnerRect.GetWidth() + 1.0f);
}

/// @brief 幅 280 px の Console で操作に使う高さを 2 行に収め、ログ一覧に場所を残す。
TEST_F(EditorPanelToolbarTest, CompactConsoleReservesTwoRowsForControls)
{
    editor::ConsoleSink sink;
    editor::ConsolePanel panel(sink);
    Frame(panel, 280);
    Frame(panel, 280);
    const auto* window = ImGui::FindWindowByName("Console###Console");
    ASSERT_NE(window, nullptr);
    ASSERT_FALSE(window->DC.ChildWindows.empty());
    const auto* logs = window->DC.ChildWindows.front();
    const float maxTop = window->DC.CursorStartPos.y + 2.0f * ImGui::GetFrameHeightWithSpacing()
                       + ImGui::GetStyle().ItemSpacing.y + 2.0f;
    EXPECT_LE(logs->Pos.y, maxTop);
}

/// @brief 狭いドックでは一覧に幅を譲り、フォルダー操作から同じツリーを開ける。
TEST_F(EditorPanelToolbarTest, NarrowAssetBrowserOpensFoldersOnDemand)
{
    const auto root = File("Assets");
    std::filesystem::create_directories(root / "Materials");
    editor::AssetBrowserPanel panel(util::FileSystem::PathToUtf8(root));
    Frame(panel, 336);
    Frame(panel, 336);
    const auto* panelWindow = ImGui::FindWindowByName("Asset Browser###Asset Browser");
    ASSERT_NE(panelWindow, nullptr);
    ASSERT_FALSE(panelWindow->DC.ChildWindows.empty());
    auto* content = panelWindow->DC.ChildWindows.back();
    EXPECT_GE(content->Size.x, panelWindow->InnerRect.GetWidth() - 2.0f * ImGui::GetStyle().WindowPadding.x - 1.0f);
    ImGui::ActivateItemByID(content->GetID("##folder_navigator"));
    Frame(panel, 336);
    EXPECT_FALSE(ImGui::GetCurrentContext()->OpenPopupStack.empty());
}

/// @brief 深いフォルダーを狭いドックで開いても、現在地が一覧の表示幅を押し広げない。
TEST_F(EditorPanelToolbarTest, DeepAssetBreadcrumbFitsTheDock)
{
    const auto root = File("Assets");
    const auto nested = root / "Environment" / "Rivers" / "WaterMaterials" / "Riverbed" / "Summer" / "RiverFoam";
    std::filesystem::create_directories(nested);
    editor::AssetBrowserPanel panel(util::FileSystem::PathToUtf8(root));
    editor::EditorSettings settings;
    settings.AssetBrowserPanelAt(0).currentFolder = util::FileSystem::PathToUtf8(nested);
    panel.OnLoadSettings(settings);
    Frame(panel, 336);
    Frame(panel, 336);
    const auto* window = ImGui::FindWindowByName("Asset Browser###Asset Browser");
    ASSERT_NE(window, nullptr);
    auto* content = window->DC.ChildWindows.back();
    EXPECT_LE(content->ContentSize.x, content->InnerRect.GetWidth() + 1.0f);
    ImGui::ActivateItemByID(content->GetID("##ancestors"));
    Frame(panel, 336);
    Frame(panel, 336);
    auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
    ASSERT_FALSE(popups.empty());
    auto* menu = popups.back().Window;
    ASSERT_NE(menu, nullptr);
    ImGui::ActivateItemByID(ImHashStr("WaterMaterials", 0, menu->GetID(3)));
    Frame(panel, 336);
    panel.OnSaveSettings(settings);
    EXPECT_TRUE(util::FileSystem::SamePathText(settings.AssetBrowserPanelAt(0).currentFolder,
        util::FileSystem::PathToUtf8(root / "Environment" / "Rivers" / "WaterMaterials")));
}

/// @brief ショートカット一覧を割り当て文字列で検索すると該当する操作だけが残る。
TEST_F(EditorPanelToolbarTest, FindsShortcutByBinding)
{
    editor::HotkeyManager keys;
    keys.RegisterInfo("Orbit sample", "Alt+Drag", editor::HotkeyCategory::Viewport);
    keys.RegisterInfo("Duplicate sample", "Ctrl+D", editor::HotkeyCategory::Edit);
    Context().hotkeyManager = &keys;
    editor::HotkeyEditorPanel panel;
    const auto filtered = Search(panel, "ctrl+d");
    EXPECT_NE(filtered.find("Duplicate sample"), std::string::npos);
    EXPECT_EQ(filtered.find("Orbit sample"), std::string::npos);
    Context().hotkeyManager = nullptr;
}

/// @brief 更新日時を選んだグリッドでは、名前が後でも新しいファイルが先に並ぶ。
TEST_F(EditorPanelToolbarTest, OrdersAssetGridByModificationTime)
{
    const auto directory = File("Assets");
    std::filesystem::create_directories(directory);
    const auto oldFile = directory / "A.txt";
    const auto newFile = directory / "Z.txt";
    std::ofstream(oldFile).put('a');
    std::ofstream(newFile).put('z');
    const auto stamp = std::filesystem::file_time_type{} + std::chrono::hours(48);
    std::filesystem::last_write_time(oldFile, stamp - std::chrono::hours(1));
    std::filesystem::last_write_time(newFile, stamp);
    const std::string root = util::FileSystem::PathToUtf8(directory);
    editor::AssetBrowserPanel panel(root, 1);
    editor::EditorSettings settings;
    auto& state = settings.AssetBrowserPanelAt(1);
    state.sortMode = 3;
    state.currentFolder = root;
    state.treeShowFiles = false;
    panel.OnLoadSettings(settings);
    Frame(panel, 800);
    const auto drawn = Frame(panel, 800);
    const auto newer = drawn.find("Z.txt"), older = drawn.find("A.txt");
    ASSERT_NE(older, std::string::npos);
    ASSERT_NE(newer, std::string::npos);
    EXPECT_LT(newer, older);
}

/// @brief グリッドで省略される日本語の名前も、描画側には完全な UTF-8 の名前が届く。
TEST_F(EditorPanelToolbarTest, LongAssetCaptionRetainsTheWholeUtf8Name)
{
    const auto root = File("Assets");
    const std::string name = "河口の浅瀬にある濡れた岩のマテリアル.mat";
    std::ofstream(root / util::FileSystem::PathFromUtf8(name)).put('x');
    editor::AssetBrowserPanel panel(util::FileSystem::PathToUtf8(root));
    editor::EditorSettings settings;
    auto& state = settings.AssetBrowserPanelAt(0);
    state.currentFolder = util::FileSystem::PathToUtf8(root);
    state.treeShowFiles = false;
    state.iconSize = 56;
    panel.OnLoadSettings(settings);
    Frame(panel, 800);
    EXPECT_NE(Frame(panel, 800).find(name), std::string::npos);
}

/// @brief グリッドの名前をクリックしても、サムネイルと同じアセットが Inspector に渡る。
TEST_F(EditorPanelToolbarTest, ClickingGridCaptionSelectsTheAsset)
{
    const auto root = File("Palette");
    std::filesystem::create_directories(root);
    const auto file = root / "Sample.txt";
    std::ofstream fileStream(file);
    fileStream.put('x');
    fileStream.close();
    ASSERT_TRUE(fileStream.good());

    constexpr float PANEL_WIDTH = 800.0f;
    constexpr float ICON_SIZES[] = {64.0f, 112.0f};
    const std::string rootPath = util::FileSystem::PathToUtf8(root);
    const std::string path = util::FileSystem::NormalizePathSeparators(util::FileSystem::PathToUtf8(file));
    editor::AssetBrowserPanel panel(rootPath);
    editor::EditorSettings settings;
    auto& state = settings.AssetBrowserPanelAt(0);
    state.currentFolder = rootPath;
    state.treeShowFiles = false;
    auto& io = ImGui::GetIO();

    for (float iconSize : ICON_SIZES) {
        SCOPED_TRACE(iconSize);
        editor::ClearAssetSelection(Context());
        state.iconSize = iconSize;
        panel.OnLoadSettings(settings);
        io.AddMousePosEvent(io.DisplaySize.x - 1.0f, io.DisplaySize.y - 1.0f);
        Frame(panel, PANEL_WIDTH);
        const std::string rendered = Frame(panel, PANEL_WIDTH);
        ASSERT_NE(rendered.find("Sample.txt"), std::string::npos) << rendered;

        auto* window = ImGui::FindWindowByName("Asset Browser###Asset Browser");
        ASSERT_NE(window, nullptr);
        const ImGuiID contentId = window->GetID("##content");
        const auto& children = window->DC.ChildWindows;
        const auto child = std::find_if(children.begin(), children.end(), [contentId](const ImGuiWindow* candidate) {
            return candidate->ChildId == contentId;
        });
        ASSERT_NE(child, children.end());
        auto* content = *child;
        const ImGuiID tile = ImHashStr("##icon", 0, content->GetID(path.c_str()));

        // 1 枚だけのグリッドでは内容の下端が名前欄の下端になる。描画済みの位置から各欄の中央を求める。
        const float labelHeight = ImGui::GetFrameHeight();
        const float captionBottom = content->DC.CursorMaxPos.y;
        const float x = content->DC.CursorStartPos.x + iconSize * 0.5f;
        const ImVec2 caption{x, captionBottom - labelHeight * 0.5f};
        const ImVec2 thumbnail{x, captionBottom - labelHeight - ImGui::GetStyle().ItemSpacing.y - iconSize * 0.5f};
        ASSERT_TRUE(content->InnerClipRect.Contains(caption));
        ASSERT_TRUE(content->InnerClipRect.Contains(thumbnail));

        // ツールバーを通過せずにカードへ移し、止まる前にツールチップが開かないことを確認する。
        io.AddMousePosEvent(thumbnail.x, thumbnail.y);
        Frame(panel, PANEL_WIDTH);
        ASSERT_EQ(ImGui::GetCurrentContext()->HoveredId, tile);
        for (const auto* popup : ImGui::GetCurrentContext()->Windows) {
            EXPECT_FALSE(popup->Active && (popup->Flags & ImGuiWindowFlags_Tooltip))
                << "Entering a card should leave the grid visible";
        }

        io.AddMousePosEvent(caption.x, caption.y);
        Frame(panel, PANEL_WIDTH);
        ASSERT_EQ(ImGui::GetCurrentContext()->HoveredId, tile);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        Frame(panel, PANEL_WIDTH);
        EXPECT_TRUE(Context().selectedAssetPath.empty());
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        Frame(panel, PANEL_WIDTH);
        EXPECT_TRUE(util::FileSystem::SamePathText(Context().selectedAssetPath, path));
    }
}

/// @brief フォルダーの原画を配布する全サイズで読み込み、背景の透過と本体の不透明性を保つ。
TEST_F(EditorPanelToolbarTest, ShipsTransparentFolderArtworkAtEveryDisplaySize)
{
    const auto directory = std::filesystem::path(FBZZ_SOURCE_DIR) / "Assets/Icons/AssetBrowser";
    for (const int size : {16, 24, 32, 64, 128, 256}) {
        const auto image = editor::assetthumbnail::Decode(directory / ("Folder" + std::to_string(size) + ".png"));
        ASSERT_EQ(image.state, editor::assetthumbnail::State::READY) << size;
        ASSERT_EQ(image.width, size);
        ASSERT_EQ(image.height, size);
        EXPECT_EQ(image.rgba[3], 0);
        EXPECT_GT(image.rgba[4 * (size / 2 * size + size / 2) + 3], 245);
    }
}

/// @brief 同じ名前のまま Component を追加しても、階層行と検索結果の両方が種類の変化を描く。
TEST_F(EditorPanelToolbarTest, UpdatesObjectIconInBothHierarchyViews)
{
    auto& object = AttachScene().CreateGameObject("Subject");
    editor::SceneHierarchyPanel panel;
    editor::OperatorRegistry registry;
    editor::UndoStack undo;
    editor::OpContext operation{ Context(), undo };
    panel.OnRegisterOperators(registry);
    const auto geometry = [&](const char* search) {
        editor::OpArgs args;
        args.Set("search", std::string(search));
        EXPECT_TRUE(registry.Invoke("hierarchy.view.configure", operation, args).ok);
        Frame(panel);
        Frame(panel);
        return ImGui::GetDrawData()->TotalVtxCount;
    };
    const auto tree = geometry("");
    const auto filtered = geometry("Sub");
    object.AddComponent<scene::CameraComponent>();
    EXPECT_NE(tree, geometry(""));
    EXPECT_NE(filtered, geometry("Sub"));
}

}
