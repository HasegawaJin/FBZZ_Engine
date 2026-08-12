// FBZZ Engine
// AiSettingsPanel.cpp | fbzz::editor
// AI 連携の状態表示とセットアップ操作を描画する
#include <Editor/Panels/AiSettingsPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <imgui.h>
#include <array>
#include <utility>

namespace fbzz::editor {

namespace {

constexpr std::array<const char*, 3> PERMISSIONS = { "read", "dry-run", "write" };

// 診断結果を色と短いラベルで統一表示し、未設定箇所を一目で識別できるようにする。
void DrawStatusRow(const char* label, bool ready, const char* readyText, const char* missingText)
{
    ImGui::TextUnformatted(label);
    ImGui::SameLine(190.0f);
    const ImVec4 color = ready
        ? ImVec4(0.30f, 0.85f, 0.45f, 1.0f)
        : ImVec4(0.95f, 0.45f, 0.30f, 1.0f);
    ImGui::TextColored(color, "%s", ready ? readyText : missingText);
}

} // namespace

void AiSettingsPanel::RefreshDiagnostics(const EditorContext& ctx)
{
    m_status = ai::AiSetupService::Inspect(ctx.engineRoot);
    m_inspectedEngineRoot = ctx.engineRoot;
    m_hasDiagnostics = true;

    for (int i = 0; i < static_cast<int>(PERMISSIONS.size()); ++i) {
        if (m_status.desktopPermission == PERMISSIONS[static_cast<std::size_t>(i)]) {
            m_permissionIndex = i;
            break;
        }
    }
}

void AiSettingsPanel::SetMessage(std::string message, bool error)
{
    m_message = std::move(message);
    m_messageIsError = error;
}

void AiSettingsPanel::OnRenderContent(EditorContext& ctx)
{
    if (!m_hasDiagnostics || m_inspectedEngineRoot != ctx.engineRoot)
        RefreshDiagnostics(ctx);

    ImGui::TextUnformatted("Connection Status");
    ImGui::Separator();
    DrawStatusRow("Node.js", m_status.nodeFound, "Found", "Not found");
    DrawStatusRow("Editor MCP", m_status.mcpDistFound, "Ready", "Build required");
    DrawStatusRow("Claude Desktop", m_status.desktopRegistered, "Registered", "Not registered");
    DrawStatusRow("Editor Command Bus", ctx.aiCommandBusRunning, "Running", "Stopped");

    if (m_status.desktopConfigBroken) {
        ImGui::Spacing();
        ImGui::TextColored({ 0.95f, 0.45f, 0.30f, 1.0f },
            "claude_desktop_config.json is invalid. Registration is disabled.");
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Editor Connection");
    ImGui::Separator();
    bool enabled = ctx.aiCommandBusEnabled;
    if (ImGui::Checkbox("Enable Editor Command Bus", &enabled)) {
        if (enabled) {
            const bool started = ctx.startAiCommandBus && ctx.startAiCommandBus();
            ctx.aiCommandBusEnabled = started;
            SetMessage(started
                ? "Editor Command Bus を開始しました。"
                : "Editor Command Bus を開始できませんでした。Console を確認してください。", !started);
        } else {
            if (ctx.stopAiCommandBus) ctx.stopAiCommandBus();
            ctx.aiCommandBusEnabled = false;
            SetMessage("Editor Command Bus を停止しました。", false);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("現在の待受状態と、次回起動時の自動開始設定を同時に変更します。");

    ImGui::Spacing();
    ImGui::TextUnformatted("Client Setup");
    ImGui::Separator();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::Combo("Permission", &m_permissionIndex, PERMISSIONS.data(),
                 static_cast<int>(PERMISSIONS.size()));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("read: 読み取りのみ / dry-run: 変更の試算まで / write: 編集を許可");

    const bool canRegister = m_status.nodeFound && m_status.mcpDistFound && !m_status.desktopConfigBroken;
    if (!canRegister) ImGui::BeginDisabled();
    if (ImGui::Button(m_status.desktopRegistered ? "Update Claude Desktop" : "Register Claude Desktop")) {
        std::string error;
        const bool registered = ai::AiSetupService::RegisterClaudeDesktop(
            m_status.mcpStdioPath, PERMISSIONS[static_cast<std::size_t>(m_permissionIndex)], error);
        SetMessage(registered
            ? "Claude Desktop の設定を更新しました。再起動後に反映されます。"
            : error, !registered);
        if (registered) RefreshDiagnostics(ctx);
    }
    if (!canRegister) ImGui::EndDisabled();

    ImGui::SameLine();
    if (!m_status.mcpDistFound) ImGui::BeginDisabled();
    if (ImGui::Button("Copy Claude Code Command")) {
        const std::string command = ai::AiSetupService::BuildClaudeCodeCommand(
            m_status.mcpStdioPath, PERMISSIONS[static_cast<std::size_t>(m_permissionIndex)]);
        ImGui::SetClipboardText(command.c_str());
        SetMessage("Claude Code 用の登録コマンドをコピーしました。", false);
    }
    if (!m_status.mcpDistFound) ImGui::EndDisabled();

    if (ImGui::Button("Launch Claude Desktop")) {
        std::string error;
        const bool launched = ai::AiSetupService::LaunchClaudeDesktop(error);
        SetMessage(launched ? "Claude Desktop を起動しました。" : error, !launched);
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh Diagnostics")) {
        RefreshDiagnostics(ctx);
        SetMessage("診断結果を更新しました。", false);
    }

    if (!m_status.mcpStdioPath.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("MCP entry point");
        ImGui::TextWrapped("%s", m_status.mcpStdioPath.c_str());
    }

    if (!m_message.empty()) {
        ImGui::Spacing();
        const ImVec4 color = m_messageIsError
            ? ImVec4(0.95f, 0.45f, 0.30f, 1.0f)
            : ImVec4(0.45f, 0.80f, 0.95f, 1.0f);
        ImGui::TextColored(color, "%s", m_message.c_str());
    }
}

} // namespace fbzz::editor
