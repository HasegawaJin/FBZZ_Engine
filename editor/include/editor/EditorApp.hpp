// FBZZ Engine
// EditorApp.hpp | fbzz::editor
// エディター全体のライフサイクルを管理する
#pragma once
#include <editor/EditorContext.hpp>
#include <editor/Panels/IPanel.hpp>
#include <memory>
#include <vector>

namespace fbzz::renderer { class IRenderer; }

namespace fbzz::editor {

class EditorApp {
public:
    bool Init(renderer::IRenderer& renderer, void* hwnd);
    void Shutdown();

    void BeginFrame();
    void RenderPanels(EditorContext& ctx);
    void EndFrame(renderer::IRenderer& renderer);

    EditorContext& GetContext() { return m_ctx; }

private:
    EditorContext                          m_ctx;
    std::vector<std::unique_ptr<IPanel>>   m_panels;
};

} // namespace fbzz::editor
