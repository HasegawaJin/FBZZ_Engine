/// @file    AiSetupService.cpp
/// @brief   AI 連携セットアップの実装。診断・Claude Desktop 登録・起動を Win32/FS 操作で行う。
/// @author  Hasegawa Jin
/// @date    2026-07-20
#include <Editor/Ai/AiSetupService.hpp>

#include <Editor/Ai/Json.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#pragma comment(lib, "shell32.lib") // ShellExecuteW (Claude Desktop 起動)

namespace fbzz::editor::ai {

namespace fs = std::filesystem;

namespace {

std::string GetEnvVar(const char* name)
{
    char buffer[1024];
    const DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    return (length > 0 && length < sizeof(buffer)) ? std::string(buffer, length) : std::string{};
}

// claude_desktop_config.json の場所 (%APPDATA%\Claude\)。APPDATA 未設定なら空を返す。
fs::path DesktopConfigPath()
{
    const std::string appData = GetEnvVar("APPDATA");
    if (appData.empty()) return {};
    return fs::path(appData) / "Claude" / "claude_desktop_config.json";
}

// Editor 実行ファイルのあるディレクトリ。EditorMcp 探索の起点にする。
fs::path ExecutableDir()
{
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return fs::path(buffer).parent_path();
}

// engineRoot ヒントと exe 位置の親探索で EditorMcp/dist/stdio.js を解決する。
// WHY: Editor は <repo>/build/<cfg>/Binaries/<cfg>/Editor/ から起動されるため、
//      リポジトリルートは実行時に確定しない。GameHub の Editor 探索と同じ遡り方式を使う。
std::string ResolveMcpStdioPath(const std::string& engineRootHint)
{
    std::error_code ec;
    const auto probe = [&ec](const fs::path& root) -> std::string {
        const fs::path candidate = root / "Projects" / "EditorMcp" / "dist" / "stdio.js";
        return fs::exists(candidate, ec) ? candidate.generic_string() : std::string{};
    };

    if (!engineRootHint.empty()) {
        if (std::string found = probe(fs::path(engineRootHint)); !found.empty()) return found;
    }

    fs::path current = ExecutableDir();
    for (int depth = 0; depth < 10 && !current.empty(); ++depth) {
        if (std::string found = probe(current); !found.empty()) return found;
        const fs::path parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return {};
}

// ファイル全体を読む。失敗時は nullopt ではなく空文字 (存在チェックは呼び出し側で済ませる)。
std::string ReadTextFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

// 通常インストーラ版 claude.exe の候補パスを広めに列挙する。
// WHY: インストーラ版は %LOCALAPPDATA%\AnthropicClaude\claude.exe 直下だけでなく、
//      squirrel 形式の "app-<version>\claude.exe" サブフォルダに置かれる場合がある。
std::vector<fs::path> ClaudeExeCandidates(const std::string& localAppData)
{
    std::vector<fs::path> out;
    const fs::path base(localAppData);
    out.push_back(base / "AnthropicClaude" / "claude.exe");
    out.push_back(base / "Programs" / "Claude" / "Claude.exe");

    std::error_code ec;
    const fs::path anthropicDir = base / "AnthropicClaude";
    if (fs::exists(anthropicDir, ec) && fs::is_directory(anthropicDir, ec)) {
        for (const auto& entry : fs::directory_iterator(anthropicDir, ec)) {
            if (ec || !entry.is_directory(ec)) continue;
            out.push_back(entry.path() / "claude.exe");
        }
    }
    return out;
}

// PowerShell の Get-StartApps を経由して Claude を起動する (フォールバック)。
// WHY: Microsoft Store (MSIX) 版は claude.exe への直接パスが存在せず、実体は
//      権限制限された WindowsApps 配下にあるため直接探索できない。Get-StartApps は
//      アプリの種別 (Win32/MSIX) を問わず Start メニュー登録から AppID を引けるため、
//      shell:AppsFolder\<AppID> 経由で確実に起動できる。
bool LaunchViaStartApps()
{
    std::wstring command =
        L"powershell.exe -NoProfile -WindowStyle Hidden -Command "
        L"\"$a = Get-StartApps | Where-Object { $_.Name -like '*Claude*' } | Select-Object -First 1; "
        L"if ($a) { Start-Process ('shell:AppsFolder\\' + $a.AppID) }\"";

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                         CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo);
    if (!created) return false;
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}

// 実行中プロセス一覧から "claude.exe" / "Claude.exe" を探し、PID を返す (未検出は 0)。
// WHY: Claude Desktop を多重起動すると GPUCache フォルダの取り合いになり
//      "Unable to move the cache" / ERROR_ACCESS_DENIED (0x5) を引き起こすことがある。
//      起動前に既存プロセスの有無を確認し、あれば新規起動ではなく前面表示に切り替える。
DWORD FindRunningClaudeProcessId()
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    DWORD foundPid = 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"claude.exe") == 0) {
                foundPid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return foundPid;
}

struct FindWindowContext {
    DWORD targetPid = 0;
    HWND  found = nullptr;
};

BOOL CALLBACK FindTopWindowForProcess(HWND hwnd, LPARAM lParam)
{
    auto* context = reinterpret_cast<FindWindowContext*>(lParam);
    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid == context->targetPid && IsWindowVisible(hwnd)) {
        context->found = hwnd;
        return FALSE; // 発見したので列挙を打ち切る
    }
    return TRUE;
}

// 既に起動している Claude Desktop のトップウィンドウを前面に出す。ウィンドウが見つからない場合は false。
bool FocusRunningClaudeWindow(DWORD pid)
{
    FindWindowContext context;
    context.targetPid = pid;
    EnumWindows(FindTopWindowForProcess, reinterpret_cast<LPARAM>(&context));
    if (context.found == nullptr) return false;

    if (IsIconic(context.found)) ShowWindow(context.found, SW_RESTORE);
    SetForegroundWindow(context.found);
    return true;
}

} // namespace

AiSetupStatus AiSetupService::Inspect(const std::string& engineRootHint)
{
    AiSetupStatus status;

    // node.exe: Claude クライアントが MCP サーバを spawn するのに必要。
    wchar_t nodePath[MAX_PATH];
    status.nodeFound = SearchPathW(nullptr, L"node.exe", nullptr, MAX_PATH, nodePath, nullptr) > 0;

    status.mcpStdioPath = ResolveMcpStdioPath(engineRootHint);
    status.mcpDistFound = !status.mcpStdioPath.empty();

    const fs::path configPath = DesktopConfigPath();
    std::error_code ec;
    if (!configPath.empty() && fs::exists(configPath, ec)) {
        status.desktopConfigFound = true;
        const std::optional<JsonValue> parsed = ParseJson(ReadTextFile(configPath));
        if (!parsed.has_value() || !parsed->IsObject()) {
            status.desktopConfigBroken = true;
        } else if (const JsonValue* servers = parsed->Find("mcpServers")) {
            if (const JsonValue* entry = servers->Find("fbzz-editor")) {
                status.desktopRegistered = true;
                if (const JsonValue* env = entry->Find("env")) {
                    if (const JsonValue* permission = env->Find("FBZZ_MCP_PERMISSION")) {
                        if (permission->IsString()) status.desktopPermission = permission->AsString();
                    }
                }
            }
        }
    }
    return status;
}

bool AiSetupService::RegisterClaudeDesktop(const std::string& stdioJsPath,
                                           const std::string& permission,
                                           std::string& error)
{
    if (stdioJsPath.empty()) {
        error = "stdio.js が見つかりません。EditorMcp を npm run build してください";
        return false;
    }
    const fs::path configPath = DesktopConfigPath();
    if (configPath.empty()) {
        error = "APPDATA 環境変数が取得できません";
        return false;
    }

    // 既存 config を読み、他の MCP サーバ登録を保持したまま fbzz-editor だけ更新する。
    JsonValue root = JsonValue::MakeObject();
    std::error_code ec;
    if (fs::exists(configPath, ec)) {
        std::string parseError;
        const std::optional<JsonValue> parsed = ParseJson(ReadTextFile(configPath), &parseError);
        if (!parsed.has_value() || !parsed->IsObject()) {
            // 解析不能な既存設定を上書きすると他ツールの登録を壊すため、安全側で中止する。
            error = "既存の claude_desktop_config.json を解析できないため中止しました: " + parseError;
            return false;
        }
        root = *parsed;
    } else {
        fs::create_directories(configPath.parent_path(), ec);
        if (ec) {
            error = "設定フォルダを作成できません: " + ec.message();
            return false;
        }
    }

    JsonValue args = JsonValue::MakeArray();
    args.Push(JsonValue(stdioJsPath));
    JsonValue env = JsonValue::MakeObject();
    env.Set("FBZZ_MCP_PERMISSION", JsonValue(permission));
    JsonValue entry = JsonValue::MakeObject();
    entry.Set("command", JsonValue("node"));
    entry.Set("args", std::move(args));
    entry.Set("env", std::move(env));

    JsonValue servers = JsonValue::MakeObject();
    if (const JsonValue* existing = root.Find("mcpServers"); existing != nullptr && existing->IsObject()) {
        servers = *existing;
    }
    servers.Set("fbzz-editor", std::move(entry));
    root.Set("mcpServers", std::move(servers));

    std::ofstream output(configPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "claude_desktop_config.json へ書き込めません";
        return false;
    }
    output << SerializeJson(root);
    return true;
}

std::string AiSetupService::BuildClaudeCodeCommand(const std::string& stdioJsPath,
                                                   const std::string& permission)
{
    return "claude mcp add fbzz-editor -e FBZZ_MCP_PERMISSION=" + permission
         + " -- node \"" + stdioJsPath + "\"";
}

bool AiSetupService::LaunchClaudeDesktop(std::string& error)
{
    // 0. 既に起動中なら多重起動しない。二重起動は GPUCache フォルダの取り合いで
    //    "Unable to move the cache" (ERROR_ACCESS_DENIED) の原因になるため、
    //    既存プロセスのウィンドウを前面に出すだけに留める。
    if (const DWORD runningPid = FindRunningClaudeProcessId(); runningPid != 0) {
        FocusRunningClaudeWindow(runningPid); // 失敗してもタスクバーには存在するので致命的ではない
        return true;
    }

    const std::string localAppData = GetEnvVar("LOCALAPPDATA");
    if (localAppData.empty()) {
        error = "LOCALAPPDATA 環境変数が取得できません";
        return false;
    }

    // 1. 通常インストーラ版: claude.exe への直接パスが見つかればそれを起動する。
    std::error_code ec;
    for (const fs::path& candidate : ClaudeExeCandidates(localAppData)) {
        if (fs::exists(candidate, ec)) {
            const HINSTANCE result = ShellExecuteW(nullptr, L"open", candidate.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(result) > 32) return true; // ShellExecute は 32 以下がエラー
        }
    }

    // 2. Microsoft Store (MSIX) 版などのフォールバック。
    if (LaunchViaStartApps()) return true;

    error = "Claude Desktop の実行ファイルが見つかりません。手動で起動してください";
    return false;
}

} // namespace fbzz::editor::ai
