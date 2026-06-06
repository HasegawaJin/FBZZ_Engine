// FBZZ Engine
// EditorApp.cpp | fbzz::editor
// エディター全体のライフサイクル管理 + DockSpace
//
// ファイル構成:
//   EditorApp.cpp         - Init / Shutdown / OpenProject / BeginFrame / EndFrame / RenderPanels
//   EditorApp_Scene.cpp   - シーン I/O・ダーティ追跡・ホットリロード
//   EditorApp_MenuBar.cpp - メインメニューバーの構築・ホットキー登録
#include <Editor/EditorApp.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/EditorTaskOverlay.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/FileDialog.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Panels/SceneHierarchyPanel.hpp>
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/Panels/ViewportPanel.hpp>
#include <Editor/Panels/ConsolePanel.hpp>
#include <Editor/Panels/AssetBrowserPanel.hpp>
#include <Editor/Panels/StatusBar.hpp>
#include <Editor/Panels/ProjectSettingsPanel.hpp>
#include <Editor/Panels/BuildSettingsPanel.hpp>
#include <Editor/Panels/AnalysisPanel.hpp>
#include "Tools/TerrainTool.hpp"
#include "Tools/WaterTool.hpp"
#include <Engine/Core/Application.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/RenderDebugOverlay.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <ImGuizmo.h>
#include <imgui_impl_win32.h>
#include <toml++/toml.hpp>
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

// imgui_impl_win32.h では #if 0 で隠されているため手動で前方宣言する
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fbzz::editor {

static constexpr const char* SETTINGS_DIR  = "editor_config";
static constexpr const char* SETTINGS_PATH = "editor_config/editor_settings.toml";

namespace {

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    return wide;
}

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string utf8(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), size, nullptr, nullptr);
    return utf8;
}

bool IsEditorUICanvas(const scene::UICanvas& canvas)
{
    return canvas.enabled
        && (canvas.renderMode == scene::UIRenderMode::ScreenSpaceOverlay
            || canvas.renderMode == scene::UIRenderMode::ScreenSpaceCamera);
}

std::filesystem::path MakeAbsolutePath(const std::filesystem::path& path)
{
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    return ec ? path.lexically_normal() : absolute.lexically_normal();
}

std::string ToLowerAscii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

std::string ResolveScenePathForProject(const std::string& projectRoot, const std::string& scenePath)
{
    if (scenePath.empty()) return {};
    const std::filesystem::path rootPath = std::filesystem::path(Utf8ToWide(projectRoot));
    std::filesystem::path candidate = std::filesystem::path(Utf8ToWide(scenePath));
    if (!candidate.is_absolute())
        candidate = rootPath / candidate;
    return WideToUtf8(MakeAbsolutePath(candidate).wstring());
}

bool IsScenePathInsideProject(const std::string& projectRoot, const std::string& scenePath)
{
    if (projectRoot.empty() || scenePath.empty()) return false;
    if (ToLowerAscii(util::FileSystem::GetExtension(scenePath)) != ".fbzz") return false;
    if (!util::FileSystem::Exists(scenePath)) return false;

    const std::filesystem::path rootPath  = MakeAbsolutePath(std::filesystem::path(Utf8ToWide(projectRoot)));
    const std::filesystem::path sceneFs   = MakeAbsolutePath(std::filesystem::path(Utf8ToWide(scenePath)));

    const std::string rootText  = ToLowerAscii(WideToUtf8(rootPath.wstring()));
    const std::string sceneText = ToLowerAscii(WideToUtf8(sceneFs.wstring()));
    std::string prefix = rootText;
    if (!prefix.empty() && prefix.back() != '\\' && prefix.back() != '/')
        prefix += '\\';

    return sceneText == rootText || sceneText.rfind(prefix, 0) == 0;
}

void LoadRuntimeBuildMetadata(EditorContext& ctx)
{
    ctx.projectBuildRoot.clear();
    ctx.standaloneTargetName = "SandboxStandalone";
    if (ctx.projectRoot.empty()) return;

    std::string projectText;
    if (!util::FileSystem::ReadText(ctx.projectRoot + "/.fbzz_proj", projectText))
        return;

    toml::parse_result result = toml::parse(projectText);
    if (!result) return;

    const toml::table& table = result.table();
    const std::string buildRoot = table["project"]["build_root"].value_or(std::string{});
    if (!buildRoot.empty() && buildRoot.rfind("{{", 0) != 0) {
        std::filesystem::path path(Utf8ToWide(buildRoot));
        if (!path.is_absolute())
            path = std::filesystem::path(Utf8ToWide(ctx.projectRoot)) / path;
        ctx.projectBuildRoot = WideToUtf8(path.lexically_normal().wstring());
    }

    const std::string standaloneTarget = table["project"]["standalone_target_name"].value_or(std::string{});
    if (!standaloneTarget.empty() && standaloneTarget.rfind("{{", 0) != 0) {
        ctx.standaloneTargetName = standaloneTarget;
        return;
    }

    const std::string targetName = table["project"]["target_name"].value_or(std::string{});
    if (!targetName.empty() && targetName.rfind("{{", 0) != 0) {
        ctx.standaloneTargetName = targetName + "Standalone";
        ctx.projectTargetName    = targetName;
    }

    const std::string engineRoot = table["engine"]["root"].value_or(std::string{});
    if (!engineRoot.empty() && engineRoot.rfind("{{", 0) != 0) {
        std::filesystem::path path(Utf8ToWide(engineRoot));
        if (!path.is_absolute())
            path = std::filesystem::path(Utf8ToWide(ctx.projectRoot)) / path;
        ctx.engineRoot = WideToUtf8(path.lexically_normal().wstring());
    }
}

} // namespace

// =============================================================================
// 初期化 / 終了
// =============================================================================

// コンストラクタ・デストラクタをここで定義する。
// WHY: EditorApp.hpp は TerrainTool を前方宣言のみにとどめているため、
//      ヘッダーのインクルード先 (main.cpp 等) では TerrainTool の定義が見えない。
//      std::unique_ptr のデストラクタは完全型を要求するので、
//      TerrainTool.hpp をインクルードしているこの .cpp で定義する必要がある。
EditorApp::EditorApp()  = default;
EditorApp::~EditorApp() = default;

bool EditorApp::Init(renderer::IRenderer& renderer, renderer::IImGuiRenderer& imguiRenderer, renderer::ResourceManager& resources, core::Window& window)
{
    m_hwnd          = window.GetHandle();
    m_renderer      = &renderer;
    m_imguiRenderer = &imguiRenderer;
    m_resources     = &resources;

    window.SetWndProcHook([this](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> bool {
        if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp) != 0)
            return true;
        if (msg == WM_CLOSE) {
            RequestExit();
            return true;
        }
        return false;
    });

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuizmo::SetImGuiContext(ImGui::GetCurrentContext());
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "editor_config/imgui_layout.ini";

    // ini がなければデフォルトレイアウトをメモリから適用する。
    // LoadIniSettingsFromMemory は SettingsLoaded フラグを立てるため、
    // その後の NewFrame() でファイルから上書きされることはない。
    if (!util::FileSystem::Exists("editor_config/imgui_layout.ini")) {
        static constexpr const char* DEFAULT_IMGUI_LAYOUT =
            "[Window][##statusbar]\n"
            "Pos=0,970\n"
            "Size=1904,32\n"
            "Collapsed=0\n"
            "\n"
            "[Window][##DockSpaceHost]\n"
            "Pos=0,0\n"
            "Size=1904,993\n"
            "Collapsed=0\n"
            "\n"
            "[Window][Debug##Default]\n"
            "Pos=60,60\n"
            "Size=400,400\n"
            "Collapsed=0\n"
            "\n"
            "[Window][Scene Hierarchy]\n"
            "Pos=0,19\n"
            "Size=209,974\n"
            "Collapsed=0\n"
            "DockId=0x00000001,0\n"
            "\n"
            "[Window][Inspector]\n"
            "Pos=1675,19\n"
            "Size=229,974\n"
            "Collapsed=0\n"
            "DockId=0x00000004,0\n"
            "\n"
            "[Window][Scene]\n"
            "Pos=211,19\n"
            "Size=1462,667\n"
            "Collapsed=0\n"
            "DockId=0x00000007,0\n"
            "\n"
            "[Window][Game]\n"
            "Pos=211,19\n"
            "Size=1462,667\n"
            "Collapsed=0\n"
            "DockId=0x00000007,2\n"
            "\n"
            "[Window][UI]\n"
            "Pos=211,19\n"
            "Size=1462,667\n"
            "Collapsed=0\n"
            "DockId=0x00000007,1\n"
            "\n"
            "[Window][Console]\n"
            "Pos=211,688\n"
            "Size=1462,305\n"
            "Collapsed=0\n"
            "DockId=0x00000005,1\n"
            "\n"
            "[Window][Asset Browser]\n"
            "Pos=211,688\n"
            "Size=1462,305\n"
            "Collapsed=0\n"
            "DockId=0x00000005,0\n"
            "\n"
            "[Window][Project Settings]\n"
            "Pos=211,19\n"
            "Size=1462,594\n"
            "Collapsed=0\n"
            "DockId=0x00000007,3\n"
            "\n"
            "[Window][New Scene]\n"
            "Pos=820,459\n"
            "Size=264,75\n"
            "Collapsed=0\n"
            "\n"
            "[Docking][Data]\n"
            "DockSpace       ID=0xFF535877 Window=0xE26AC72C Pos=0,19 Size=1904,974 Split=X\n"
            "  DockNode      ID=0x00000001 Parent=0xFF535877 SizeRef=209,1042 HiddenTabBar=1 Selected=0xB8729153\n"
            "  DockNode      ID=0x00000006 Parent=0xFF535877 SizeRef=1703,1042 Split=X\n"
            "    DockNode    ID=0x00000003 Parent=0x00000006 SizeRef=1478,1042 Split=Y\n"
            "      DockNode  ID=0x00000007 Parent=0x00000003 SizeRef=1464,683 CentralNode=1 Selected=0xD1EB2482\n"
            "      DockNode  ID=0x00000005 Parent=0x00000003 SizeRef=1464,305 Selected=0x36AF052B\n"
            "    DockNode    ID=0x00000004 Parent=0x00000006 SizeRef=229,1042 HiddenTabBar=1 Selected=0x36DC96AB\n";
        ImGui::LoadIniSettingsFromMemory(DEFAULT_IMGUI_LAYOUT);
    }

    EditorTheme::Apply();

    imguiRenderer.ImGuiInit(m_hwnd);

    m_ctx.undoStack   = &m_undoStack;
    m_ctx.playMode    = &m_playMode;
    m_ctx.renderer    = &renderer;
    m_ctx.imguiRenderer = &imguiRenderer;
    m_ctx.resources   = &resources;
    m_ctx.memorySystem = &core::Application::Get().GetMemorySystem();
    m_terrainTool     = std::make_unique<TerrainTool>();
    m_ctx.terrainTool = m_terrainTool.get();
    m_waterTool       = std::make_unique<WaterTool>();
    m_ctx.waterTool   = m_waterTool.get();
    m_ctx.markSceneDirty  = [this]() { MarkSceneDirty(); };
    m_ctx.requestOpenScene = [this](const std::string& path) { RequestOpenScenePath(path); };

    m_panels.push_back(std::make_unique<SceneHierarchyPanel>());
    m_panels.push_back(std::make_unique<InspectorPanel>());
    {
        auto vp = std::make_unique<ViewportPanel>(ViewportPanel::Kind::Scene);
        m_sceneViewportPanel = vp.get();
        m_panels.push_back(std::move(vp));
    }
    {
        auto vp = std::make_unique<ViewportPanel>(ViewportPanel::Kind::Game);
        m_gameViewportPanel = vp.get();
        m_panels.push_back(std::move(vp));
    }
    {
        auto vp = std::make_unique<ViewportPanel>(ViewportPanel::Kind::UI);
        m_uiViewportPanel = vp.get();
        m_panels.push_back(std::move(vp));
    }
    m_panels.push_back(std::make_unique<ConsolePanel>(m_consoleSink));
    {
        auto assets = std::make_unique<AssetBrowserPanel>("Assets");
        m_assetBrowserPanel = assets.get();
        m_panels.push_back(std::move(assets));
    }
    m_panels.push_back(std::make_unique<StatusBar>());
    {
        auto ps = std::make_unique<ProjectSettingsPanel>();
        ps->visible = false;
        m_projectSettingsPanel = ps.get();
        m_panels.push_back(std::move(ps));
    }
    {
        auto bs = std::make_unique<BuildSettingsPanel>();
        bs->visible = false;
        m_buildSettingsPanel = bs.get();
        m_panels.push_back(std::move(bs));
    }
    {
        auto analysis = std::make_unique<AnalysisPanel>();
        analysis->visible = false;
        m_analysisPanel = analysis.get();
        m_panels.push_back(std::move(analysis));
    }

    for (auto& panel : m_panels)
        panel->OnInit(m_ctx);

    RegisterDefaultHotkeys();

    util::FileSystem::EnsureDirectory(SETTINGS_DIR);
    m_settings.Load(SETTINGS_PATH, m_ctx.projectRoot);

    // --- EditorSettings → EditorContext への全フィールド適用 ---------------
    // WHY: EditorSettings は TOML から読んだ raw 値を保持し、
    //      EditorContext はパネル・メインループが参照するライブ値を保持する。
    //      Init 時に一括コピーし、Shutdown 時に逆方向で書き戻す設計にすることで
    //      両者の責務が明確になり、保存漏れを防げる。
    m_ctx.showGrid          = m_settings.showGrid;
    m_ctx.gridSize          = m_settings.gridSize;
    m_ctx.snapEnabled       = m_settings.snapEnabled;
    m_ctx.snapDistance      = m_settings.snapDistance;
    m_ctx.gizmoMode         = static_cast<EditorContext::GizmoMode>(m_settings.gizmoMode);
    m_ctx.gizmoSpace        = static_cast<EditorContext::GizmoSpace>(m_settings.gizmoSpace);
    m_ctx.showLightRange    = m_settings.showLightRange;
    m_ctx.showSkeleton      = m_settings.showSkeleton;
    m_ctx.showStats         = m_settings.showStats;
    m_ctx.hotReloadEnabled  = m_settings.hotReloadEnabled;
    m_ctx.gameViewportAspect = static_cast<EditorContext::GameViewportAspect>(m_settings.gameViewportAspect);
    m_ctx.cameraSpeed       = m_settings.cameraSpeed;
    m_ctx.cameraSensitivity = m_settings.cameraSensitivity;

    // 初回 RT をウィンドウサイズで生成する
    m_sceneViewportRT = resources.CreateRenderTarget(window.GetWidth(), window.GetHeight());
    m_gameViewportRT  = resources.CreateRenderTarget(window.GetWidth(), window.GetHeight());
    if (m_sceneViewportPanel) {
        m_sceneViewportPanel->hdrRT     = m_sceneViewportRT;
        m_sceneViewportPanel->renderer  = &renderer;
        m_sceneViewportPanel->resources = &resources;
    }
    if (m_gameViewportPanel) {
        m_gameViewportPanel->hdrRT     = m_gameViewportRT;
        m_gameViewportPanel->renderer  = &renderer;
        m_gameViewportPanel->resources = &resources;
    }
    if (m_uiViewportPanel) {
        // WHY: UI Viewport は Game View の完成フレームを背景として共有する。
        //      UI 専用 RT を別描画すると、Clear 順や RenderGraph 経路の差で青い空 RT が表示される。
        //      編集用ガイドとギズモだけを ImGui 側で重ねることで、Game View と同じ出力を見ながら UI を編集できる。
        m_uiViewportPanel->hdrRT     = m_gameViewportRT;
        m_uiViewportPanel->renderer  = &renderer;
        m_uiViewportPanel->resources = &resources;
    }

    FBZZ_LOG_INFO("EditorApp init done");
    UpdateWindowTitle();
    return true;
}

void EditorApp::Shutdown()
{
    // DLL 仮想デストラクタが DLL コードを参照するため、パネル・シーンより先にアンロードする。
    m_scriptDll.Unload(m_ctx.activeScene);

    for (auto& panel : m_panels)
        panel->OnShutdown();

    // --- EditorContext → EditorSettings への書き戻し ----------------------
    // WHY: パネルやメインループは EditorContext のライブ値を直接変更する。
    //      Shutdown 時にここで一括書き戻さないと、起動時に読んだ初期値が
    //      そのまま保存されてしまい、ユーザーの変更が永続化されない。
    m_settings.showGrid           = m_ctx.showGrid;
    m_settings.gridSize           = m_ctx.gridSize;
    m_settings.snapEnabled        = m_ctx.snapEnabled;
    m_settings.snapDistance       = m_ctx.snapDistance;
    m_settings.gizmoMode          = static_cast<int>(m_ctx.gizmoMode);
    m_settings.gizmoSpace         = static_cast<int>(m_ctx.gizmoSpace);
    m_settings.showLightRange     = m_ctx.showLightRange;
    m_settings.showSkeleton       = m_ctx.showSkeleton;
    m_settings.showStats          = m_ctx.showStats;
    m_settings.hotReloadEnabled   = m_ctx.hotReloadEnabled;
    m_settings.gameViewportAspect = static_cast<int>(m_ctx.gameViewportAspect);
    m_settings.cameraSpeed        = m_ctx.cameraSpeed;
    m_settings.cameraSensitivity  = m_ctx.cameraSensitivity;

    m_settings.Save(SETTINGS_PATH, m_ctx.projectRoot);
    m_ctx.projectSettings.Save(m_projectSettingsPath);
    m_sceneViewportRT = {};
    m_gameViewportRT  = {};
    m_imguiRenderer->ImGuiShutdown();
    ImGui::DestroyContext();
}

bool EditorApp::OpenProject(const std::string& projectRoot, const std::string& projectSettingsPath, const std::string& scenePath)
{
    if (!m_ctx.activeScene || !m_resources) return false;

    m_projectRoot      = projectRoot;
    m_ctx.projectRoot  = projectRoot;
    LoadRuntimeBuildMetadata(m_ctx);
    if (m_assetBrowserPanel && !m_projectRoot.empty())
        m_assetBrowserPanel->SetRootPath(m_projectRoot + "/Assets");

    if (!projectSettingsPath.empty()) {
        m_projectSettingsPath = projectSettingsPath;
        m_ctx.projectSettings.Load(m_projectSettingsPath);
        core::Time::SetTargetFps(m_ctx.projectSettings.app.targetFps);
    }

    // WHY: SceneIO::Load() がシーン内の ScriptComponent を復元する際に
    //      ScriptFactory からファクトリ関数を引く。DLL が未ロードだとスクリプトインスタンスが
    //      生成されず Play 中も OnUpdate() が呼ばれない。
    //      必ずシーンロードより前に DLL をロードして ScriptFactory を準備する。
    InitScriptDll();

    std::string sceneToOpen = scenePath;
    const std::string lastScenePath = ResolveScenePathForProject(projectRoot, m_settings.lastScenePath);
    if (IsScenePathInsideProject(projectRoot, lastScenePath)) {
        sceneToOpen = lastScenePath;
    } else if (sceneToOpen.empty() && !m_ctx.projectSettings.runtime.startScene.empty()) {
        sceneToOpen = ResolveScenePathForProject(projectRoot, m_ctx.projectSettings.runtime.startScene);
    }

    if (!sceneToOpen.empty()) {
        if (!SceneIO::Load(*m_ctx.activeScene, sceneToOpen)) {
            FBZZ_LOG_ERROR("Open project scene failed: %s", sceneToOpen.c_str());
            return false;
        }
        m_settings.lastScenePath = sceneToOpen;
        m_ctx.currentScenePath   = sceneToOpen;
        m_ctx.selectedEntities.clear();
        RebuildEditorUIFromScene();
        CaptureCleanScene();
    }

    FBZZ_LOG_INFO("Opened project: %s", m_projectRoot.c_str());
    UpdateWindowTitle();

    return true;
}

void EditorApp::RebuildEditorUIFromScene()
{
    // WHY: UI Viewport の編集対象は EditorContext の一時状態であり、.fbzz には保存しない。
    //      シーンを読み込んだ直後に Scene 内の UICanvas から復元しないと、初回表示で UI 編集ガイドや
    //      pick 対象が前シーンの無効 ID のままになり、Canvas をクリックするまで再構築されない。
    m_ctx.activeUICanvas = scene::EntityID::INVALID;
    if (!m_ctx.activeScene) {
        return;
    }

    for (auto& go : m_ctx.activeScene->GameObjects()) {
        auto* canvas = go.GetComponent<scene::UICanvas>();
        if (!canvas || !IsEditorUICanvas(*canvas)) {
            continue;
        }

        m_ctx.activeUICanvas = go.GetID();
        FBZZ_LOG_DEBUG("EditorUI: active Canvas restored from scene data");
        return;
    }
}

// =============================================================================
// ウィンドウタイトル
// =============================================================================

void EditorApp::UpdateWindowTitle()
{
    if (!m_hwnd) return;

    const std::string sceneName = m_ctx.currentScenePath.empty()
        ? "Untitled"
        : util::FileSystem::GetFilename(m_ctx.currentScenePath);

    if (m_titleInitialized &&
        m_lastTitleDirty     == m_ctx.sceneDirty &&
        m_lastTitleScenePath == m_ctx.currentScenePath)
        return;

    m_titleInitialized   = true;
    m_lastTitleDirty     = m_ctx.sceneDirty;
    m_lastTitleScenePath = m_ctx.currentScenePath;

    std::string title = "FBZZ Editor - " + sceneName;
    if (m_ctx.sceneDirty) title += "*";
    SetWindowTextW(m_hwnd, Utf8ToWide(title).c_str());
}

// =============================================================================
// フレーム
// =============================================================================

void EditorApp::BeginFrame()
{
    // Viewport パネルサイズが前フレームで変わった場合は RT を再生成する。
    // main ループの「シーン描画」より前に呼ぶことで、RT のサイズが確定した状態で
    // シーンをレンダリングでき、リサイズ直後のフレームで古い解像度の画像が表示されるのを防ぐ。
    ResizeViewportRTsIfNeeded();

    m_imguiRenderer->ImGuiNewFrame();
    ImGui::NewFrame();

    // WHY: Unity 同様、Play 中・Pause 中はエディターとの区別を一目で把握できるようにする。
    //      ImGui のスタイルカラーをフレームごとに上書きすることで
    //      全ウィンドウ背景にティントを掛けられる。
    //      Push/Pop ではなくフレームごと直接書き換えることで、
    //      ウィンドウ単位でなく全体へ適用できる。
    {
        ImGuiStyle& style = ImGui::GetStyle();
        if (m_playMode.IsPlaying()) {
            // Play 中: 青系ティント (#1A2433)
            style.Colors[ImGuiCol_WindowBg]  = { 0.10f, 0.14f, 0.20f, 1.0f };
            style.Colors[ImGuiCol_ChildBg]   = { 0.08f, 0.12f, 0.18f, 1.0f };
            style.Colors[ImGuiCol_MenuBarBg] = { 0.07f, 0.10f, 0.16f, 1.0f };
        } else if (m_playMode.IsPaused()) {
            // Pause 中: 黄系ティント (#2E2614)
            style.Colors[ImGuiCol_WindowBg]  = { 0.18f, 0.16f, 0.10f, 1.0f };
            style.Colors[ImGuiCol_ChildBg]   = { 0.15f, 0.13f, 0.08f, 1.0f };
            style.Colors[ImGuiCol_MenuBarBg] = { 0.13f, 0.11f, 0.07f, 1.0f };
        } else {
            // Editor モードのデフォルト色を毎フレーム復元する
            style.Colors[ImGuiCol_WindowBg]  = { 0.173f, 0.173f, 0.173f, 1.0f }; // BG_BASE
            style.Colors[ImGuiCol_ChildBg]   = { 0.141f, 0.141f, 0.141f, 1.0f }; // BG_DARK
            style.Colors[ImGuiCol_MenuBarBg] = { 0.102f, 0.102f, 0.102f, 1.0f }; // BG_DARKEST
        }
    }

    ImGuizmo::BeginFrame();
    m_hotkeys.ProcessInput();
    CheckHotReload();
    CheckScriptDirtyAndRebuild();
    CheckHlslDirty();
    RefreshSceneDirtyState(false);

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoDocking             |
        ImGuiWindowFlags_NoTitleBar            |
        ImGuiWindowFlags_NoCollapse            |
        ImGuiWindowFlags_NoResize              |
        ImGuiWindowFlags_NoMove                |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus            |
        ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    { 0.0f, 0.0f });
    ImGui::Begin("##DockSpaceHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    BuildMenuBar(m_ctx);
    BuildPlayToolbar(m_ctx);

    ImGuiID dockId = ImGui::GetID("MainDockSpace");
    ImGui::DockSpace(dockId, { 0, 0 }, ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_AutoHideTabBar);

    // ModalDialog::OpenPopup は ImGui ウィンドウ (Begin/End) のスコープ内でしか機能しない。
    // DockSpaceHost ウィンドウの内側に置くことでその制約を満たす。
    ModalDialog::OnRender();

    ImGui::End();
}

void EditorApp::RenderPanels(EditorContext& ctx)
{
    for (auto& panel : m_panels)
        if (panel->visible) panel->OnRender(ctx);

    // GPU レンダリング完了後・ImGui フレーム内のここで描画する。
    // RenderSystem は GPU 実行中のため直接 ImGui を呼べず、スナップショットだけ保存している。
    if (m_imguiRenderer && m_resources)
        renderer::RenderDebugOverlay::DrawIfEnabled(*m_imguiRenderer, *m_resources);

    if (ctx.requestOpenProjectSettings) {
        if (m_projectSettingsPanel) m_projectSettingsPanel->visible = true;
        ctx.requestOpenProjectSettings = false;
    }

    if (ctx.requestOpenBuildSettings) {
        ctx.requestOpenBuildSettings = false;
        if (m_buildSettingsPanel) {
            m_buildSettingsPanel->visible = true;
            ImGui::SetNextWindowFocus();
        }
    }

    if (ctx.requestOpenAnalysis) {
        ctx.requestOpenAnalysis = false;
        if (m_analysisPanel) {
            m_analysisPanel->visible = true;
            ImGui::SetNextWindowFocus();
        }
    }

    // WHY: すべての通常ウィンドウの後に呼ぶことで、オーバーレイが最前面に描画される。
    //      IsActive() == false のときは何もしないのでパネルのないフレームでも安全。
    EditorTaskOverlay::Render();
}

void EditorApp::EndFrame(renderer::IImGuiRenderer& imguiRenderer)
{
    ImGui::Render();
    imguiRenderer.ImGuiRenderDrawData();
}

// =============================================================================
// Viewport RT リサイズ
// =============================================================================

void EditorApp::ResizeViewportRTsIfNeeded()
{
    if (!m_renderer) return;

    auto resizeRT = [this](renderer::ResourceHandle<renderer::RenderTargetTag>& rt,
                           ViewportPanel* panel,
                           float width,
                           float height) {
        if (!rt.IsValid() || !panel) return;

        const uint32_t vpW = static_cast<uint32_t>(width);
        const uint32_t vpH = static_cast<uint32_t>(height);
        if (vpW == 0 || vpH == 0) return;
        auto* currentRT = m_resources->Get(rt);
        if (currentRT && vpW == currentRT->GetWidth() && vpH == currentRT->GetHeight()) return;

        rt         = m_resources->CreateRenderTarget(vpW, vpH);
        panel->hdrRT = rt;
    };

    resizeRT(m_sceneViewportRT, m_sceneViewportPanel, m_ctx.viewportWidth,     m_ctx.viewportHeight);
    resizeRT(m_gameViewportRT,  m_gameViewportPanel,  m_ctx.gameViewportWidth,  m_ctx.gameViewportHeight);
    // WHY: UI Viewport は専用 RT を持たず、Game View の完成済み RT を参照する。
    //      リサイズ後もパネル側のハンドルを張り直して、古い RT 参照が残らないようにする。
    if (m_uiViewportPanel)
        m_uiViewportPanel->hdrRT = m_gameViewportRT;
}

} // namespace fbzz::editor
