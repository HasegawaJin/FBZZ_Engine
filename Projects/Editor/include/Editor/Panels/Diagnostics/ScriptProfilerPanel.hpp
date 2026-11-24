/// @file    ScriptProfilerPanel.hpp
/// @brief   スクリプトの型、callback、実体と呼び出しツリーの計測表示。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#pragma once
#include <Editor/Panels/IPanel.hpp>
namespace fbzz::editor {
class ScriptProfilerPanel final : public IPanel {
public:
    const char* GetWindowName() const override { return "Script Profiler"; }
    bool GetDefaultVisibility() const override { return false; }
    void OnLoadSettings(const EditorSettings& settings) override;
    void OnSaveSettings(EditorSettings& settings) const override;
protected:
    void OnRenderContent(EditorContext& ctx) override;
private:
    int m_group = 0;
    int m_mode = 0;
    int m_range = 1;
    bool m_callTree = false;
    char m_name[128] = {};
    char m_type[128] = {};
    char m_instance[128] = {};
    char m_callback[128] = {};
    char m_scene[32] = {};
};
}
