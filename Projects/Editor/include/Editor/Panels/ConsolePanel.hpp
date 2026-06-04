// FBZZ Engine
// ConsolePanel.hpp | fbzz::editor
// ログエントリをフィルタ・検索・表示するコンソールパネル
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Util/ConsoleSink.hpp>
#include <array>

namespace fbzz::editor {

class ConsolePanel : public IPanel {
public:
    explicit ConsolePanel(ConsoleSink& sink);
    const char* GetWindowName() const override { return "Console"; }
    void OnInit(EditorContext& ctx) override;
    void OnShutdown() override;

private:
    void OnRenderContent(EditorContext& ctx) override;

    ConsoleSink& m_sink;

    bool m_showDebug  = false;
    bool m_showInfo   = true;
    bool m_showWarn   = true;
    bool m_showError  = true;
    bool m_autoScroll = true;
    std::array<char, 256> m_filterBuf = {};
};

} // namespace fbzz::editor
