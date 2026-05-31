// FBZZ Engine
// EditorApp.hpp | fbzz::editor
// エディター全体のライフサイクルを管理する
#pragma once
#include <Editor/EditorContext.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/HotkeyManager.hpp>
#include <Editor/Util/ConsoleSink.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <Editor/Util/SceneDirtyTracker.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <Windows.h>

namespace fbzz::renderer { class IRenderer; class ResourceManager; }
namespace fbzz::core     { class Window; }

namespace fbzz::editor {

class ViewportPanel;
class ProjectSettingsPanel;
class AssetBrowserPanel;

class EditorApp {
public:
    bool Init(renderer::IRenderer& renderer, renderer::ResourceManager& resources, core::Window& window);
    void Shutdown();
    bool OpenProject(const std::string& projectRoot, const std::string& projectSettingsPath, const std::string& scenePath);

    void BeginFrame();
    void RenderPanels(EditorContext& ctx);
    void EndFrame(renderer::IRenderer& renderer);

    EditorContext& GetContext() { return m_ctx; }

    // Viewport に紐づいたオフスクリーン RT (main.cpp はここに描く)
    renderer::ResourceHandle<renderer::RenderTargetTag> GetViewportRT() const { return m_sceneViewportRT; }
    renderer::ResourceHandle<renderer::RenderTargetTag> GetGameViewportRT() const { return m_gameViewportRT; }
    renderer::ResourceHandle<renderer::RenderTargetTag> GetUIViewportRT() const { return m_uiViewportRT; }

private:
    void BuildMenuBar(EditorContext& ctx);
    void BuildPlayToolbar(EditorContext& ctx);
    void RegisterDefaultHotkeys();
    void ResizeViewportRTsIfNeeded();
    void CheckHotReload();
    void CacheSceneWriteTime();
    void CaptureCleanScene();
    void RefreshSceneDirtyState(bool force);
    void UpdateWindowTitle();
    void MarkSceneDirty();
    void ConfirmDiscardUnsaved(const std::string& actionName, std::function<void()> action);
    void NewScene();
    void RequestNewScene();
    void RequestOpenSceneFromDialog();
    void RequestOpenScenePath(const std::string& path);
    void RequestExit();
    bool OpenSceneFromDialog();
    bool OpenScenePath(const std::string& path);
    bool SaveScene();
    bool SaveSceneAsDialog();

    EditorContext                        m_ctx;
    std::vector<std::unique_ptr<IPanel>> m_panels;

    UndoStack          m_undoStack;
    HotkeyManager      m_hotkeys;
    ConsoleSink        m_consoleSink;
    EditorSettings     m_settings;
    SceneDirtyTracker  m_dirtyTracker;
    std::string        m_projectRoot;
    std::string        m_projectSettingsPath = "editor_config/project_settings.toml";
    float              m_dirtyPollTimer = 0.0f;
    bool               m_titleInitialized = false;
    bool               m_lastTitleDirty = false;
    std::string        m_lastTitleScenePath;
    PlayModeController m_playMode;

    FILETIME                                 m_lastSceneWriteTime = {};
    HWND                                     m_hwnd          = nullptr;
    renderer::IRenderer*                     m_renderer      = nullptr;
    renderer::ResourceManager*               m_resources     = nullptr;
    ViewportPanel*                           m_sceneViewportPanel     = nullptr;
    ViewportPanel*                           m_gameViewportPanel      = nullptr;
    ViewportPanel*                           m_uiViewportPanel        = nullptr;
    ProjectSettingsPanel*                    m_projectSettingsPanel   = nullptr;
    AssetBrowserPanel*                       m_assetBrowserPanel      = nullptr;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_sceneViewportRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_gameViewportRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_uiViewportRT;
};

} // namespace fbzz::editor
