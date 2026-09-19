/// @file    AiSettingsPanel.hpp
/// @brief   AI 連携の診断・登録・Command Bus 制御を集約する設定パネル。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once
#include <Editor/Ai/AiSetupService.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <string>

namespace fbzz::editor {

/// AI クライアント設定と Editor 側の接続状態を一つのドッキング可能 Window として管理する。
class AiSettingsPanel final : public IPanel {
public:
    const char* GetWindowName() const override { return "AI Settings"; }
    bool GetDefaultVisibility() const override { return false; }
    const char* GetMenuCategory() const override { return "Settings"; }

protected:
    /// 接続状態とクライアント登録操作を描画し、EditorContext 経由で Bus の寿命を制御する。
    void OnRenderContent(EditorContext& ctx) override;

private:
    /// Node / MCP 成果物 / Claude 登録を再検査し、表示用スナップショットを更新する。
    void RefreshDiagnostics(const EditorContext& ctx);
    /// 直近の操作結果を成功・失敗の色付きメッセージとして保持する。
    void SetMessage(std::string message, bool error);

    ai::AiSetupStatus m_status;
    std::string m_inspectedEngineRoot;
    std::string m_message;
    int m_permissionIndex = 0;
    bool m_hasDiagnostics = false;
    bool m_messageIsError = false;
};

} // namespace fbzz::editor
