/// @file    Logger.cpp
/// @brief   Logger の出力処理実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note ログレベルでフィルタし、登録済み ILogSink へ LogEntry を配信する。
/// @note 出力先は非所有ポインタとして扱い、寿命管理は登録側が行う。
#include "Core/Logger/Logger.hpp"
#include "Core/Logger/ILogSink.hpp"
#include <Core/Util/ExternalCallbackBoundary.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>

namespace fbzz::core {

/// @note デフォルトは INFO。DEBUG は ConsolePanel の DEBUG チェックボックスで有効化する。
LogLevel Logger::s_minLevel = LogLevel::INFO;
std::vector<ILogSink*> Logger::s_sinks;

namespace {

/// @brief シンクの一覧と 1 行の出力を守る再帰ミューテックス。
/// @note ワーカーからの出力中にシンクの登録・解除や別の出力が重なるため、一覧の走査から配信完了までを保護する。
/// @note シンク内から再度ログを出しても、再帰可能なので自分を待って固まらない。
std::recursive_mutex& LogMutex()
{
    static std::recursive_mutex mutex;
    return mutex;
}

void BuildLocatedFormat(char* out, size_t outSize, const char* file, int line, const char* fmt)
{
    snprintf(out, outSize, "[%s:%d] %s", file ? file : "unknown", line, fmt ? fmt : "");
}

std::string FormatLogMessage(const char* fmt, va_list args)
{
    if (!fmt) {
        return {};
    }

    // MSVC の vsnprintf_s は切り詰め時も -1 を返す。先に必要な長さを求め、診断に使うログの全文を確保する。
    va_list countArgs;
    va_copy(countArgs, args);
    const int required = _vscprintf(fmt, countArgs);
    va_end(countArgs);

    if (required < 0) {
        return "[Logger format error]";
    }

    std::string message(static_cast<size_t>(required) + 1, '\0');
    va_list writeArgs;
    va_copy(writeArgs, args);
    vsnprintf_s(message.data(), message.size(), _TRUNCATE, fmt, writeArgs);
    va_end(writeArgs);
    message.resize(static_cast<size_t>(required));
    return message;
}

} // namespace

void Logger::SetMinLevel(LogLevel level) { s_minLevel = level; }

void Logger::Log(LogLevel level, const char* fmt, va_list args)
{
    if (level < s_minLevel) {
        return;
    }

    // 通知前の確定処理が描画ワーカーを待つことがあるため、ワーカーも使うログのロックを取る前に通知する。
    (void)NotifyBeforeExternalCallback();

    static bool s_cpSet = (SetConsoleOutputCP(CP_UTF8), true);
    (void)s_cpSet;

    const char* prefix = nullptr;
    switch (level) {
    case LogLevel::DEBUG:
        prefix = "[DEBUG] ";
        break;
    case LogLevel::INFO:
        prefix = "[INFO]  ";
        break;
    case LogLevel::WARNING:
        prefix = "[WARN]  ";
        break;
    case LogLevel::LOG_ERROR:
        prefix = "[ERROR] ";
        break;
    }

    const std::string body = FormatLogMessage(fmt, args);

    // 書式化はロックの外で済ませてある (シンクを待たせる時間を短くする)。
    const std::lock_guard lock(LogMutex());
    std::string line = prefix;
    line += body;
    line += '\n';

    // デバッガ不在時は DBWIN の外部受信側に待たされず、標準出力だけへ送る。
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/debugapi/nf-debugapi-isdebuggerpresent IsDebuggerPresent によるデバッガ接続の判定。
    if (IsDebuggerPresent()) {
        OutputDebugStringA(line.c_str());
    }
    printf("%s", line.c_str());

    LogEntry entry{ level, body };
    for (ILogSink* sink : s_sinks) {
        sink->OnLog(entry);
    }
}

void Logger::AddSink(ILogSink* sink)
{
    const std::lock_guard lock(LogMutex());
    if (sink) {
        s_sinks.push_back(sink);
    }
}

void Logger::RemoveSink(ILogSink* sink)
{
    const std::lock_guard lock(LogMutex());
    const auto registeredSink = std::find(s_sinks.begin(), s_sinks.end(), sink);
    if (registeredSink != s_sinks.end()) {
        s_sinks.erase(registeredSink);
    }
}

void Logger::Debug(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Log(LogLevel::DEBUG, fmt, args);
    va_end(args);
}

void Logger::Info(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Log(LogLevel::INFO, fmt, args);
    va_end(args);
}

void Logger::Warn(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Log(LogLevel::WARNING, fmt, args);
    va_end(args);
}

void Logger::Error(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Log(LogLevel::LOG_ERROR, fmt, args);
    va_end(args);
}

void Logger::DebugAt(const char* file, int line, const char* fmt, ...)
{
    char locatedFmt[1200] = {};
    BuildLocatedFormat(locatedFmt, sizeof(locatedFmt), file, line, fmt);

    va_list args;
    va_start(args, fmt);
    Log(LogLevel::DEBUG, locatedFmt, args);
    va_end(args);
}

void Logger::InfoAt(const char* file, int line, const char* fmt, ...)
{
    char locatedFmt[1200] = {};
    BuildLocatedFormat(locatedFmt, sizeof(locatedFmt), file, line, fmt);

    va_list args;
    va_start(args, fmt);
    Log(LogLevel::INFO, locatedFmt, args);
    va_end(args);
}

void Logger::WarnAt(const char* file, int line, const char* fmt, ...)
{
    char locatedFmt[1200] = {};
    BuildLocatedFormat(locatedFmt, sizeof(locatedFmt), file, line, fmt);

    va_list args;
    va_start(args, fmt);
    Log(LogLevel::WARNING, locatedFmt, args);
    va_end(args);
}

void Logger::ErrorAt(const char* file, int line, const char* fmt, ...)
{
    char locatedFmt[1200] = {};
    BuildLocatedFormat(locatedFmt, sizeof(locatedFmt), file, line, fmt);

    va_list args;
    va_start(args, fmt);
    Log(LogLevel::LOG_ERROR, locatedFmt, args);
    va_end(args);
}

} // namespace fbzz::core
