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
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/IRenderTarget.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <ImGuizmo.h>
#include <imgui_impl_win32.h>
#include <Windows.h>

// imgui_impl_win32.h では #if 0 で隠されているため手動で前方宣言する
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fbzz::editor {

static constexpr const char* SETTINGS_DIR  = "editor_config";
static constexpr const char* SETTINGS_PATH = "editor_config/editor_settings.toml";

namespace {

const FileFilter SCENE_FILTER{ "FBZZ Scene", "*.fbzz" };

std::string WithFbzzExtension(const std::string& path)
{
    if (path.empty() || !util::FileSystem::GetExtension(path).empty()) return path;
    return path + ".fbzz";
}

} // namespace

bool EditorApp::Init(renderer::IRenderer& renderer, core::Window& window)
{
    m_hwnd     = window.GetHandle();
    m_renderer = &renderer;

    window.SetWndProcHook([](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> bool {
        return ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp) != 0;
    });

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuizmo::SetImGuiContext(ImGui::GetCurrentContext());
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    renderer.ImGuiInit(m_hwnd);

    m_ctx.undoStack = &m_undoStack;
    m_ctx.playMode  = &m_playMode;

    m_panels.push_back(std::make_unique<SceneHierarchyPanel>());
    m_panels.push_back(std::make_unique<InspectorPanel>());
    {
        auto vp = std::make_unique<ViewportPanel>();
        m_viewportPanel = vp.get();
        m_panels.push_back(std::move(vp));
    }
    m_panels.push_back(std::make_unique<ConsolePanel>(m_consoleSink));
    m_panels.push_back(std::make_unique<AssetBrowserPanel>("Assets"));
    m_panels.push_back(std::make_unique<StatusBar>());

    for (auto& panel : m_panels)
        panel->OnInit(m_ctx);

    RegisterDefaultHotkeys();

    util::FileSystem::EnsureDirectory(SETTINGS_DIR);
    m_settings.Load(SETTINGS_PATH);
    m_ctx.showGrid    = m_settings.showGrid;
    m_ctx.snapEnabled = m_settings.snapEnabled;

    // 初回 RT をウィンドウサイズで生成する
    m_viewportRT = renderer.CreateRenderTarget(window.GetWidth(), window.GetHeight());
    if (m_viewportPanel)
        m_viewportPanel->hdrRT = m_viewportRT;

    FBZZ_LOG_INFO("EditorApp init done");
    return true;
}

void EditorApp::Shutdown()
{
    for (auto& panel : m_panels)
        panel->OnShutdown();

    m_settings.Save(SETTINGS_PATH);
    m_viewportRT.reset();
    m_renderer->ImGuiShutdown();
    ImGui::DestroyContext();
}

void EditorApp::BeginFrame()
{
    // Viewport パネルサイズが前フレームで変わった場合は RT を再生成する
    // シーン描画の前に呼ぶことで「空の RT をパネルに表示」を防ぐ
    ResizeViewportRTIfNeeded();

    m_renderer->ImGuiNewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    m_hotkeys.ProcessInput();

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
    ImGui::DockSpace(dockId, { 0, 0 }, ImGuiDockNodeFlags_PassthruCentralNode);

    BuildMenuBar(m_ctx);

    // ModalDialog は OpenPopup を Begin/End 内から発行する必要がある
    ModalDialog::OnRender();

    ImGui::End();
}

void EditorApp::RenderPanels(EditorContext& ctx)
{
    for (auto& panel : m_panels)
        if (panel->visible) panel->OnRender(ctx);
}

void EditorApp::EndFrame(renderer::IRenderer& renderer)
{
    ImGui::Render();
    renderer.ImGuiRenderDrawData();
}

void EditorApp::ResizeViewportRTIfNeeded()
{
    if (!m_viewportRT || !m_viewportPanel || !m_renderer) return;

    uint32_t vpW = static_cast<uint32_t>(m_ctx.viewportWidth);
    uint32_t vpH = static_cast<uint32_t>(m_ctx.viewportHeight);
    if (vpW == 0 || vpH == 0) return;
    if (vpW == m_viewportRT->GetWidth() && vpH == m_viewportRT->GetHeight()) return;

    m_viewportRT = m_renderer->CreateRenderTarget(vpW, vpH);
    m_viewportPanel->hdrRT = m_viewportRT;
}

void EditorApp::BuildMenuBar(EditorContext& ctx)
{
    if (!ImGui::BeginMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Scene")) {
            ModalDialog::OpenConfirm("New Scene",
                "Changes will be lost. Continue?",
                [&ctx]() { (void)ctx; });
        }
        if (ImGui::MenuItem("Open...", "Ctrl+O", false, ctx.activeScene != nullptr))
            OpenSceneFromDialog();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, ctx.activeScene != nullptr))
            SaveScene();
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, ctx.activeScene != nullptr))
            SaveSceneAsDialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) PostQuitMessage(0);
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
        ImGui::MenuItem("Grid",        nullptr, &ctx.showGrid);
        ImGui::MenuItem("Light Range", nullptr, &ctx.showLightRange);
        ImGui::MenuItem("Colliders",   nullptr, &ctx.showColliders);
        ImGui::MenuItem("Stats",       nullptr, &ctx.showSceneStats);
        ImGui::Separator();
        ImGui::MenuItem("Wireframe",   nullptr, &ctx.renderSettings.wireframeMode);
        ImGui::Separator();
        if (ImGui::BeginMenu("Post Process")) {
            ImGui::MenuItem("Shadow",   nullptr, &ctx.renderSettings.shadowEnabled);
            ImGui::MenuItem("Bloom",    nullptr, &ctx.renderSettings.bloomEnabled);
            ImGui::MenuItem("Fog",      nullptr, &ctx.renderSettings.fogEnabled);
            ImGui::MenuItem("FXAA",     nullptr, &ctx.renderSettings.fxaaEnabled);
            ImGui::Separator();
            ImGui::SliderFloat("Exposure",    &ctx.renderSettings.exposure,   0.1f, 4.0f);
            ImGui::SliderFloat("Fog Density", &ctx.renderSettings.fogDensity, 0.0f, 1.0f);
            ImGui::SliderFloat("Fog Far",     &ctx.renderSettings.fogFar,     1.0f, 100.0f);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    PlayModeController* pm = ctx.playMode;
    if (pm && pm->IsInEditor()) {
        if (ImGui::MenuItem("  Play  ") && ctx.activeScene)
            pm->Play(*ctx.activeScene);
    } else if (pm) {
        if (ImGui::MenuItem("  Stop  ") && ctx.activeScene)
            pm->Stop(*ctx.activeScene);
        if (ImGui::MenuItem(pm->IsPaused() ? " Resume " : " Pause  "))
            pm->Pause();
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
        [this]() { OpenSceneFromDialog(); } });
    m_hotkeys.Register({ "Save Scene", ImGuiKey_S, true, false, false,
        [this]() { SaveScene(); } });
    m_hotkeys.Register({ "Save Scene As", ImGuiKey_S, true, true, false,
        [this]() { SaveSceneAsDialog(); } });
}

bool EditorApp::OpenSceneFromDialog()
{
    if (!m_ctx.activeScene) return false;

    std::string path;
    if (!FileDialog::OpenFile(m_hwnd, { SCENE_FILTER }, path)) return false;
    if (!SceneSerializer::Load(*m_ctx.activeScene, path)) {
        FBZZ_LOG_ERROR("Open scene failed: %s", path.c_str());
        return false;
    }

    m_settings.lastScenePath = path;
    m_ctx.selectedEntities.clear();
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

    m_settings.lastScenePath = path;
    FBZZ_LOG_INFO("Saved scene: %s", path.c_str());
    return true;
}

} // namespace fbzz::editor
