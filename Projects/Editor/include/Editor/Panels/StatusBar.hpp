// FBZZ Engine
// StatusBar.hpp | fbzz::editor
// MenuBar 直下に固定描画される情報バー（DockSpaceHost 内インライン描画）
#pragma once
#include <string>

namespace fbzz::editor { struct EditorContext; }

namespace fbzz::editor {

class StatusBar {
public:
    void Draw(EditorContext& ctx);
    void SetMessage(const std::string& msg) { m_message = msg; }

private:
    std::string m_message;
    float       m_fps       = 0.0f;
    float       m_fpsTimer  = 0.0f;
    int         m_fpsCount  = 0;
};

} // namespace fbzz::editor
