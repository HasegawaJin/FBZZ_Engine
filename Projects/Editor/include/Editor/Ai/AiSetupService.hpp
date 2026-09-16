/// @file    AiSetupService.hpp
/// @brief   AI 連携 (Claude/MCP) のセットアップを Editor 内で完結させるためのサービス。
/// @author  Hasegawa Jin
/// @date    2026-07-20
///
/// 設計 (WHY):
/// MCP サーバ (stdio.js) は Claude クライアントが自動起動するため、ユーザーが行う作業は本来
/// 「登録」と「Bus 有効化」だけ。しかし登録は claude_desktop_config.json の手編集や CLI 実行が
/// 必要で、Editor の外に出る摩擦になっていた。本サービスは環境診断 (node / dist / 登録状態) と
/// Claude Desktop への登録書き込みを Editor 内の 1 クリックへ集約する。
/// ファイル IO / Win32 詳細は .cpp に閉じ、UI (AI Settings Panel) はここの結果を表示するだけにする。
#pragma once
#include <string>

namespace fbzz::editor::ai {

// AI Settings Panel が表示する環境診断の結果。Inspect() が一括で埋める。
struct AiSetupStatus {
    bool        nodeFound = false;           // node.exe が PATH 上にある
    bool        mcpDistFound = false;        // EditorMcp/dist/stdio.js を発見した
    std::string mcpStdioPath;                // 発見した stdio.js の絶対パス (表示・登録に使う)
    bool        desktopConfigFound = false;  // claude_desktop_config.json が存在する
    bool        desktopConfigBroken = false; // 存在するが JSON として解析できない (上書き禁止)
    bool        desktopRegistered = false;   // fbzz-editor エントリが登録済み
    std::string desktopPermission;           // 登録済みエントリの FBZZ_MCP_PERMISSION (空 = 不明)
};

class AiSetupService {
public:
    // 環境を診断する。engineRootHint (EditorContext::engineRoot) が空でも
    // 実行ファイル位置から親を遡って EditorMcp を探索する。
    static AiSetupStatus Inspect(const std::string& engineRootHint);

    // Claude Desktop の claude_desktop_config.json へ fbzz-editor を登録 (または権限更新) する。
    // 既存 config の他エントリは保持する。解析できない config は安全のため上書きしない。
    static bool RegisterClaudeDesktop(const std::string& stdioJsPath,
                                      const std::string& permission,
                                      std::string& error);

    // Claude Code (CLI) 用の登録コマンド文字列を作る (クリップボードコピー用)。
    static std::string BuildClaudeCodeCommand(const std::string& stdioJsPath,
                                              const std::string& permission);

    // Claude Desktop アプリを起動する (インストール済みの場合)。
    static bool LaunchClaudeDesktop(std::string& error);
};

} // namespace fbzz::editor::ai
