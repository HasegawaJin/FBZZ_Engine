/// @file    EngineRebuildBootstrap.cpp
/// @brief   起動時 Engine 鮮度チェック + 自動リビルド・リランチ。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#include <Engine/Core/EngineRebuildBootstrap.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <Windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fbzz::core {

/// @note Release (配布構成) では自己リビルド機構を持たない。dev パスも埋め込まれない。
#if defined(NDEBUG) || !defined(FBZZ_DEV_BUILD_DIR)

bool CheckEngineFreshnessAndRelaunch() { return false; }

#else

namespace {

namespace fs = std::filesystem;

/// このバイナリを生成したビルド情報 (CMake が埋め込む)
constexpr const char* kDevBuildDir    = FBZZ_DEV_BUILD_DIR;    ///< build.config と同じトップ build ディレクトリ
constexpr const char* kDevEngineRoot  = FBZZ_DEV_ENGINE_ROOT;  ///< engine ソースルート
constexpr const char* kDevCMake       = FBZZ_DEV_CMAKE_COMMAND; ///< configure に使われた cmake.exe
constexpr const char* kBuildConfig    = FBZZ_CMAKE_CONFIG;      ///< Debug / Development

/// module=nullptr で実行中 exe、DLL ハンドルでその DLL のフルパスを返す。
std::wstring GetModulePath(HMODULE module)
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (len == 0) return {};
        if (len < path.size()) { path.resize(len); return path; }
        /// @note バッファ不足: 倍にして再取得
        path.resize(path.size() * 2);
    }
}

/// dir 以下を再帰し、ソース拡張子の最新更新時刻を返す。走査できない場合は最小値。
fs::file_time_type NewestSourceTime(const fs::path& dir)
{
    /// @note (min) の括弧は Windows.h の min マクロによる誤展開を防ぐため。
    fs::file_time_type newest = (fs::file_time_type::min)();
    if (!fs::exists(dir)) return newest;

    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (it != end) {
        const fs::directory_entry entry = *it;

        std::error_code fileEc;
        if (entry.is_regular_file(fileEc) && !fileEc) {
            /// @note ABI (Scene / Component レイアウト) に影響するソースのみ対象にする。
            const fs::path ext = entry.path().extension();
            if (ext == L".cpp" || ext == L".hpp" || ext == L".h" || ext == L".inl") {
                const fs::file_time_type t = fs::last_write_time(entry.path(), fileEc);
                if (!fileEc && t > newest) newest = t;
            }
        }

        /// @note increment がエラーでイテレータを進められない場合、ループが無限化しないよう打ち切る。
        it.increment(ec);
        if (ec) break;
    }
    return newest;
}

/// バッチ用に 1 引数をダブルクオートで囲む (空白を含むパス対策の簡易版)。
std::wstring Quote(const std::wstring& s)
{
    return L"\"" + s + L"\"";
}

/// @brief 「親プロセス終了を待つ → cmake ビルド → 再起動」を行う一時バッチを生成し起動する。
/// @note 実行中プロセスは `FBZZEngine.dll` をロックしているため自プロセスではビルドできず、
///       バッチへ委譲して本体終了・DLL 解放後にビルドさせる。
bool SpawnRebuildAndRelaunch(const std::wstring& exePath)
{
    /// @note 元の起動引数 (argv[1..]) を復元する。
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring relaunchArgs;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            relaunchArgs += L" " + Quote(argv[i]);
        }
        LocalFree(argv);
    }

    std::wstring cwd(MAX_PATH, L'\0');
    const DWORD cwdLen = GetCurrentDirectoryW(static_cast<DWORD>(cwd.size()), cwd.data());
    cwd.resize(cwdLen);

    const DWORD pid = GetCurrentProcessId();
    const std::wstring pidStr    = std::to_wstring(pid);
    const std::wstring buildDirW = util::StringUtils::ToWide(kDevBuildDir);
    const std::wstring configW   = util::StringUtils::ToWide(kBuildConfig);

    /// @note 埋め込み cmake が実在しなければ PATH から探す (SDK 更新等でパスが変わったケース)。
    std::wstring cmakeW = util::StringUtils::ToWide(kDevCMake);
    if (!fs::exists(cmakeW)) {
        wchar_t found[MAX_PATH]{};
        if (SearchPathW(nullptr, L"cmake.exe", nullptr, MAX_PATH, found, nullptr) != 0)
            cmakeW = found;
    }

    /// @note 一時バッチのパス (PID でユニーク化)
    std::wstring tempDir(MAX_PATH, L'\0');
    const DWORD tempLen = GetTempPathW(static_cast<DWORD>(tempDir.size()), tempDir.data());
    tempDir.resize(tempLen);
    const fs::path batchPath = fs::path(tempDir) / (L"fbzz_engine_rebuild_" + pidStr + L".bat");

    /// @note バッチ本文 (cmd は UTF-8 を chcp 65001 で扱う)
    std::wstring bat;
    bat += L"@echo off\r\n";
    bat += L"chcp 65001 >nul\r\n";
    bat += L"rem === FBZZ Engine 自動リビルド・リランチ (EngineRebuildBootstrap が生成) ===\r\n";
    bat += L"rem 1) 親プロセス(PID=" + pidStr + L")が FBZZEngine.dll を解放するまで待機\r\n";
    bat += L":waitloop\r\n";
    bat += L"tasklist /fi \"PID eq " + pidStr + L"\" /nh 2>nul | find \"" + pidStr + L"\" >nul\r\n";
    bat += L"if not errorlevel 1 (\r\n";
    bat += L"    timeout /t 1 /nobreak >nul\r\n";
    bat += L"    goto waitloop\r\n";
    bat += L")\r\n";
    bat += L"rem 2) Engine を含む全ターゲットを再ビルド (増分ビルド)\r\n";
    bat += L"echo === Rebuilding FBZZ Engine (" + configW + L") ===\r\n";
    /// @note --parallel 1: 並列度はルート CMakeLists.txt の /MP${FBZZ_BUILD_JOBS} に一元化しており、
    ///       MSBuild のノード並列 (/m) を開くと「プロジェクト数 × /MP」の cl.exe が同時に走り、
    ///       メモリ使用量が掛け算で膨らんで並行ビルドを巻き添えにする。
    bat += L"" + Quote(cmakeW) + L" --build " + Quote(buildDirW) +
           L" --config " + configW + L" --parallel 1\r\n";
    bat += L"if errorlevel 1 (\r\n";
    bat += L"    echo.\r\n";
    bat += L"    echo *** Engine rebuild FAILED - 上のログを確認してください ***\r\n";
    bat += L"    pause\r\n";
    bat += L"    goto cleanup\r\n";
    bat += L")\r\n";
    bat += L"rem 3) 元の作業ディレクトリ・引数でアプリを再起動\r\n";
    bat += L"cd /d " + Quote(cwd) + L"\r\n";
    /// @note 再起動後のプロセスで再度チェックが走り、ビルドが DLL の更新時刻をソースより
    ///       後にできなかった場合に無限ループするのを防ぐ一回限りのスキップ。
    bat += L"set \"FBZZ_SKIP_ENGINE_REBUILD=1\"\r\n";
    bat += L"start \"\" " + Quote(exePath) + relaunchArgs + L"\r\n";
    bat += L":cleanup\r\n";
    /// @note 自分自身(バッチ)を削除して終了
    bat += L"(goto) 2>nul & del " + Quote(batchPath.wstring()) + L"\r\n";

    /// @note UTF-8 で書き出す (chcp 65001 と対応)
    {
        std::ofstream out(batchPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            FBZZ_LOG_WARN("EngineRebuild: 一時バッチを作成できませんでした: %s",
                          util::StringUtils::ToNarrow(batchPath.wstring()).c_str());
            return false;
        }
        const std::string utf8 = util::StringUtils::ToNarrow(bat);
        out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }

    /// @note 新しいコンソールでバッチを起動 (timeout / echo / pause 用に独立コンソールが要る)
    std::wstring command = L"cmd.exe /c " + Quote(batchPath.wstring());
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                   CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    if (!ok) {
        FBZZ_LOG_WARN("EngineRebuild: リビルドバッチの起動に失敗しました (err=%lu)", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace

bool CheckEngineFreshnessAndRelaunch()
{
    /// @note 再起動直後の一回はスキップ (無限ループ防止)
    if (GetEnvironmentVariableW(L"FBZZ_SKIP_ENGINE_REBUILD", nullptr, 0) != 0)
        return false;

    std::error_code ec;
    const fs::path buildDir   = util::StringUtils::ToWide(kDevBuildDir);
    const fs::path engineRoot = util::StringUtils::ToWide(kDevEngineRoot);

    /// @note 配布物・ソース非同梱環境ではビルド情報が実在しない → 何もしない。
    if (!fs::exists(buildDir, ec) || !fs::exists(engineRoot, ec))
        return false;

    const std::wstring exePath = GetModulePath(nullptr);
    if (exePath.empty()) return false;

    /// @note 実際にロード済みの `FBZZEngine.dll` のパスを取得する。exe が `Binaries/<Config>/Editor/`
    ///       のサブフォルダ、DLL は親フォルダという配置でも正しく比較対象を特定できる。
    const HMODULE engineDll = GetModuleHandleW(L"FBZZEngine.dll");
    if (!engineDll) return false;
    const std::wstring dllPathStr = GetModulePath(engineDll);
    if (dllPathStr.empty()) return false;
    const fs::path dllPath = dllPathStr;

    const fs::file_time_type dllTime = fs::last_write_time(dllPath, ec);
    if (ec) return false;

    /// @note ABI に効く 3 ライブラリのソース最新時刻を求める。
    fs::file_time_type newest = (fs::file_time_type::min)();
    for (const wchar_t* sub : { L"Projects/Math", L"Projects/Physics", L"Projects/Engine" }) {
        const fs::file_time_type t = NewestSourceTime(engineRoot / sub);
        if (t > newest) newest = t;
    }

    /// @note ソースが DLL より新しくなければ最新 → 続行。
    if (newest <= dllTime)
        return false;

    FBZZ_LOG_INFO("EngineRebuild: Engine ソースが FBZZEngine.dll より新しいため、リビルドを提案します");

    const int choice = MessageBoxW(
        nullptr,
        L"Engine のソースが現在のビルドより新しくなっています。\n"
        L"いま再ビルドしてアプリを再起動しますか?\n\n"
        L"[はい]   終了 → cmake で Engine を再ビルド → 自動的に再起動\n"
        L"[いいえ] 現在のビルドのまま起動を続行",
        L"FBZZ Engine - 再ビルド確認",
        MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
    if (choice != IDYES)
        return false;

    return SpawnRebuildAndRelaunch(exePath);
}

#endif // NDEBUG / FBZZ_DEV_BUILD_DIR

} // namespace fbzz::core
