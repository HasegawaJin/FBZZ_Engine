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
#include <memory>
#include <vector>

namespace fbzz::renderer { class IRenderer; class IRenderTarget; }
namespace fbzz::core     { class Window; }

namespace fbzz::editor {

class ViewportPanel;

class EditorApp {
public:
    bool Init(renderer::IRenderer& renderer, core::Window& window);
    void Shutdown();

    void BeginFrame();
    void RenderPanels(EditorContext& ctx);
    void EndFrame(renderer::IRenderer& renderer);

    EditorContext& GetContext() { return m_ctx; }

    // Viewport に紐づいたオフスクリーン RT (main.cpp はここに描く)
    std::shared_ptr<renderer::IRenderTarget> GetViewportRT() const { return m_sceneViewportRT; }
    std::shared_ptr<renderer::IRenderTarget> GetGameViewportRT() const { return m_gameViewportRT; }

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
    ViewportPanel*                           m_sceneViewportPanel = nullptr;
    ViewportPanel*                           m_gameViewportPanel  = nullptr;
    std::shared_ptr<renderer::IRenderTarget> m_sceneViewportRT;
    std::shared_ptr<renderer::IRenderTarget> m_gameViewportRT;
};

} // namespace fbzz::editor
