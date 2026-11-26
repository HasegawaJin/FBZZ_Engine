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

    ai::AiSetupStatus m_status; ///< 最新の AI 連携環境の診断結果。
    std::string m_inspectedEngineRoot; ///< m_status を取得したときに調べた Editor の SDK またはリポジトリのルート。
    std::string m_message; ///< 直近の AI 連携設定操作の結果を表示する文言。
    int m_permissionIndex = 0; ///< AI 連携設定で選択した権限プリセットの添字。
    bool m_hasDiagnostics = false; ///< 現在表示できる AI 連携の診断結果を取得済みか。
    bool m_messageIsError = false; ///< m_message をエラーとして表示するか。
};

} // namespace fbzz::editor
