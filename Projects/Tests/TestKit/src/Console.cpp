/// @file    Console.cpp
/// @brief   コンソール確保・文字サイズ・入力待ちの実装。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <TestKit/Console.hpp>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <Windows.h>

#include <conio.h>
#include <crtdbg.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <iterator>
#include <string_view>

namespace fbzz::testkit {

namespace {

bool HasEnv(const char* name)
{
    size_t length = 0;
    return getenv_s(&length, nullptr, 0, name) == 0 && length > 0;
}

int EnvInt(const char* name, int fallback)
{
    char   buffer[32] = {};
    size_t length     = 0;
    if (getenv_s(&length, buffer, sizeof(buffer), name) != 0 || length == 0) return fallback;

    const int value = std::atoi(buffer);
    return value > 0 ? value : fallback;
}

/// 画面の DPI に合わせて文字の高さを決める。
/// WHY: CONSOLE_FONT_INFOEX の高さは論理 px なので、4K の高 DPI 環境では
///      96dpi 想定の値をそのまま渡すと «読めないほど小さい» ままになる。
int ScaledFontHeight(HWND console, int baseHeight)
{
    int dpi = 96;
    if (console) {
        const UINT queried = GetDpiForWindow(console);
        if (queried > 0) dpi = static_cast<int>(queried);
    }

    int height = MulDiv(baseHeight, dpi, 96);
    if (height < 14) height = 14;
    if (height > 48) height = 48;
    return height;
}

void ApplyFont(HANDLE output, HWND console, const ConsoleOptions& options)
{
    const int baseHeight = EnvInt("FBZZ_CONSOLE_FONT_SIZE", options.fontHeight);

    CONSOLE_FONT_INFOEX font = {};
    font.cbSize     = sizeof(font);
    font.nFont      = 0;
    font.dwFontSize = {0, static_cast<SHORT>(ScaledFontHeight(console, baseHeight))};
    font.FontFamily = FF_DONTCARE;
    font.FontWeight = FW_NORMAL;
    wcscpy_s(font.FaceName, options.faceName);

    SetCurrentConsoleFontEx(output, FALSE, &font);
}

void ApplyBufferSize(HANDLE output, int bufferLines)
{
    CONSOLE_SCREEN_BUFFER_INFO info = {};
    if (!GetConsoleScreenBufferInfo(output, &info)) return;

    // WHY 窓を先に縮めるか: バッファは常に窓以上でなければならず、順序を誤ると
    //     どちらの API も «引数が不正» で黙って失敗する。
    SMALL_RECT collapsed = {0, 0, 1, 1};
    SetConsoleWindowInfo(output, TRUE, &collapsed);

    const SHORT width = info.srWindow.Right - info.srWindow.Left + 1;
    COORD       size{width > 0 ? width : 120, static_cast<SHORT>(bufferLines)};
    SetConsoleScreenBufferSize(output, size);

    SMALL_RECT restored = info.srWindow;
    restored.Left       = 0;
    restored.Right      = static_cast<SHORT>(size.X - 1);
    SetConsoleWindowInfo(output, TRUE, &restored);
}

/// このコンソールに繋がっているプロセスが自分だけか。
/// WHY: 「自分がコンソールを作ったか」だけでは足りない。Visual Studio が F5 で
///      起動したコンソールアプリは OS がコンソールを用意するので AllocConsole を
///      通らないが、繋がっているのは自分だけなので終了と同時に窓が消える。
///      ターミナルや VS Code のタスクから起動した場合はシェルも繋がっているため
///      窓は残る ─ そこで入力待ちすると、かえって邪魔になる。
bool IsSoleConsoleOwner()
{
    DWORD       processIds[8] = {};
    const DWORD count = GetConsoleProcessList(processIds, static_cast<DWORD>(std::size(processIds)));
    // 0 は「コンソールに繋がっていない」= 呼び出し失敗。閉じる想定で扱う。
    return count <= 1;
}

void RedirectStandardStreams()
{
    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);

    // gtest は printf 経由で書く。バッファに溜めたまま落ちると出力が消えるので、
    // 行単位で吐かせる。テストの実行時間に対して無視できるコスト。
    setvbuf(stdout, nullptr, _IOLBF, 4096);
}

} // namespace

bool IsAutomatedRun(int argc, char** argv)
{
    // CTest は起動する全テストにこれを立てる。
    if (HasEnv("CTEST_INTERACTIVE_DEBUG_MODE")) return true;
    if (HasEnv("CI")) return true;
    if (HasEnv("FBZZ_TEST_NO_PAUSE")) return true;

    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        const std::string_view argument{argv[i]};

        // gtest_discover_tests がビルド中に走らせる列挙。ここで止まるとビルドがハングする。
        if (argument.starts_with("--gtest_list_tests")) return true;
        // レポート出力を要求している = 誰かが機械で読む実行。
        if (argument.starts_with("--gtest_output")) return true;
    }
    return false;
}

bool EnsureConsole(const ConsoleOptions& options)
{
    bool ownsConsole = false;

    if (GetConsoleWindow() == nullptr) {
        // 親のコンソール (VS の出力先や cmd) があればそれを借りる。
        // 無ければ自前で作る ─ このときだけ、終了と同時に窓が消える。
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
            if (!AllocConsole()) return false;
            ownsConsole = true;
        }
        RedirectStandardStreams();
    }

    // 自分で作った場合はもちろん、OS / デバッガが用意したコンソールでも
    // 繋がっているのが自分だけなら終了と同時に消える。どちらも入力待ちが要る。
    const bool closesWithUs = ownsConsole || IsSoleConsoleOwner();

    const HANDLE output  = GetStdHandle(STD_OUTPUT_HANDLE);
    const HWND   console = GetConsoleWindow();
    if (output == INVALID_HANDLE_VALUE) return closesWithUs;

    // 日本語のテスト説明とファイル名を化けさせない。
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    ApplyFont(output, console, options);
    ApplyBufferSize(output, options.bufferLines);

    return closesWithUs;
}

void WaitForKey(const char* message)
{
    if (message && *message) {
        std::printf("\n%s", message);
        std::fflush(stdout);
    }
    _getch();
}

void SuppressBlockingErrorDialogs()
{
    // クリティカルエラーと «アプリケーションエラー» の窓を出さずに終了させる。
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    // abort() の «Debug Error!» ダイアログを止め、メッセージだけ stderr へ残す。
    _set_abort_behavior(_WRITE_ABORT_MSG, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

#ifdef _DEBUG
    // assert 失敗の報告先をダイアログから stderr へ切り替える。
    // WHY 3 種類とも指定するか: assert は _CRT_ASSERT だが、CRT の内部検査
    //     (イテレーターの範囲外・二重解放) は _CRT_ERROR で上がる。どちらも止まる。
    for (const int reportType : { _CRT_ASSERT, _CRT_ERROR, _CRT_WARN }) {
        _CrtSetReportMode(reportType, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(reportType, _CRTDBG_FILE_STDERR);
    }
#endif
}

} // namespace fbzz::testkit
