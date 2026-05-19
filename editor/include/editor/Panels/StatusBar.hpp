// FBZZ Engine
// StatusBar.hpp | fbzz::editor
// ウィンドウ最下部に固定表示される情報バー
#pragma once
#include <editor/Panels/IPanel.hpp>
#include <string>

namespace fbzz::editor {

class StatusBar : public IPanel {
public:
    void OnRender(EditorContext& ctx) override;

    void SetMessage(const std::string& msg) { m_message = msg; }

private:
    std::string m_message;
    float       m_fps       = 0.0f;
    float       m_fpsTimer  = 0.0f;
    int         m_fpsCount  = 0;
};

} // namespace fbzz::editor
