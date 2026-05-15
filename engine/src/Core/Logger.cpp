// FBZZ Engine
// Logger.cpp | fbzz::core
// ログ出力実装
#include "engine/Core/Logger.hpp"

#include <cstdio>
#include <Windows.h>

namespace fbzz::core {

LogLevel Logger::s_minLevel = LogLevel::INFO;

void Logger::SetMinLevel(LogLevel level) { s_minLevel = level; }

void Logger::Log(LogLevel level, const char* fmt, va_list args)
{
    if (level < s_minLevel) return;

    const char* prefix = nullptr;
    switch (level) {
    case LogLevel::INFO:      prefix = "[FBZZ INFO]  "; break;
    case LogLevel::WARNING:   prefix = "[FBZZ WARN]  "; break;
    case LogLevel::LOG_ERROR: prefix = "[FBZZ ERROR] "; break;
    }

    char body[1024];
    vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, args);

    char line[1200];
    snprintf(line, sizeof(line), "%s%s\n", prefix, body);

    // VS デバッグ出力 + stdout の両方に出す
    OutputDebugStringA(line);
    printf("%s", line);
}

void Logger::Info (const char* fmt, ...) { va_list a; va_start(a, fmt); Log(LogLevel::INFO,      fmt, a); va_end(a); }
void Logger::Warn (const char* fmt, ...) { va_list a; va_start(a, fmt); Log(LogLevel::WARNING,   fmt, a); va_end(a); }
void Logger::Error(const char* fmt, ...) { va_list a; va_start(a, fmt); Log(LogLevel::LOG_ERROR, fmt, a); va_end(a); }

} // namespace fbzz::core
