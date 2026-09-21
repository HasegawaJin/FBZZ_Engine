/// @file    CrashHandler.cpp
/// @brief   未処理例外でミニダンプとログの末尾を書き、次回起動時に知らせる。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @see Docs/design/crash-report.md
#include "Engine/Core/CrashHandler.hpp"
#include "Engine/Core/DeveloperMode.hpp"
#include "Engine/Core/ILogSink.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Util/StringUtils.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <DbgHelp.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

namespace fbzz::core {
namespace {

using util::StringUtils;

constexpr size_t kCrashLogLines     = 200;
constexpr size_t kCrashLogLineBytes = 512;
constexpr size_t kCrashPathChars    = 1024;
constexpr size_t kCrashReasonBytes  = 256;
constexpr DWORD  kCrashWriterStackBytes = 256 * 1024;
/// @note ローダーロックを持ったまま落ちると書き出しスレッドが始まらない。そのときは落ちたスレッドで書く。
constexpr DWORD  kCrashWriterTimeoutMs  = 30 * 1000;
/// @note スタックオーバーフロー後もスレッドを作れるだけのスタックを残す。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadstackguarantee SetThreadStackGuarantee
constexpr ULONG  kCrashStackGuaranteeBytes = 64 * 1024;
/// @brief SEH 例外ではない落ち方 (abort・purecall・不正引数・WriteReport) をダンプの例外ストリームに載せるときのコード。
constexpr DWORD  kCrashSoftwareCode = 0xE046425Au;
/// @brief abort() と同じ終了コード。
constexpr UINT   kCrashAbortExitCode = 3;

/// @brief スタックと、スタックから指されるメモリまで。全メモリは含めず数 MB に収める。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/ne-minidumpapiset-minidump_type MINIDUMP_TYPE
constexpr MINIDUMP_TYPE kCrashDumpType = static_cast<MINIDUMP_TYPE>(
    MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

#ifdef FBZZ_ENGINE_VERSION_STRING
constexpr const char* kCrashEngineVersion = FBZZ_ENGINE_VERSION_STRING;
#else
constexpr const char* kCrashEngineVersion = "unknown";
#endif
#ifdef FBZZ_CMAKE_CONFIG
constexpr const char* kCrashBuildConfig = FBZZ_CMAKE_CONFIG;
#else
constexpr const char* kCrashBuildConfig = "unknown";
#endif

const char* CrashLevelPrefix(LogLevel level)
{
    switch (level) {
    case LogLevel::DEBUG:     return "[DEBUG] ";
    case LogLevel::INFO:      return "[INFO]  ";
    case LogLevel::WARNING:   return "[WARN]  ";
    case LogLevel::LOG_ERROR: return "[ERROR] ";
    }
    return "";
}

/// @brief ログの末尾を固定長の輪で持つ。落ちた後にヒープを触らずに読むため。
/// @note Logger が配送を直列化しているので書き手は同時に 1 本。落ちた後の読み手はロックを取らない (1 行が欠けうる)。
class CrashLogTail final : public ILogSink {
public:
    void OnLog(const LogEntry& entry) override
    {
        const uint32_t index = m_next.load(std::memory_order_relaxed);
        char* line = m_lines[index % kCrashLogLines];

        const char* prefix = CrashLevelPrefix(entry.level);
        const size_t prefixLen = std::strlen(prefix);
        std::memcpy(line, prefix, prefixLen);

        /// @note UTF-8 の途中で切らない。継続バイト (10xxxxxx) の手前まで戻す。
        size_t length = std::min(entry.message.size(), kCrashLogLineBytes - 1 - prefixLen);
        while (length > 0 && length < entry.message.size()
               && (static_cast<unsigned char>(entry.message[length]) & 0xC0u) == 0x80u)
            --length;
        std::memcpy(line + prefixLen, entry.message.data(), length);
        line[prefixLen + length] = '\0';

        m_next.store(index + 1, std::memory_order_release);
    }

    /// @brief 古い順に 1 行ずつ渡す。
    template <class Fn>
    void ForEachLine(Fn&& fn) const
    {
        const uint32_t next  = m_next.load(std::memory_order_acquire);
        const uint32_t count = std::min<uint32_t>(next, static_cast<uint32_t>(kCrashLogLines));
        for (uint32_t i = next - count; i != next; ++i)
            fn(m_lines[i % kCrashLogLines]);
    }

    void Clear() { m_next.store(0, std::memory_order_release); }

private:
    char                  m_lines[kCrashLogLines][kCrashLogLineBytes] = {};
    std::atomic<uint32_t> m_next{0};
};

/// @brief 1 回の書き出しの入出力。落ちたときは静的領域のものを使い、落ちたスタックに置かない。
struct CrashRequest {
    EXCEPTION_POINTERS* exception = nullptr;
    DWORD               threadId  = 0;
    char                reason[kCrashReasonBytes] = {};  ///< 空なら例外コードから «何が起きたか» を作る
    wchar_t             outDir[kCrashPathChars]   = {};
    std::atomic<bool>   claimed{false};                  ///< 書き出しスレッドと落ちたスレッドのどちらが書くか
    bool                written = false;
};

struct CrashState {
    bool     installed = false;
    wchar_t  savedDir[kCrashPathChars] = {};  ///< `<根>\Saved`
    wchar_t  crashDir[kCrashPathChars] = {};  ///< `<根>\Saved\Crashes`
    char     appName[128] = {};

    LPTOP_LEVEL_EXCEPTION_FILTER prevFilter   = nullptr;
    _invalid_parameter_handler   prevInvalid  = nullptr;
    _purecall_handler            prevPurecall = nullptr;
    _crt_signal_t                prevAbort    = SIG_DFL;
    unsigned int                 prevAbortBehavior = 0;

    std::atomic<bool> handling{false};
    std::atomic<long> serial{0};
    CrashRequest      crashRequest;
};

CrashState& GetCrashState()
{
    static CrashState state;
    return state;
}

CrashLogTail& GetCrashLogTail()
{
    static CrashLogTail tail;
    return tail;
}

void CrashWrite(HANDLE file, const char* text, size_t length)
{
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
}

void CrashPrintf(HANDLE file, const char* fmt, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    const int length = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (length > 0)
        CrashWrite(file, buffer, std::min(static_cast<size_t>(length), sizeof(buffer) - 1));
}

/// @see https://learn.microsoft.com/en-us/windows/win32/debug/getexceptioncode GetExceptionCode (例外コードの一覧)
const char* CrashExceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:             return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case 0xC0000374u:                        return "STATUS_HEAP_CORRUPTION";
    case 0xE06D7363u:                        return "C++ exception";
    default:                                 return "unknown exception";
    }
}

/// @brief アドレスを `モジュール名+オフセット` で書く。PDB と突き合わせれば行が分かる。
void CrashDescribeAddress(const void* address, char* out, size_t capacity)
{
    HMODULE module = nullptr;
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    wchar_t modulePath[MAX_PATH] = {};
    if (!GetModuleHandleExW(flags, static_cast<LPCWSTR>(address), &module) || !module
        || GetModuleFileNameW(module, modulePath, MAX_PATH) == 0) {
        snprintf(out, capacity, "0x%016llX", static_cast<unsigned long long>(std::bit_cast<uintptr_t>(address)));
        return;
    }
    const wchar_t* slash = std::wcsrchr(modulePath, L'\\');
    const wchar_t* name  = slash ? slash + 1 : modulePath;
    char nameUtf8[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, nameUtf8, static_cast<int>(sizeof(nameUtf8)), nullptr, nullptr);
    const uintptr_t offset = std::bit_cast<uintptr_t>(address) - std::bit_cast<uintptr_t>(module);
    snprintf(out, capacity, "%s+0x%llX", nameUtf8, static_cast<unsigned long long>(offset));
}

/// @brief «何が起きたか» の 1 行。report.txt の `what:` と次回起動時の通知に使う。
void CrashDescribeWhat(const CrashRequest& request, char* out, size_t capacity)
{
    if (request.reason[0] != '\0') {
        snprintf(out, capacity, "%s", request.reason);
        return;
    }
    if (!request.exception || !request.exception->ExceptionRecord) {
        snprintf(out, capacity, "unknown");
        return;
    }
    const EXCEPTION_RECORD& record = *request.exception->ExceptionRecord;
    char where[MAX_PATH * 3 + 32] = {};
    CrashDescribeAddress(record.ExceptionAddress, where, sizeof(where));
    snprintf(out, capacity, "%s (0x%08lX) at %s",
             CrashExceptionName(record.ExceptionCode), record.ExceptionCode, where);
}

HANDLE CrashCreateFile(const wchar_t* dir, const wchar_t* name)
{
    wchar_t path[kCrashPathChars + 32] = {};
    swprintf_s(path, L"%s\\%s", dir, name);
    return CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

/// @brief ダンプとレポートを書く。ヒープを使わない。
/// @note in-process の MiniDumpWriteDump は呼んだスレッドのスタックを正しく残せないので、別スレッドから呼ぶ。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump MiniDumpWriteDump (Remarks)
void CrashWriteFiles(CrashRequest& request)
{
    CrashState& state = GetCrashState();

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    const long serial = ++state.serial;
    const DWORD pid = GetCurrentProcessId();
    if (serial == 1) {
        swprintf_s(request.outDir, L"%s\\%04u-%02u-%02u_%02u-%02u-%02u_%lu", state.crashDir,
                   now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, pid);
    } else {
        swprintf_s(request.outDir, L"%s\\%04u-%02u-%02u_%02u-%02u-%02u_%lu_%ld", state.crashDir,
                   now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, pid, serial);
    }
    CreateDirectoryW(state.savedDir, nullptr);
    CreateDirectoryW(state.crashDir, nullptr);
    if (!CreateDirectoryW(request.outDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return;

    bool dumpWritten = false;
    DWORD dumpError = 0;
    const HANDLE dump = CrashCreateFile(request.outDir, L"crash.dmp");
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exceptionInfo = {};
        exceptionInfo.ThreadId          = request.threadId;
        exceptionInfo.ExceptionPointers = request.exception;
        exceptionInfo.ClientPointers    = FALSE;
        dumpWritten = MiniDumpWriteDump(GetCurrentProcess(), pid, dump, kCrashDumpType,
                                        request.exception ? &exceptionInfo : nullptr, nullptr, nullptr) != FALSE;
        if (!dumpWritten) dumpError = GetLastError();
        CloseHandle(dump);
    } else {
        dumpError = GetLastError();
    }

    const HANDLE report = CrashCreateFile(request.outDir, L"report.txt");
    if (report == INVALID_HANDLE_VALUE)
        return;

    char what[kCrashReasonBytes + MAX_PATH * 3 + 64] = {};
    CrashDescribeWhat(request, what, sizeof(what));

    char commandLine[4096] = {};
    WideCharToMultiByte(CP_UTF8, 0, GetCommandLineW(), -1, commandLine, static_cast<int>(sizeof(commandLine)), nullptr, nullptr);

    CrashPrintf(report, "FBZZ crash report\r\n");
    CrashPrintf(report, "app:     %s\r\n", state.appName);
    CrashPrintf(report, "time:    %04u-%02u-%02u %02u:%02u:%02u\r\n",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    CrashPrintf(report, "engine:  %s (%s)\r\n", kCrashEngineVersion, kCrashBuildConfig);
    CrashPrintf(report, "what:    %s\r\n", what);
    if (request.exception && request.exception->ExceptionRecord) {
        const EXCEPTION_RECORD& record = *request.exception->ExceptionRecord;
        /// @note アクセス違反の ExceptionInformation は [0] = 0 読み / 1 書き / 8 DEP、[1] = 触ったアドレス。
        /// @see https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record EXCEPTION_RECORD
        if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
            const ULONG_PTR kind = record.ExceptionInformation[0];
            const char* verb = kind == 0 ? "read from" : kind == 1 ? "write to" : kind == 8 ? "execute at" : "access";
            CrashPrintf(report, "detail:  %s 0x%016llX\r\n", verb,
                        static_cast<unsigned long long>(record.ExceptionInformation[1]));
        }
    }
    CrashPrintf(report, "thread:  %lu\r\n", request.threadId);
    CrashPrintf(report, "command: %s\r\n", commandLine);
    if (dumpWritten)
        CrashPrintf(report, "dump:    crash.dmp\r\n");
    else
        CrashPrintf(report, "dump:    failed (error %lu)\r\n", dumpError);

    CrashPrintf(report, "\r\n--- log (oldest first, last %u lines) ---\r\n", static_cast<unsigned>(kCrashLogLines));
    GetCrashLogTail().ForEachLine([report](const char* line) {
        CrashWrite(report, line, std::strlen(line));
        CrashWrite(report, "\r\n", 2);
    });
    CloseHandle(report);
    request.written = true;
}

DWORD WINAPI CrashWriterThread(LPVOID param)
{
    CrashRequest& request = *static_cast<CrashRequest*>(param);
    if (!request.claimed.exchange(true))
        CrashWriteFiles(request);
    return 0;
}

/// @brief 別スレッドで書き、終わるまで待つ。スレッドが始まらなければ呼んだスレッドで書く。
void CrashRunWriter(CrashRequest& request, DWORD timeoutMs)
{
    const HANDLE thread = CreateThread(nullptr, kCrashWriterStackBytes, &CrashWriterThread, &request, 0, nullptr);
    if (thread) {
        WaitForSingleObject(thread, timeoutMs);
        CloseHandle(thread);
    }
    if (!request.claimed.exchange(true))
        CrashWriteFiles(request);
}

/// @brief 呼んだ地点の CONTEXT を例外レコードに包む。ダンプを開くとこの地点で止まって見える。
struct CrashCapturedContext {
    CONTEXT            context = {};
    EXCEPTION_RECORD   record  = {};
    EXCEPTION_POINTERS pointers = {};

    void Capture()
    {
        RtlCaptureContext(&context);
        record.ExceptionCode = kCrashSoftwareCode;
#ifdef _M_X64
        record.ExceptionAddress = std::bit_cast<PVOID>(static_cast<uintptr_t>(context.Rip));
#endif
        pointers.ExceptionRecord = &record;
        pointers.ContextRecord   = &context;
    }
};

/// @brief 1 つ目の落ちたスレッドだけが書き、終わったらプロセスを終える。2 つ目以降は終わるまで眠らせる。
[[noreturn]] void CrashHandleFatal(EXCEPTION_POINTERS* exception, const char* reason, UINT exitCode)
{
    CrashState& state = GetCrashState();
    if (state.handling.exchange(true)) {
        for (;;) Sleep(INFINITE);
    }
    CrashRequest& request = state.crashRequest;
    request.exception = exception;
    request.threadId  = GetCurrentThreadId();
    if (reason) strncpy_s(request.reason, reason, _TRUNCATE);
    CrashRunWriter(request, kCrashWriterTimeoutMs);
    TerminateProcess(GetCurrentProcess(), exitCode);
    for (;;) Sleep(INFINITE);
}

/// @see https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-setunhandledexceptionfilter SetUnhandledExceptionFilter
LONG WINAPI CrashUnhandledFilter(EXCEPTION_POINTERS* exception)
{
    CrashHandleFatal(exception, nullptr, exception && exception->ExceptionRecord
                                             ? exception->ExceptionRecord->ExceptionCode
                                             : kCrashAbortExitCode);
}

/// @note この 3 つは CRT が既定で WER を直接呼び、SetUnhandledExceptionFilter を通らない。
[[noreturn]] void CrashHandleSoftware(const char* reason)
{
    static CrashCapturedContext captured;
    captured.Capture();
    CrashHandleFatal(&captured.pointers, reason, kCrashAbortExitCode);
}

/// @see https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/signal signal
void __cdecl CrashOnAbort(int)
{
    CrashHandleSoftware("abort()");
}

/// @see https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/get-purecall-handler-set-purecall-handler _set_purecall_handler
void __cdecl CrashOnPurecall()
{
    CrashHandleSoftware("pure virtual function call");
}

/// @see https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/set-invalid-parameter-handler-set-thread-local-invalid-parameter-handler _set_invalid_parameter_handler
void __cdecl CrashOnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
{
    CrashHandleSoftware("invalid parameter passed to a CRT function");
}

/// @name わざと落とす
/// @note 最適化で消されないよう、どれも volatile か noinline を通す。

__declspec(noinline) void CrashTriggerStackOverflow(volatile unsigned depth)
{
    volatile char frame[4096];
    frame[0] = static_cast<char>(depth);
    /// @note 終わらない再帰に見せない (C4717) ための番兵。実際には届かない。
    if (depth == 0xFFFFFFFFu) return;
    CrashTriggerStackOverflow(depth + 1);
    frame[1] = frame[0];
}

/// @brief 構築中の基底から純粋仮想関数を呼ぶと、vtable が基底のものなので _purecall へ落ちる。
struct CrashPureCallBase {
    CrashPureCallBase() { CallPure(); }
    virtual ~CrashPureCallBase() = default;
    virtual void Pure() = 0;
    __declspec(noinline) void CallPure() { Pure(); }
};

struct CrashPureCallDerived final : CrashPureCallBase {
    void Pure() override {}
};

std::string CrashReadSummary(const std::filesystem::path& report)
{
    std::ifstream in(report);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("what:", 0) != 0) continue;
        line.erase(0, 5);
        /// @note 前後を同じ集合で削る。前だけ ' ' で削ると «空白だけの値» のとき末尾が npos になり、
        ///       npos - begin + 1 が巨大な長さに化けて CR がそのまま残る。
        const size_t begin = line.find_first_not_of(" \r");
        const size_t end   = line.find_last_not_of(" \r");
        if (begin == std::string::npos) return {};
        return line.substr(begin, end - begin + 1);
    }
    return {};
}

} // namespace

bool CrashHandler::Install(const std::filesystem::path& root, std::string_view appName)
{
    CrashState& state = GetCrashState();

    const std::filesystem::path saved   = root / L"Saved";
    const std::filesystem::path crashes = saved / L"Crashes";
    if (crashes.native().size() + 64 >= kCrashPathChars) {
        FBZZ_LOG_WARN("CrashHandler: パスが長すぎるため無効にしました: %s", StringUtils::PathToUtf8(crashes).c_str());
        return false;
    }
    wcsncpy_s(state.savedDir, saved.c_str(), _TRUNCATE);
    wcsncpy_s(state.crashDir, crashes.c_str(), _TRUNCATE);
    strncpy_s(state.appName, std::string(appName).c_str(), _TRUNCATE);

    if (state.installed)
        return true;

    Logger::AddSink(&GetCrashLogTail());
    state.prevFilter      = SetUnhandledExceptionFilter(&CrashUnhandledFilter);
    state.prevInvalid       = _set_invalid_parameter_handler(&CrashOnInvalidParameter);
    state.prevPurecall      = _set_purecall_handler(&CrashOnPurecall);
    state.prevAbort         = signal(SIGABRT, &CrashOnAbort);
    /// @note _CALL_REPORTFAULT が立っていると abort() が SIGABRT より先に WER へ渡してしまう。
    state.prevAbortBehavior = _set_abort_behavior(0, _CALL_REPORTFAULT);

    ULONG guarantee = kCrashStackGuaranteeBytes;
    SetThreadStackGuarantee(&guarantee);

    state.installed = true;
    return true;
}

void CrashHandler::Uninstall()
{
    CrashState& state = GetCrashState();
    if (!state.installed) return;

    SetUnhandledExceptionFilter(state.prevFilter);
    _set_invalid_parameter_handler(state.prevInvalid);
    _set_purecall_handler(state.prevPurecall);
    signal(SIGABRT, state.prevAbort == SIG_ERR ? SIG_DFL : state.prevAbort);
    _set_abort_behavior(state.prevAbortBehavior, _CALL_REPORTFAULT);
    Logger::RemoveSink(&GetCrashLogTail());
    GetCrashLogTail().Clear();
    state.installed = false;
}

bool CrashHandler::IsInstalled()
{
    return GetCrashState().installed;
}

std::filesystem::path CrashHandler::WriteReport(std::string_view reason)
{
    if (!IsInstalled()) return {};

    CrashCapturedContext captured;
    captured.Capture();

    auto request = std::make_unique<CrashRequest>();
    request->exception = &captured.pointers;
    request->threadId  = GetCurrentThreadId();
    strncpy_s(request->reason, std::string(reason).c_str(), _TRUNCATE);
    /// @note 落ちていないのでローダーロックの心配は無い。request を先に捨てないよう書き終わるまで待つ。
    CrashRunWriter(*request, INFINITE);
    if (!request->written) return {};
    return std::filesystem::path(request->outDir);
}

std::vector<CrashRecord> CrashHandler::FindUnreported(const std::filesystem::path& root)
{
    std::vector<CrashRecord> records;
    std::error_code ec;
    const std::filesystem::path crashes = root / L"Saved" / L"Crashes";
    std::filesystem::directory_iterator it(crashes, ec);
    for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const std::filesystem::path& dir = it->path();
        std::error_code entryError;
        if (!std::filesystem::is_regular_file(dir / L"report.txt", entryError)) continue;
        if (std::filesystem::exists(dir / L"reported", entryError)) continue;
        records.push_back({ dir, CrashReadSummary(dir / L"report.txt") });
    }
    /// @note ディレクトリ名は時刻で始まるので、名前の降順が新しい順。
    std::sort(records.begin(), records.end(), [](const CrashRecord& a, const CrashRecord& b) {
        return a.directory.filename() > b.directory.filename();
    });
    return records;
}

void CrashHandler::MarkReported(const CrashRecord& record)
{
    std::ofstream marker(record.directory / L"reported");
}

const std::vector<std::string>& CrashHandler::TriggerNames()
{
    /// @note CrashTrigger の並びと同じ順。
    static const std::vector<std::string> names = {
        "access_violation", "stack_overflow", "abort", "pure_call", "invalid_parameter", "report",
    };
    return names;
}

bool CrashHandler::ParseTrigger(std::string_view name, CrashTrigger& out)
{
    const std::vector<std::string>& names = TriggerNames();
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) {
            out = static_cast<CrashTrigger>(i);
            return true;
        }
    }
    return false;
}

bool CrashHandler::Trigger(CrashTrigger kind)
{
    if (!DeveloperMode::IsEnabled() || !IsInstalled()) {
        FBZZ_LOG_WARN("CrashHandler: 開発者モードでないか未 Install のため、わざと落とすのを拒否しました");
        return false;
    }
    FBZZ_LOG_WARN("CrashHandler: わざと落とします (%s)", TriggerNames()[static_cast<size_t>(kind)].c_str());

    switch (kind) {
    case CrashTrigger::AccessViolation: {
        volatile int* target = nullptr;
        *target = 1;
        break;
    }
    case CrashTrigger::StackOverflow:
        CrashTriggerStackOverflow(0);
        break;
    case CrashTrigger::Abort:
        std::abort();
    case CrashTrigger::PureCall: {
        CrashPureCallDerived derived;
        break;
    }
    case CrashTrigger::InvalidParameter: {
        char tooShort[2] = {};
        strcpy_s(tooShort, sizeof(tooShort), "does not fit");
        break;
    }
    case CrashTrigger::Report:
        return !WriteReport("developer mode: report test").empty();
    }
    return false;
}

size_t CrashHandler::NotifyUnreported(const std::filesystem::path& root, std::string_view appName, bool interactive)
{
    const std::vector<CrashRecord> records = FindUnreported(root);
    if (records.empty()) return 0;

    const CrashRecord& latest = records.front();
    const std::string app = std::string(appName);
    const std::string dir = StringUtils::PathToUtf8(latest.directory);
    FBZZ_LOG_WARN("CrashHandler: 前回 %s が異常終了しました (%zu 件)。最新: %s (%s)",
                  app.c_str(), records.size(), latest.summary.c_str(), dir.c_str());
    if (!interactive) return records.size();

    std::string message = "前回の実行で " + app + " が異常終了しました。\n\n" + latest.summary + "\n\n記録の場所:\n" + dir;
    if (records.size() > 1)
        message += "\n(ほかに " + std::to_string(records.size() - 1) + " 件)";
    message += "\n\nフォルダを開きますか？";

    const int answer = MessageBoxW(nullptr, StringUtils::ToWide(message).c_str(), StringUtils::ToWide(app).c_str(),
                                   MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND);
    if (answer == IDYES)
        ShellExecuteW(nullptr, L"open", latest.directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    for (const CrashRecord& record : records)
        MarkReported(record);
    return records.size();
}

} // namespace fbzz::core
