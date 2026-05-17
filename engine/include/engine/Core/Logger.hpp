// FBZZ Engine
// Logger.hpp | fbzz::core
// ログレベル付きデバッグ出力 (Release では除去)
#pragma once
#include <cstdarg>

namespace fbzz::core {

enum class LogLevel { INFO = 0, WARNING = 1, LOG_ERROR = 2 };

class Logger {
public:
    static void Info (const char* fmt, ...);
    static void Warn (const char* fmt, ...);
    static void Error(const char* fmt, ...);

    static void SetMinLevel(LogLevel level);

private:
    static LogLevel s_minLevel;
    static void     Log(LogLevel level, const char* fmt, va_list args);
};

} // namespace fbzz::core

#ifdef NDEBUG
    #define FBZZ_LOG_INFO(fmt, ...)  ((void)0)
    #define FBZZ_LOG_WARN(fmt, ...)  ((void)0)
    #define FBZZ_LOG_ERROR(fmt, ...) ((void)0)
#else
    #define FBZZ_LOG_INFO(fmt, ...)  ::fbzz::core::Logger::Info ("[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
    #define FBZZ_LOG_WARN(fmt, ...)  ::fbzz::core::Logger::Warn ("[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
    #define FBZZ_LOG_ERROR(fmt, ...) ::fbzz::core::Logger::Error("[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#endif
