/// @file    IntegrationOperators.cpp
/// @brief   AI接続の診断・登録コマンド取得・クライアント設定を直接公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Ai/AiSetupService.hpp>
namespace fbzz::editor {
void RegisterIntegrationOperators(OperatorRegistry& registry)
{
    for (const char* action : {"inspect", "registration_command", "register_desktop", "launch_desktop"}) {
        EditorOperator op; const std::string name = action;
        op.id = "ai.setup." + name; op.label = op.id; op.category = "AI Setup";
        op.kind = name == "inspect" || name == "registration_command" ? OpKind::Query : OpKind::Action;
        op.desc = "AI Settingsと同じセットアップ処理。inspectは診断、registration_commandは登録文字列、register_desktopは既存の他エントリを保持してクライアント設定を書き、launch_desktopはクライアントを起動する。";
        if (name == "registration_command" || name == "register_desktop") { OpParam p; p.name = "permission"; p.enumValues = {"read", "dry-run", "write"}; op.params.push_back(p); }
        op.exec = [name](OpContext& context, const OpArgs& args) {
            auto status = ai::AiSetupService::Inspect(context.ctx.engineRoot);
            if (name == "registration_command" || name == "register_desktop") {
                if (!status.mcpDistFound || status.mcpStdioPath.empty()) return OpResult::Err("MCP_NOT_FOUND", "MCPの配布ファイルが見つかりません。");
                if (name == "register_desktop") {
                    std::string error; if (!ai::AiSetupService::RegisterClaudeDesktop(status.mcpStdioPath, args.GetString("permission"), error)) return OpResult::Err("REGISTRATION_FAILED", error);
                    status = ai::AiSetupService::Inspect(context.ctx.engineRoot);
                } else {
                    OpData data = OpData::MakeObject(); data.Set("command", OpData(ai::AiSetupService::BuildClaudeCodeCommand(status.mcpStdioPath, args.GetString("permission")))); return OpResult::Data(std::move(data));
                }
            } else if (name == "launch_desktop") {
                std::string error; if (!ai::AiSetupService::LaunchClaudeDesktop(error)) return OpResult::Err("LAUNCH_FAILED", error);
            }
            OpData data = OpData::MakeObject(); data.Set("nodeFound", OpData(status.nodeFound)); data.Set("mcpDistFound", OpData(status.mcpDistFound));
            data.Set("mcpStdioPath", OpData(status.mcpStdioPath)); data.Set("desktopConfigFound", OpData(status.desktopConfigFound));
            data.Set("desktopConfigBroken", OpData(status.desktopConfigBroken)); data.Set("desktopRegistered", OpData(status.desktopRegistered));
            data.Set("desktopPermission", OpData(status.desktopPermission)); data.Set("busRunning", OpData(context.ctx.aiCommandBusRunning)); return OpResult::Data(std::move(data));
        }; registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
