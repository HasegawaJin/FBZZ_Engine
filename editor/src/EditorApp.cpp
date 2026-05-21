// FBZZ Engine
// EditorApp.cpp | fbzz::editor
// エディター全体のライフサイクル管理 + DockSpace + MenuBar
#include <editor/EditorApp.hpp>
#include <editor/EditorContext.hpp>
#include <editor/Util/ModalDialog.hpp>
#include <editor/Util/FileDialog.hpp>
#include <editor/Util/SceneSerializer.hpp>
#include <editor/Panels/SceneHierarchyPanel.hpp>
#include <editor/Panels/InspectorPanel.hpp>
#include <editor/Panels/ViewportPanel.hpp>
#include <editor/Panels/LightPanel.hpp>
#include <editor/Panels/ConsolePanel.hpp>
#include <editor/Panels/AssetBrowserPanel.hpp>
#include <editor/Panels/StatusBar.hpp>
#include <engine/Renderer/IRenderer.hpp>
#include <engine/Renderer/IRenderTarget.hpp>
#include <engine/Core/Logger.hpp>
#include <engine/Core/Window.hpp>
#include <engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <ImGuizmo.h>
#include <imgui_impl_win32.h>
#include <Windows.h>

// imgui_impl_win32.h では #if 0 で隠されているため手動で前方宣言する
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fbzz::editor {

static constexpr const char* SETTINGS_DIR  = "editor_config";
static constexpr const char* SETTINGS_PATH = "editor_config/editor_settings.toml";

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

    core::Logger::AddSink(&m_consoleSink);

    m_ctx.undoStack = &m_undoStack;
    m_ctx.playMode  = &m_playMode;

    m_panels.push_back(std::make_unique<SceneHierarchyPanel>());
    m_panels.push_back(std::make_unique<InspectorPanel>());
    {
        auto vp = std::make_unique<ViewportPanel>();
        m_viewportPanel = vp.get();
        m_panels.push_back(std::move(vp));
    }
    m_panels.push_back(std::make_unique<LightPanel>());
    m_panels.push_back(std::make_unique<ConsolePanel>(m_consoleSink));
    m_panels.push_back(std::make_unique<AssetBrowserPanel>("Assets"));
    m_panels.push_back(std::make_unique<StatusBar>());

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
    m_settings.Save(SETTINGS_PATH);
    m_viewportRT.reset();
    core::Logger::RemoveSink(&m_consoleSink);
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
        if (ImGui::MenuItem("Open...", "Ctrl+O")) {
            std::string path;
            if (ctx.activeScene &&
                FileDialog::OpenFile(m_hwnd, { {"FBZZ Scene", "*.fbzz"} }, path))
                SceneSerializer::Load(*ctx.activeScene, path);
        }
        if (ImGui::MenuItem("Save", "Ctrl+S")) {
            if (ctx.activeScene && !m_settings.lastScenePath.empty())
                SceneSerializer::Save(*ctx.activeScene, m_settings.lastScenePath);
        }
        if (ImGui::MenuItem("Save As...")) {
            std::string path;
            if (ctx.activeScene &&
                FileDialog::SaveFile(m_hwnd, { {"FBZZ Scene", "*.fbzz"} }, path)) {
                m_settings.lastScenePath = path;
                SceneSerializer::Save(*ctx.activeScene, path);
            }
        }
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
}

} // namespace fbzz::editor
