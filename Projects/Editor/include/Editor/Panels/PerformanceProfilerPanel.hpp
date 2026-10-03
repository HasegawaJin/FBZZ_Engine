/// @file    PerformanceProfilerPanel.hpp
/// @brief   CPU と Rendering を表示する Performance Profiler。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#pragma once
#include <Editor/Panels/IPanel.hpp>
namespace fbzz::editor {
class PerformanceProfilerPanel final : public IPanel {
public:
    const char* GetWindowName() const override { return "Analysis"; }
    const char* GetWindowTitle() const override { return "Performance Profiler"; }
    const char* GetViewMenuName() const override { return "Performance Profiler"; }
    bool GetDefaultVisibility() const override { return false; }
    void OnLoadSettings(const EditorSettings& settings) override;
    void OnSaveSettings(EditorSettings& settings) const override;
protected:
    void OnRenderContent(EditorContext& ctx) override;
};
}
