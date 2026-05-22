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
#include <Editor/PlayModeController.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <memory>
#include <vector>

namespace fbzz::renderer { class IRenderer; class ResourceManager; }
namespace fbzz::core     { class Window; }

namespace fbzz::editor {

class ViewportPanel;

class EditorApp {
public:
    bool Init(renderer::IRenderer& renderer, renderer::ResourceManager& resources, core::Window& window);
    void Shutdown();

    void BeginFrame();
    void RenderPanels(EditorContext& ctx);
    void EndFrame(renderer::IRenderer& renderer);

    EditorContext& GetContext() { return m_ctx; }

    // Viewport に紐づいたオフスクリーン RT (main.cpp はここに描く)
    renderer::ResourceHandle<renderer::RenderTargetTag> GetViewportRT() const { return m_sceneViewportRT; }
    renderer::ResourceHandle<renderer::RenderTargetTag> GetGameViewportRT() const { return m_gameViewportRT; }

private:
    void BuildMenuBar(EditorContext& ctx);
    void RegisterDefaultHotkeys();
    void ResizeViewportRTsIfNeeded();
    bool OpenSceneFromDialog();
    bool SaveScene();
    bool SaveSceneAsDialog();

    EditorContext                        m_ctx;
    std::vector<std::unique_ptr<IPanel>> m_panels;

    UndoStack          m_undoStack;
    HotkeyManager      m_hotkeys;
    ConsoleSink        m_consoleSink;
    EditorSettings     m_settings;
    PlayModeController m_playMode;

    void*                                    m_hwnd          = nullptr;
    renderer::IRenderer*                     m_renderer      = nullptr;
    renderer::ResourceManager*               m_resources     = nullptr;
    ViewportPanel*                           m_sceneViewportPanel = nullptr;
    ViewportPanel*                           m_gameViewportPanel  = nullptr;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_sceneViewportRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_gameViewportRT;
};

} // namespace fbzz::editor
