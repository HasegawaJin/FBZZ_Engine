// FBZZ Engine
// StatusBar.hpp | fbzz::editor
// ウィンドウ最下部に固定表示される情報バー
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <string>

namespace fbzz::editor {

class StatusBar : public IPanel {
public:
    const char* GetWindowName() const override { return "##statusbar"; }
    const char* GetViewMenuName() const override { return "Status Bar"; }

    void SetMessage(const std::string& msg) { m_message = msg; }

private:
    void OnRenderContent(EditorContext& ctx) override;
    bool CanClose() const override { return false; }
    ImGuiWindowFlags GetWindowFlags() const override;
    void OnBeforeBegin(EditorContext& ctx) override;
    void OnAfterBegin(EditorContext& ctx) override;

    std::string m_message;
    float       m_fps       = 0.0f;
    float       m_fpsTimer  = 0.0f;
    int         m_fpsCount  = 0;
};

} // namespace fbzz::editor
