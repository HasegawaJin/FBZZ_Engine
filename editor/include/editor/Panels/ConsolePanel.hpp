// FBZZ Engine
// ConsolePanel.hpp | fbzz::editor
// ログエントリをフィルタ・検索・表示するコンソールパネル
#pragma once
#include <editor/Panels/IPanel.hpp>
#include <editor/Util/ConsoleSink.hpp>
#include <array>

namespace fbzz::editor {

class ConsolePanel : public IPanel {
public:
    explicit ConsolePanel(ConsoleSink& sink);
    void OnRender(EditorContext& ctx) override;

private:
    ConsoleSink& m_sink;

    bool m_showInfo   = true;
    bool m_showWarn   = true;
    bool m_showError  = true;
    bool m_autoScroll = true;
    std::array<char, 256> m_filterBuf = {};
};

} // namespace fbzz::editor
