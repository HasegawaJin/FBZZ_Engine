// FBZZ Engine
// EditorApp.cpp | fbzz::editor
// エディター全体のライフサイクルを管理する
#include <editor/EditorApp.hpp>

namespace fbzz::editor {

bool EditorApp::Init(renderer::IRenderer& /*renderer*/, void* /*hwnd*/)
{
    return true;
}

void EditorApp::Shutdown()
{
}

void EditorApp::BeginFrame()
{
}

void EditorApp::RenderPanels(EditorContext& ctx)
{
    for (auto& panel : m_panels)
    {
        if (panel->visible)
            panel->OnRender(ctx);
    }
}

void EditorApp::EndFrame(renderer::IRenderer& /*renderer*/)
{
}

} // namespace fbzz::editor
