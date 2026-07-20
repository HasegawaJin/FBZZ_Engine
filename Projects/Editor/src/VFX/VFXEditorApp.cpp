// FBZZ Engine
// VFXEditorApp.cpp | fbzz::editor
// 独立VFX制作Applicationと専用Preview Worldの実装
#include <Editor/VFX/VFXEditorApp.hpp>

#include <Editor/Ai/EditorBusDispatcher.hpp>
#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Editor/Ai/Json.hpp>
#include <Editor/Ai/NamedPipeServer.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/FileDialog.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <utility>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fbzz::editor {

VFXEditorApp::~VFXEditorApp() = default;

bool VFXEditorApp::Init(renderer::IRenderer& renderer,
                        renderer::IImGuiRenderer& imguiRenderer,
                        renderer::ResourceManager& resources,
                        core::Window& window,
                        const std::string& projectRoot,
                        const std::string& projectSettingsPath,
                        const std::string& initialAssetPath)
{
    m_renderer = &renderer;
    m_imguiRenderer = &imguiRenderer;
    m_resources = &resources;
    m_window = &window;

    m_context.renderer = &renderer;
    m_context.imguiRenderer = &imguiRenderer;
    m_context.resources = &resources;
    m_context.memorySystem = &core::Application::Get().GetMemorySystem();
    m_context.projectRoot = projectRoot;
    m_context.selectedAssetPath = initialAssetPath;
    if (!m_context.projectSettings.Load(projectSettingsPath)) {
        FBZZ_LOG_ERROR("VFXEditorApp: ProjectSettingsの読み込みに失敗しました: %s",
                       projectSettingsPath.c_str());
        return false;
    }

    m_previewScene = std::make_unique<scene::Scene>();
    m_previewSceneManager.SetScene(m_previewScene.get());
    m_previewSceneManager.SetSimulating(false);
    m_context.activeScene = m_previewScene.get();
    m_context.vfxPreviewScene = m_previewScene.get();
    // AI captureはUI Previewと別Worldを使い、開いているGraphや操作中Emitterの混入を防ぐ。
    m_aiPreviewScene = std::make_unique<scene::Scene>();
    m_aiPreviewSceneManager.SetScene(m_aiPreviewScene.get());
    m_aiPreviewSceneManager.SetSimulating(false);
    m_aiContext = m_context;
    m_aiContext.activeScene = m_aiPreviewScene.get();
    m_aiContext.vfxPreviewScene = m_aiPreviewScene.get();

    window.SetWndProcHook([this](HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_CLOSE && m_panel.HasUnsavedChanges()) {
            const int choice = MessageBoxW(hwnd,
                L"VFX Graphに未保存の変更があります。保存して終了しますか？",
                L"FBZZ VFX Editor", MB_YESNOCANCEL | MB_ICONWARNING);
            if (choice == IDCANCEL) return true;
            if (choice == IDYES && !m_panel.SaveCurrentGraph()) {
                MessageBoxW(hwnd, L"VFX Graphを保存できませんでした。", L"FBZZ VFX Editor",
                            MB_OK | MB_ICONERROR);
                return true;
            }
        }
        return ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam) != 0;
    });
    window.SetFileDropCallback([this](const std::vector<std::string>& paths, int, int) {
        for (const std::string& path : paths) {
            if (util::FileSystem::PathFromUtf8(path).extension() == L".vfx") {
                if (ConfirmDocumentSwitch()) m_panel.OpenDroppedAsset(m_context, path);
                break;
            }
            m_panel.OpenDroppedAsset(m_context, path);
        }
    });

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    EditorTheme::Apply();

    const std::filesystem::path configDirectory =
        util::FileSystem::PathFromUtf8(projectRoot) / L"Assets" / L"EditorConfig";
    (void)util::FileSystem::EnsureDirectory(configDirectory);
    m_imguiIniPath = util::FileSystem::PathToUtf8(configDirectory / L"vfx_editor.ini");
    io.IniFilename = m_imguiIniPath.c_str();
    imguiRenderer.ImGuiInit(window.GetHandle());

    m_context.requestOpenVFXAssetDialog = [this]() { OpenGraphDialog(); };
    m_panel.SetStandaloneApplicationMode(true);
    m_panel.visible = true;
    m_panel.OnInit(m_context);
    if (!initialAssetPath.empty())
        m_panel.OpenDroppedAsset(m_context, initialAssetPath);
    m_previewRT = resources.CreateRenderTarget(960, 540);
    m_aiPreviewRT = resources.CreateRenderTarget(960, 540);
    m_panel.previewRT = m_previewRT;
    m_previewCamera.camera.m_position = { 0.0f, 1.0f, -5.0f };
    m_previewCamera.LookAt({ 0.0f, 0.5f, 0.0f });

    m_aiDispatcher = std::make_unique<ai::EditorBusDispatcher>(m_aiContext);
    m_aiPipeServer = std::make_unique<ai::NamedPipeServer>();
    if (!m_aiPipeServer->Start(L"\\\\.\\pipe\\FBZZVFXEditorCommandBus")) {
        FBZZ_LOG_ERROR("VFXEditorApp: 専用AI Command Busを開始できませんでした");
        m_aiPipeServer.reset();
        m_aiDispatcher.reset();
    }

    window.SetTitle(util::StringUtils::ToWide(
        std::string("FBZZ VFX Editor - ")
        + (initialAssetPath.empty() ? "Untitled"
                                    : util::StringUtils::PathToUtf8(
                                          util::FileSystem::PathFromUtf8(initialAssetPath).filename()))));
    return true;
}

bool VFXEditorApp::OnInit()
{
    return m_renderer != nullptr && m_imguiRenderer != nullptr && m_resources != nullptr;
}

void VFXEditorApp::OnUpdate(float dt)
{
    ResizePreviewIfNeeded();
    m_imguiRenderer->ImGuiNewFrame();
    ImGui::NewFrame();
    if (m_aiPipeServer && m_aiPipeServer->IsRunning() && m_aiDispatcher) {
        m_aiDispatcher->SetVFXPreviewRT(m_aiPreviewRT);
        m_aiPipeServer->DrainRequests([this](const std::string& request) {
            return HandleIpcRequest(request);
        });
    }
    m_panel.OnRender(m_context);

    m_previewSceneManager.SetPhysicsHz(m_context.projectSettings.physics.hz);
    m_previewSceneManager.Update(dt, m_previewPhysicsWorld);
    const bool aiPreviewActive = m_aiContext.vfxAiPreviewUntilFrame != 0
        && Time::frameCount <= m_aiContext.vfxAiPreviewUntilFrame;
    if (aiPreviewActive) {
        m_aiPreviewSceneManager.SetPhysicsHz(m_aiContext.projectSettings.physics.hz);
        m_aiPreviewSceneManager.Update(dt, m_aiPreviewPhysicsWorld);
    }
    if (m_panel.IsPreviewHovered())
        m_previewCamera.Update(dt, true);
}

void VFXEditorApp::OnLateUpdate(float dt)
{
    m_previewSceneManager.LateUpdate(dt, m_previewPhysicsWorld);
    const bool aiPreviewActive = m_aiContext.vfxAiPreviewUntilFrame != 0
        && Time::frameCount <= m_aiContext.vfxAiPreviewUntilFrame;
    if (aiPreviewActive)
        m_aiPreviewSceneManager.LateUpdate(dt, m_aiPreviewPhysicsWorld);
}

void VFXEditorApp::OnRender()
{
    m_renderer->BeginFrame();
    RenderPreview();
    RenderAiPreview();
    m_renderer->SetRenderTarget({}, *m_resources);
    m_renderer->Clear({ 0.018f, 0.021f, 0.028f, 1.0f });
    ImGui::Render();
    m_imguiRenderer->ImGuiRenderDrawData();
    m_renderer->EndFrame();
}

void VFXEditorApp::OnShutdown()
{
    if (m_aiPipeServer) {
        m_aiPipeServer->Stop();
        m_aiPipeServer.reset();
    }
    m_aiDispatcher.reset();
    m_panel.OnShutdown();
    m_context.requestOpenVFXAssetDialog = {};
    m_context.activeScene = nullptr;
    m_context.vfxPreviewScene = nullptr;
    m_aiContext.activeScene = nullptr;
    m_aiContext.vfxPreviewScene = nullptr;
    m_previewSceneManager.ClearScenes();
    m_aiPreviewSceneManager.ClearScenes();
    m_previewScene.reset();
    m_aiPreviewScene.reset();
    if (m_previewRT.IsValid()) {
        m_resources->Release(m_previewRT);
        m_previewRT = {};
        m_panel.previewRT = {};
    }
    if (m_aiPreviewRT.IsValid()) {
        m_resources->Release(m_aiPreviewRT);
        m_aiPreviewRT = {};
    }
    if (ImGui::GetCurrentContext() != nullptr) {
        m_imguiRenderer->ImGuiShutdown();
        ImGui::DestroyContext();
    }
}

std::string VFXEditorApp::HandleIpcRequest(const std::string& request)
{
    std::string error;
    const auto root = ai::ParseJson(request, &error);
    if (!root.has_value()) return {};
    const auto envelope = ai::ParseBusRequest(*root, &error);
    if (!envelope.has_value()) {
        const ai::JsonValue* id = root->Find("id");
        return id != nullptr && id->IsString()
            ? ai::SerializeJson(ai::MakeErrorResponse(id->AsString(), "BAD_REQUEST", error))
            : std::string{};
    }

    const std::string type = envelope->PayloadType();
    if (type == "vfx.editor.ping") {
        ai::JsonValue result = ai::JsonValue::MakeObject();
        result.Set("application", ai::JsonValue("FBZZVFXEditor"));
        result.Set("ready", ai::JsonValue(true));
        return ai::SerializeJson(ai::MakeOkResponse(envelope->id, std::move(result)));
    }
    if (type == "vfx.editor.dropAsset") {
        const ai::JsonValue* pathValue = envelope->payload.Find("path");
        if (pathValue == nullptr || !pathValue->IsString() || pathValue->AsString().empty())
            return ai::SerializeJson(ai::MakeErrorResponse(
                envelope->id, "BAD_ARG", "dropAssetにはpathが必要です"));
        const std::string path = pathValue->AsString();
        if (util::FileSystem::PathFromUtf8(path).extension() == L".vfx"
            && !ConfirmDocumentSwitch())
            return ai::SerializeJson(ai::MakeErrorResponse(
                envelope->id, "CANCELED", "未保存Graphの切り替えをキャンセルしました"));
        m_panel.OpenDroppedAsset(m_context, path);
        if (util::FileSystem::PathFromUtf8(path).extension() == L".vfx") {
            m_window->SetTitle(util::StringUtils::ToWide(
                "FBZZ VFX Editor - " + util::FileSystem::PathToUtf8(
                    util::FileSystem::PathFromUtf8(path).filename())));
        }
        ai::JsonValue result = ai::JsonValue::MakeObject();
        result.Set("accepted", ai::JsonValue(true));
        result.Set("path", ai::JsonValue(path));
        return ai::SerializeJson(ai::MakeOkResponse(envelope->id, std::move(result)));
    }
    if (type == "vfx.editor.flush") {
        if (m_panel.HasUnsavedChanges() && !m_panel.SaveCurrentGraph())
            return ai::SerializeJson(ai::MakeErrorResponse(
                envelope->id, "SAVE_FAILED", "AI編集前に現在のVFX Graphを保存できませんでした"));
        ai::JsonValue result = ai::JsonValue::MakeObject();
        result.Set("flushed", ai::JsonValue(true));
        return ai::SerializeJson(ai::MakeOkResponse(envelope->id, std::move(result)));
    }
    if (type == "vfx.editor.assetChanged") {
        const ai::JsonValue* pathValue = envelope->payload.Find("path");
        const std::string path = pathValue != nullptr && pathValue->IsString()
            ? pathValue->AsString() : std::string{};
        if (!m_panel.ReloadCurrentGraphFromDisk(path))
            return ai::SerializeJson(ai::MakeErrorResponse(
                envelope->id, "RELOAD_FAILED", "AI編集後のVFX Graphを再読み込みできませんでした"));
        ai::JsonValue result = ai::JsonValue::MakeObject();
        result.Set("reloaded", ai::JsonValue(true));
        return ai::SerializeJson(ai::MakeOkResponse(envelope->id, std::move(result)));
    }

    const ai::JsonValue* view = envelope->payload.Find("view");
    const bool vfxCapture = type == "viewport.capture" && view != nullptr
        && view->IsString() && view->AsString() == "vfx";
    if (!type.starts_with("vfx.") && !vfxCapture)
        return ai::SerializeJson(ai::MakeErrorResponse(
            envelope->id, "VFX_ENDPOINT_ONLY", "このPipeはVFX要求だけを受け付けます"));

    if (type == "vfx.preview") {
        const ai::JsonValue* widthValue = envelope->payload.Find("w");
        const ai::JsonValue* heightValue = envelope->payload.Find("h");
        const uint32_t width = static_cast<uint32_t>(std::clamp(
            widthValue != nullptr && widthValue->IsNumber() ? widthValue->AsInt() : 960, 160, 1920));
        const uint32_t height = static_cast<uint32_t>(std::clamp(
            heightValue != nullptr && heightValue->IsNumber() ? heightValue->AsInt() : 540, 90, 1080));
        auto* current = m_resources->Get(m_aiPreviewRT);
        if (current == nullptr || current->GetWidth() != width || current->GetHeight() != height) {
            const auto previous = m_aiPreviewRT;
            m_aiPreviewRT = m_resources->CreateRenderTarget(width, height);
            if (previous.IsValid()) m_resources->Release(previous);
        }
    }
    m_aiDispatcher->SetVFXPreviewRT(m_aiPreviewRT);
    return m_aiDispatcher->Handle(request);
}

void VFXEditorApp::OpenGraphDialog()
{
    std::string path;
    if (!FileDialog::OpenFile(m_window->GetHandle(), { { "FBZZ VFX Graph", "*.vfx" } }, path))
        return;
    if (!ConfirmDocumentSwitch()) return;
    m_context.selectedAssetPath = std::move(path);
    m_window->SetTitle(util::StringUtils::ToWide(
        "FBZZ VFX Editor - " + util::StringUtils::PathToUtf8(
            util::FileSystem::PathFromUtf8(m_context.selectedAssetPath).filename())));
}

bool VFXEditorApp::ConfirmDocumentSwitch()
{
    if (!m_panel.HasUnsavedChanges()) return true;
    const int choice = MessageBoxW(m_window->GetHandle(),
        L"現在のVFX Graphに未保存の変更があります。保存しますか？",
        L"FBZZ VFX Editor", MB_YESNOCANCEL | MB_ICONWARNING);
    if (choice == IDCANCEL) return false;
    if (choice == IDYES && !m_panel.SaveCurrentGraph()) {
        MessageBoxW(m_window->GetHandle(), L"VFX Graphを保存できませんでした。",
                    L"FBZZ VFX Editor", MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

void VFXEditorApp::ResizePreviewIfNeeded()
{
    const uint32_t width = static_cast<uint32_t>((std::max)(m_panel.previewWidth, 1.0f));
    const uint32_t height = static_cast<uint32_t>((std::max)(m_panel.previewHeight, 1.0f));
    auto* current = m_resources->Get(m_previewRT);
    if (current != nullptr && current->GetWidth() == width && current->GetHeight() == height)
        return;
    const auto previous = m_previewRT;
    m_previewRT = m_resources->CreateRenderTarget(width, height);
    if (previous.IsValid()) m_resources->Release(previous);
    m_panel.previewRT = m_previewRT;
}

void VFXEditorApp::RenderPreview()
{
    if (!m_panel.WantsPreviewRender() || !m_previewRT.IsValid() || !m_previewScene)
        return;
    renderer::Camera camera = m_previewCamera.camera;
    if (auto* target = m_resources->Get(m_previewRT); target != nullptr && target->GetHeight() > 0)
        camera.m_aspect = static_cast<float>(target->GetWidth()) / static_cast<float>(target->GetHeight());

    m_renderer->SetRenderTarget(m_previewRT, *m_resources);
    m_renderer->Clear({ 0.018f, 0.021f, 0.028f, 1.0f });
    auto settings = m_context.projectSettings.render;
    settings.selectedObjects.clear();
    settings.showGrid = false;
    settings.showSkeleton = false;
    settings.showLightRange = false;
    settings.showConstraints = false;
    settings.showColliders = false;
    settings.showNavMesh = false;
    settings.showSelectionOutline = false;
    scene::RenderSystemUIOptions uiOptions{};
    uiOptions.enabled = false;
    scene::RenderSystem(*m_previewScene, *m_renderer, *m_resources, camera, m_previewRT,
                        &settings, fbzz::Layer::Everything, &uiOptions, &m_previewPhysicsWorld);
}

void VFXEditorApp::RenderAiPreview()
{
    const bool active = m_aiContext.vfxAiPreviewUntilFrame != 0
        && Time::frameCount <= m_aiContext.vfxAiPreviewUntilFrame;
    if (!active || !m_aiPreviewRT.IsValid() || !m_aiPreviewScene) return;

    renderer::Camera camera = m_previewCamera.camera;
    if (auto* target = m_resources->Get(m_aiPreviewRT); target != nullptr && target->GetHeight() > 0)
        camera.m_aspect = static_cast<float>(target->GetWidth()) / static_cast<float>(target->GetHeight());
    m_renderer->SetRenderTarget(m_aiPreviewRT, *m_resources);
    m_renderer->Clear({ 0.018f, 0.021f, 0.028f, 1.0f });
    auto settings = m_aiContext.projectSettings.render;
    settings.selectedObjects.clear();
    settings.showGrid = false;
    settings.showSkeleton = false;
    settings.showLightRange = false;
    settings.showConstraints = false;
    settings.showColliders = false;
    settings.showNavMesh = false;
    settings.showSelectionOutline = false;
    scene::RenderSystemUIOptions uiOptions{};
    uiOptions.enabled = false;
    scene::RenderSystem(*m_aiPreviewScene, *m_renderer, *m_resources, camera, m_aiPreviewRT,
                        &settings, fbzz::Layer::Everything, &uiOptions, &m_aiPreviewPhysicsWorld);
}

} // namespace fbzz::editor
