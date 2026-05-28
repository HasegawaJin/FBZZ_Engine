// FBZZ Engine
// EditorApp.cpp | fbzz::editor
// エディター全体のライフサイクル管理 + DockSpace + MenuBar
#include <Editor/EditorApp.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/FileDialog.hpp>
#include <Editor/Util/SceneSerializer.hpp>
#include <Editor/Panels/SceneHierarchyPanel.hpp>
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/Panels/ViewportPanel.hpp>
#include <Editor/Panels/ConsolePanel.hpp>
#include <Editor/Panels/AssetBrowserPanel.hpp>
#include <Editor/Panels/StatusBar.hpp>
#include <Editor/Panels/ProjectSettingsPanel.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/RenderDebugOverlay.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <ImGuizmo.h>
#include <imgui_impl_win32.h>
#include <Windows.h>
#include <utility>

// imgui_impl_win32.h では #if 0 で隠されているため手動で前方宣言する
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fbzz::editor {

static constexpr const char* SETTINGS_DIR             = "editor_config";
static constexpr const char* SETTINGS_PATH            = "editor_config/editor_settings.toml";

namespace {

const FileFilter SCENE_FILTER{ "FBZZ Scene", "*.fbzz" };

std::string WithFbzzExtension(const std::string& path)
{
    if (path.empty() || !util::FileSystem::GetExtension(path).empty()) return path;
    return path + ".fbzz";
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) return {};

    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};

    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    return wide;
}

} // namespace

bool EditorApp::Init(renderer::IRenderer& renderer, renderer::ResourceManager& resources, core::Window& window)
{
    m_hwnd     = window.GetHandle();
    m_renderer = &renderer;
    m_resources = &resources;

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

    ImGui::StyleColorsDark();

    renderer.ImGuiInit(m_hwnd);

    m_ctx.undoStack = &m_undoStack;
    m_ctx.playMode  = &m_playMode;
    m_ctx.markSceneDirty = [this]() { MarkSceneDirty(); };
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

    for (auto& panel : m_panels)
        panel->OnInit(m_ctx);

    RegisterDefaultHotkeys();

    util::FileSystem::EnsureDirectory(SETTINGS_DIR);
    m_settings.Load(SETTINGS_PATH);
    m_ctx.projectSettings.Load(m_projectSettingsPath);
    core::Time::SetTargetFps(m_ctx.projectSettings.app.targetFps);
    m_ctx.showGrid    = m_settings.showGrid;
    m_ctx.snapEnabled = m_settings.snapEnabled;

    // 初回 RT をウィンドウサイズで生成する
    m_sceneViewportRT = resources.CreateRenderTarget(window.GetWidth(), window.GetHeight());
    m_gameViewportRT = resources.CreateRenderTarget(window.GetWidth(), window.GetHeight());
    m_uiViewportRT = resources.CreateRenderTarget(window.GetWidth(), window.GetHeight());
    if (m_sceneViewportPanel)
    {
        m_sceneViewportPanel->hdrRT = m_sceneViewportRT;
        m_sceneViewportPanel->renderer = &renderer;
        m_sceneViewportPanel->resources = &resources;
    }
    if (m_gameViewportPanel)
    {
        m_gameViewportPanel->hdrRT = m_gameViewportRT;
        m_gameViewportPanel->renderer = &renderer;
        m_gameViewportPanel->resources = &resources;
    }
    if (m_uiViewportPanel)
    {
        m_uiViewportPanel->hdrRT = m_uiViewportRT;
        m_uiViewportPanel->renderer = &renderer;
        m_uiViewportPanel->resources = &resources;
    }

    FBZZ_LOG_INFO("EditorApp init done");
    UpdateWindowTitle();
    return true;
}

bool EditorApp::OpenProject(const std::string& projectRoot, const std::string& projectSettingsPath, const std::string& scenePath)
{
    if (!m_ctx.activeScene || !m_resources) return false;

    m_projectRoot = projectRoot;
    m_ctx.projectRoot = projectRoot;
    if (m_assetBrowserPanel && !m_projectRoot.empty()) {
        m_assetBrowserPanel->SetRootPath(m_projectRoot + "/Assets");
    }
    if (!projectSettingsPath.empty()) {
        m_projectSettingsPath = projectSettingsPath;
        m_ctx.projectSettings.Load(m_projectSettingsPath);
        core::Time::SetTargetFps(m_ctx.projectSettings.app.targetFps);
    }

    if (!scenePath.empty()) {
        if (!SceneSerializer::Load(*m_ctx.activeScene, scenePath)) {
            FBZZ_LOG_ERROR("Open project scene failed: %s", scenePath.c_str());
            return false;
        }
        m_settings.lastScenePath = scenePath;
        m_ctx.currentScenePath = scenePath;
        m_ctx.selectedEntities.clear();
        CaptureCleanScene();
    }

    FBZZ_LOG_INFO("Opened project: %s", m_projectRoot.c_str());
    UpdateWindowTitle();
    return true;
}

void EditorApp::Shutdown()
{
    for (auto& panel : m_panels)
        panel->OnShutdown();

    m_settings.Save(SETTINGS_PATH);
    m_ctx.projectSettings.Save(m_projectSettingsPath);
    m_sceneViewportRT = {};
    m_gameViewportRT = {};
    m_uiViewportRT = {};
    m_renderer->ImGuiShutdown();
    ImGui::DestroyContext();
}

void EditorApp::BeginFrame()
{
    // Viewport パネルサイズが前フレームで変わった場合は RT を再生成する。
    // main ループの「シーン描画」より前に呼ぶことで、RT のサイズが確定した状態で
    // シーンをレンダリングでき、リサイズ直後のフレームで古い解像度の画像が表示されるのを防ぐ。
    ResizeViewportRTsIfNeeded();

    m_renderer->ImGuiNewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    m_hotkeys.ProcessInput();
    CheckHotReload();
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

    ImGuiID dockId = ImGui::GetID("MainDockSpace");
    ImGui::DockSpace(dockId, { 0, 0 }, ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_AutoHideTabBar);

    BuildMenuBar(m_ctx);

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
    if (m_renderer && m_resources)
        renderer::RenderDebugOverlay::DrawIfEnabled(*m_renderer, *m_resources);

    if (ctx.requestOpenProjectSettings) {
        if (m_projectSettingsPanel) m_projectSettingsPanel->visible = true;
        ctx.requestOpenProjectSettings = false;
    }
}

void EditorApp::EndFrame(renderer::IRenderer& renderer)
{
    ImGui::Render();
    renderer.ImGuiRenderDrawData();
}

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

        rt = m_resources->CreateRenderTarget(vpW, vpH);
        panel->hdrRT = rt;
    };

    resizeRT(m_sceneViewportRT, m_sceneViewportPanel, m_ctx.viewportWidth, m_ctx.viewportHeight);
    resizeRT(m_gameViewportRT, m_gameViewportPanel, m_ctx.gameViewportWidth, m_ctx.gameViewportHeight);
    resizeRT(m_uiViewportRT, m_uiViewportPanel, m_ctx.uiViewportWidth, m_ctx.uiViewportHeight);
}

void EditorApp::BuildMenuBar(EditorContext& ctx)
{
    if (!ImGui::BeginMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Scene"))
            RequestNewScene();
        if (ImGui::MenuItem("Open...", "Ctrl+O", false, ctx.activeScene != nullptr))
            RequestOpenSceneFromDialog();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, ctx.activeScene != nullptr))
            SaveScene();
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, ctx.activeScene != nullptr))
            SaveSceneAsDialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) RequestExit();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        bool canUndo = ctx.undoStack && ctx.undoStack->CanUndo();
        bool canRedo = ctx.undoStack && ctx.undoStack->CanRedo();
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo)) ctx.undoStack->Undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo)) ctx.undoStack->Redo();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        if (ImGui::BeginMenu("Panels")) {
            for (auto& panel : m_panels) {
                if (panel->ShowInViewMenu())
                    ImGui::MenuItem(panel->GetViewMenuName(), nullptr, &panel->visible);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::MenuItem("Grid",        nullptr, &ctx.showGrid);
        ImGui::MenuItem("Light Range", nullptr, &ctx.showLightRange);
        ImGui::MenuItem("Colliders",   nullptr, &ctx.projectSettings.render.showColliders);
        ImGui::MenuItem("Skeleton",    nullptr, &ctx.showSkeleton);
        ImGui::MenuItem("Stats",       nullptr, &ctx.showSceneStats);
        ImGui::MenuItem("Hot Reload",  nullptr, &ctx.hotReloadEnabled);
        ImGui::Separator();
        ImGui::MenuItem("Wireframe",   nullptr, &ctx.projectSettings.render.wireframeMode);
        ImGui::Separator();
        if (ImGui::BeginMenu("Post Process")) {
            ImGui::MenuItem("Shadow",   nullptr, &ctx.projectSettings.render.shadowEnabled);
            auto& pp = ctx.projectSettings.render.postProcess;
            ImGui::MenuItem("Bloom",    nullptr, &pp.bloom.enabled);
            ImGui::MenuItem("Fog",      nullptr, &pp.fog.enabled);
            ImGui::MenuItem("FXAA",     nullptr, &pp.fxaaEnabled);
            ImGui::MenuItem("Color Grading", nullptr, &pp.colorGrading.enabled);
            ImGui::MenuItem("Vignette", nullptr, &pp.vignette.enabled);
            ImGui::MenuItem("Film Grain", nullptr, &pp.filmGrain.enabled);
            ImGui::MenuItem("Chromatic Aberration", nullptr, &pp.lens.chromaticAberrationEnabled);
            ImGui::MenuItem("Lens Distortion", nullptr, &pp.lens.distortionEnabled);
            ImGui::Separator();
            ImGui::SliderFloat("Exposure",    &pp.exposure,   0.1f, 4.0f);
            ImGui::SliderFloat("Bloom Intensity", &pp.bloom.intensity, 0.0f, 3.0f);
            ImGui::SliderFloat("Contrast", &pp.colorGrading.contrast, -1.0f, 1.0f);
            ImGui::SliderFloat("Saturation", &pp.colorGrading.saturation, 0.0f, 2.0f);
            ImGui::SliderFloat("Hue Shift", &pp.colorGrading.hueShift, -180.0f, 180.0f);
            ImGui::SliderFloat("Fog Density", &pp.fog.density, 0.0f, 1.0f);
            ImGui::SliderFloat("Fog Far",     &pp.fog.farDistance,     1.0f, 100.0f);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    PlayModeController* pm = ctx.playMode;
    if (pm && pm->IsInEditor()) {
        if (ImGui::MenuItem("  Play  ") && ctx.activeScene) {
            pm->Play(*ctx.activeScene);
            if (pm->IsPlaying())
                ctx.requestGameViewportFocus = true;
        }
    } else if (pm) {
        if (ImGui::MenuItem("  Stop  ") && ctx.activeScene)
            pm->Stop(*ctx.activeScene);
        if (ImGui::MenuItem(pm->IsPaused() ? " Resume " : " Pause  "))
            pm->Pause();
        if (pm->IsPaused() && ImGui::MenuItem("  Step   "))
            pm->RequestStep();
    }

    ImGui::EndMenuBar();
}

void EditorApp::RegisterDefaultHotkeys()
{
    m_hotkeys.Register({ "Undo", ImGuiKey_Z, true, false, false,
        [this]() { m_undoStack.Undo(); } });
    m_hotkeys.Register({ "Redo", ImGuiKey_Y, true, false, false,
        [this]() { m_undoStack.Redo(); } });
    m_hotkeys.Register({ "Open Scene", ImGuiKey_O, true, false, false,
        [this]() { RequestOpenSceneFromDialog(); } });
    m_hotkeys.Register({ "Save Scene", ImGuiKey_S, true, false, false,
        [this]() { SaveScene(); } });
    m_hotkeys.Register({ "Save Scene As", ImGuiKey_S, true, true, false,
        [this]() { SaveSceneAsDialog(); } });
}

void EditorApp::CaptureCleanScene()
{
    if (!m_ctx.activeScene) {
        m_dirtyTracker.Reset();
        m_ctx.sceneDirty = false;
        return;
    }

    CacheSceneWriteTime();
    m_dirtyTracker.CaptureClean(*m_ctx.activeScene);
    m_ctx.sceneDirty = false;
    m_dirtyPollTimer = 0.0f;
    UpdateWindowTitle();
}

void EditorApp::RefreshSceneDirtyState(bool force)
{
    if (!m_ctx.activeScene) return;
    if (m_ctx.playMode && !m_ctx.playMode->IsInEditor()) return;

    // WHY: SceneDirtyTracker::Evaluate() はシーン全体を serialize して hash 化するため、
    // 未編集のアイドル状態で 0.5 秒ごとに呼ぶと Release ビルドでは FPS の周期的な落ち込みとして見える。
    // WHAT: 編集操作は MarkDirty() で dirty に遷移させる設計なので、clean 状態では重い再評価を省略する。
    if (!force && !m_ctx.sceneDirty && !m_dirtyTracker.IsDirty())
        return;

    m_dirtyPollTimer += ImGui::GetIO().DeltaTime;
    if (!force && m_dirtyPollTimer < 0.5f && m_ctx.sceneDirty == m_dirtyTracker.IsDirty())
        return;
    m_dirtyPollTimer = 0.0f;

    const bool wasDirty = m_ctx.sceneDirty;
    m_ctx.sceneDirty = m_dirtyTracker.Evaluate(*m_ctx.activeScene);
    if (wasDirty != m_ctx.sceneDirty)
        UpdateWindowTitle();
}

void EditorApp::UpdateWindowTitle()
{
    if (!m_hwnd) return;

    const std::string sceneName = m_ctx.currentScenePath.empty()
        ? "Untitled"
        : util::FileSystem::GetFilename(m_ctx.currentScenePath);

    if (m_titleInitialized &&
        m_lastTitleDirty == m_ctx.sceneDirty &&
        m_lastTitleScenePath == m_ctx.currentScenePath)
        return;

    m_titleInitialized = true;
    m_lastTitleDirty = m_ctx.sceneDirty;
    m_lastTitleScenePath = m_ctx.currentScenePath;

    std::string title = "FBZZ Editor - " + sceneName;
    if (m_ctx.sceneDirty) title += "*";
    SetWindowTextW(m_hwnd, Utf8ToWide(title).c_str());
}

void EditorApp::MarkSceneDirty()
{
    m_dirtyTracker.MarkDirty();
    if (!m_ctx.sceneDirty) {
        m_ctx.sceneDirty = true;
        UpdateWindowTitle();
    }
}

void EditorApp::ConfirmDiscardUnsaved(const std::string& actionName, std::function<void()> action)
{
    if (!m_ctx.sceneDirty) {
        if (action) action();
        return;
    }

    ModalDialog::OpenUnsavedChanges(actionName,
        "The current scene has unsaved changes.",
        [this, action]() {
            if (!SaveScene()) return false;
            if (action) action();
            return true;
        },
        std::move(action));
}

void EditorApp::NewScene()
{
    if (!m_ctx.activeScene) return;
    m_ctx.activeScene->Clear();
    m_ctx.selectedEntities.clear();
    m_settings.lastScenePath.clear();
    m_ctx.currentScenePath.clear();
    m_lastSceneWriteTime = {};
    CaptureCleanScene();
    FBZZ_LOG_INFO("New scene created");
}

void EditorApp::RequestNewScene()
{
    ConfirmDiscardUnsaved("New Scene", [this]() { NewScene(); });
}

void EditorApp::RequestOpenSceneFromDialog()
{
    ConfirmDiscardUnsaved("Open Scene", [this]() { OpenSceneFromDialog(); });
}

void EditorApp::RequestOpenScenePath(const std::string& path)
{
    ConfirmDiscardUnsaved("Open Scene", [this, path]() { OpenScenePath(path); });
}

void EditorApp::RequestExit()
{
    ConfirmDiscardUnsaved("Exit", []() { PostQuitMessage(0); });
}

bool EditorApp::OpenSceneFromDialog()
{
    if (!m_ctx.activeScene) return false;

    std::string path;
    if (!FileDialog::OpenFile(m_hwnd, { SCENE_FILTER }, path)) return false;
    return OpenScenePath(path);
}

bool EditorApp::OpenScenePath(const std::string& path)
{
    if (!m_ctx.activeScene || path.empty()) return false;

    if (!SceneSerializer::Load(*m_ctx.activeScene, path)) {
        FBZZ_LOG_ERROR("Open scene failed: %s", path.c_str());
        return false;
    }

    m_settings.lastScenePath = path;
    m_ctx.currentScenePath = path;
    m_ctx.selectedEntities.clear();
    CaptureCleanScene();
    FBZZ_LOG_INFO("Opened scene: %s", path.c_str());
    return true;
}

bool EditorApp::SaveScene()
{
    if (!m_ctx.activeScene) return false;
    if (m_settings.lastScenePath.empty()) return SaveSceneAsDialog();

    if (!SceneSerializer::Save(*m_ctx.activeScene, m_settings.lastScenePath)) {
        FBZZ_LOG_ERROR("Save scene failed: %s", m_settings.lastScenePath.c_str());
        return false;
    }
    m_ctx.projectSettings.Save(m_projectSettingsPath);

    m_ctx.currentScenePath = m_settings.lastScenePath;
    CaptureCleanScene();
    FBZZ_LOG_INFO("Saved scene: %s", m_settings.lastScenePath.c_str());
    return true;
}

bool EditorApp::SaveSceneAsDialog()
{
    if (!m_ctx.activeScene) return false;

    std::string path;
    if (!FileDialog::SaveFile(m_hwnd, { SCENE_FILTER }, path)) return false;
    path = WithFbzzExtension(path);

    if (!SceneSerializer::Save(*m_ctx.activeScene, path)) {
        FBZZ_LOG_ERROR("Save scene failed: %s", path.c_str());
        return false;
    }
    m_ctx.projectSettings.Save(m_projectSettingsPath);

    m_settings.lastScenePath = path;
    m_ctx.currentScenePath = path;
    CaptureCleanScene();
    FBZZ_LOG_INFO("Saved scene: %s", path.c_str());
    return true;
}

void EditorApp::CacheSceneWriteTime()
{
    if (m_settings.lastScenePath.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExA(m_settings.lastScenePath.c_str(), GetFileExInfoStandard, &info))
        m_lastSceneWriteTime = info.ftLastWriteTime;
}

void EditorApp::CheckHotReload()
{
    if (!m_ctx.hotReloadEnabled) return;
    if (m_settings.lastScenePath.empty() || !m_ctx.activeScene) return;
    if (m_ctx.playMode && !m_ctx.playMode->IsInEditor()) return;

    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExA(m_settings.lastScenePath.c_str(), GetFileExInfoStandard, &info))
        return;

    const FILETIME& ft = info.ftLastWriteTime;
    // キャッシュが未設定 (初回) の場合はリロードせずに記録だけする
    if (m_lastSceneWriteTime.dwLowDateTime == 0 && m_lastSceneWriteTime.dwHighDateTime == 0) {
        m_lastSceneWriteTime = ft;
        return;
    }

    if (CompareFileTime(&ft, &m_lastSceneWriteTime) != 0) {
        m_lastSceneWriteTime = ft;
        if (!SceneSerializer::Load(*m_ctx.activeScene, m_settings.lastScenePath))
            FBZZ_LOG_WARN("Hot reload failed: %s", m_settings.lastScenePath.c_str());
        else {
            m_ctx.selectedEntities.clear();
            CaptureCleanScene();
            FBZZ_LOG_INFO("Hot reloaded: %s", m_settings.lastScenePath.c_str());
        }
    }
}

} // namespace fbzz::editor
