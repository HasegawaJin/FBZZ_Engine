/// @file    ScriptRequirementsPanel.hpp
/// @brief   Script の必須設定を確認する一覧パネル。
/// @author  Hasegawa Jin
/// @date    2026-09-29
#pragma once
#include <Editor/Panels/IPanel.hpp>

namespace fbzz::editor {
class ScriptRequirementsPanel final : public IPanel {
public:
    const char* GetWindowName() const override { return "Script Requirements"; }
    const char* GetViewMenuName() const override { return "必須設定チェック"; }
    bool GetDefaultVisibility() const override { return false; }
protected:
    void OnBeforeBegin(EditorContext& ctx) override;
    void OnRenderContent(EditorContext& ctx) override;
};
} /// @note namespace fbzz::editor
