/// @file    MemoryDebugPanel.hpp
/// @brief   確保台帳と Play 往復の差分を表示する独立パネル。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#pragma once
#include <Editor/Panels/IPanel.hpp>
namespace fbzz::editor {
class MemoryDebugPanel final : public IPanel {
public:
    const char* GetWindowName() const override { return "Memory Debug"; }
    bool GetDefaultVisibility() const override { return false; }
    void OnLoadSettings(const EditorSettings& settings) override;
    void OnSaveSettings(EditorSettings& settings) const override;
protected:
    void OnRenderContent(EditorContext& ctx) override;
};
}
