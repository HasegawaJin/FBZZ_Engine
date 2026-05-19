// FBZZ Engine
// Logger.hpp | fbzz::core
// ログレベル付きデバッグ出力 (Release では除去)
#pragma once
#include <cstdarg>
#include <vector>

namespace fbzz::core {

enum class LogLevel { INFO = 0, WARNING = 1, LOG_ERROR = 2 };

class ILogSink;  // 前方宣言

class Logger {
public:
    static void Info (const char* fmt, ...);
    static void Warn (const char* fmt, ...);
    static void Error(const char* fmt, ...);

    static void SetMinLevel(LogLevel level);

    // シンク登録 (非所有。呼び出し元がライフタイムを管理する)
    static void AddSink(ILogSink* sink);
    static void RemoveSink(ILogSink* sink);

private:
    static LogLevel            s_minLevel;
    static std::vector<ILogSink*> s_sinks;
    static void Log(LogLevel level, const char* fmt, va_list args);
};

} // namespace fbzz::core

#define FBZZ_FILENAME (strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : __FILE__)

#ifdef NDEBUG
    #define FBZZ_LOG_INFO(fmt, ...)  ((void)0)
    #define FBZZ_LOG_WARN(fmt, ...)  ((void)0)
    #define FBZZ_LOG_ERROR(fmt, ...) ((void)0)
#else
    #define FBZZ_LOG_INFO(fmt, ...)  ::fbzz::core::Logger::Info ("[%s:%d] " fmt, FBZZ_FILENAME, __LINE__, ##__VA_ARGS__)
    #define FBZZ_LOG_WARN(fmt, ...)  ::fbzz::core::Logger::Warn ("[%s:%d] " fmt, FBZZ_FILENAME, __LINE__, ##__VA_ARGS__)
    #define FBZZ_LOG_ERROR(fmt, ...) ::fbzz::core::Logger::Error("[%s:%d] " fmt, FBZZ_FILENAME, __LINE__, ##__VA_ARGS__)
#endif
