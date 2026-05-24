// FBZZ Engine
// Logger.cpp | fbzz::core
// Log output implementation
#include "Engine/Core/Logger.hpp"
#include "Engine/Core/ILogSink.hpp"

#include <Windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>

namespace fbzz::core {

LogLevel Logger::s_minLevel = LogLevel::INFO;
std::vector<ILogSink*> Logger::s_sinks;

namespace {

void BuildLocatedFormat(char* out, size_t outSize, const char* file, int line, const char* fmt)
{
    snprintf(out, outSize, "[%s:%d] %s", file ? file : "unknown", line, fmt ? fmt : "");
}

} // namespace

void Logger::SetMinLevel(LogLevel level) { s_minLevel = level; }

void Logger::Log(LogLevel level, const char* fmt, va_list args)
{
    if (level < s_minLevel) return;

    static bool s_cpSet = (SetConsoleOutputCP(CP_UTF8), true);
    (void)s_cpSet;

    const char* prefix = nullptr;
    switch (level) {
    case LogLevel::INFO:      prefix = "[INFO]  "; break;
    case LogLevel::WARNING:   prefix = "[WARNING]  "; break;
    case LogLevel::LOG_ERROR: prefix = "[ERROR] "; break;
    }

    char body[1024] = {};
    const int written = vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, args);
    if (written < 0)
        snprintf(body, sizeof(body), "[Logger format error]");

    char line[1200] = {};
    snprintf(line, sizeof(line), "%s%s\n", prefix, body);

    OutputDebugStringA(line);
    printf("%s", line);

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
