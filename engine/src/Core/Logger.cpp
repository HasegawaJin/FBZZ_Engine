// FBZZ Engine
// Logger.cpp | fbzz::core
// ログ出力実装
#include "engine/Core/Logger.hpp"
#include "engine/Core/ILogSink.hpp"

#include <cstdio>
#include <Windows.h>
#include <algorithm>

namespace fbzz::core {

LogLevel             Logger::s_minLevel = LogLevel::INFO;
std::vector<ILogSink*> Logger::s_sinks;

void Logger::SetMinLevel(LogLevel level) { s_minLevel = level; }

void Logger::Log(LogLevel level, const char* fmt, va_list args)
{
    if (level < s_minLevel) return;

    // UTF-8 ソースファイルの日本語文字列をコンソールで正しく表示するため
    // 初回呼び出し時に一度だけコードページを UTF-8 に切り替える。
    // CP932 (Shift-JIS) のままだと printf が文字化けする。
    static bool s_cpSet = (SetConsoleOutputCP(CP_UTF8), true);

    const char* prefix = nullptr;
    switch (level) {
    case LogLevel::INFO:      prefix = "[INFO]  "; break;
    case LogLevel::WARNING:   prefix = "[WARNING]  "; break;
    case LogLevel::LOG_ERROR: prefix = "[ERROR] "; break;
    }

    char body[1024];
    vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, args);

    char line[1200];
    snprintf(line, sizeof(line), "%s%s\n", prefix, body);

    // VS デバッグ出力 + stdout の両方に出す
    OutputDebugStringA(line);
    printf("%s", line);

    // 登録済みシンクに配信
    LogEntry entry{ level, body };
    for (ILogSink* sink : s_sinks)
        sink->OnLog(entry);
}

void Logger::AddSink(ILogSink* sink)
{
    if (sink) s_sinks.push_back(sink);
}

void Logger::RemoveSink(ILogSink* sink)
{
    auto it = std::find(s_sinks.begin(), s_sinks.end(), sink);
    if (it != s_sinks.end()) s_sinks.erase(it);
}

void Logger::Info (const char* fmt, ...) { va_list a; va_start(a, fmt); Log(LogLevel::INFO,      fmt, a); va_end(a); }
void Logger::Warn (const char* fmt, ...) { va_list a; va_start(a, fmt); Log(LogLevel::WARNING,   fmt, a); va_end(a); }
void Logger::Error(const char* fmt, ...) { va_list a; va_start(a, fmt); Log(LogLevel::LOG_ERROR, fmt, a); va_end(a); }

} // namespace fbzz::core
