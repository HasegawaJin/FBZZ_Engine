/// @file    Logger.cpp
/// @brief   Logger の出力処理実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note ログレベルでフィルタし、登録済み ILogSink へ LogEntry を配信する。
/// @note 出力先は非所有ポインタとして扱い、寿命管理は登録側が行う。
#include "Core/Logger.hpp"
#include "Core/ILogSink.hpp"

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
/// @note ログはワーカースレッド (流体プレビュー・アセット読み込み) からも出るため、走査中の
/// @note       AddSink/RemoveSink による vector 変更や複数シンクの同時書き込みから保護する。
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
    if (!fmt) return {};

    /// @note MSVC の vsnprintf_s(..., _TRUNCATE, ...) は切り詰め時も -1 を返すため、必要サイズを
    /// @note       先に計算して完全なログ本文を確保する (切り詰めるとビルドログの診断情報が失われる)。
    va_list countArgs;
    va_copy(countArgs, args);
    const int required = _vscprintf(fmt, countArgs);
    va_end(countArgs);

    if (required < 0)
        return "[Logger format error]";

    std::string message(static_cast<size_t>(required) + 1, '\0');
    va_list writeArgs;
    va_copy(writeArgs, args);
    vsnprintf_s(message.data(), message.size(), _TRUNCATE, fmt, writeArgs);
    va_end(writeArgs);
    message.resize(static_cast<size_t>(required));
    return message;
}

} /// @note namespace

void Logger::SetMinLevel(LogLevel level) { s_minLevel = level; }

void Logger::Log(LogLevel level, const char* fmt, va_list args)
{
    if (level < s_minLevel) return;

    static bool s_cpSet = (SetConsoleOutputCP(CP_UTF8), true);
    (void)s_cpSet;

    const char* prefix = nullptr;
    switch (level) {
    case LogLevel::DEBUG:     prefix = "[DEBUG] "; break;
    case LogLevel::INFO:      prefix = "[INFO]  "; break;
    case LogLevel::WARNING:   prefix = "[WARN]  "; break;
    case LogLevel::LOG_ERROR: prefix = "[ERROR] "; break;
    }

    const std::string body = FormatLogMessage(fmt, args);

    /// @note 書式化はロックの外で済ませてある (シンクを待たせる時間を短くする)。
    const std::lock_guard lock(LogMutex());
    std::string line = prefix;
    line += body;
    line += '\n';

    /// @note デバッガ不在時は DBWIN の外部受信側に待たされず、標準出力だけへ送る。
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/debugapi/nf-debugapi-isdebuggerpresent IsDebuggerPresent
    if (IsDebuggerPresent())
        OutputDebugStringA(line.c_str());
    printf("%s", line.c_str());

    LogEntry entry{ level, body };
    for (ILogSink* sink : s_sinks)
        sink->OnLog(entry);
}

void Logger::AddSink(ILogSink* sink)
{
    const std::lock_guard lock(LogMutex());
    if (sink) s_sinks.push_back(sink);
}

void Logger::RemoveSink(ILogSink* sink)
{
    const std::lock_guard lock(LogMutex());
    auto it = std::find(s_sinks.begin(), s_sinks.end(), sink);
    if (it != s_sinks.end()) s_sinks.erase(it);
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

} /// @note namespace fbzz::core
