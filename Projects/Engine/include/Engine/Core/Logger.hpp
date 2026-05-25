// FBZZ Engine
// Logger.hpp | fbzz::core
// ファイル名と行番号を含むログ出力
// マクロ経由で呼ぶと呼び出し位置を LogEntry に残せる。
// ログ出力先は非所有参照として扱い、Logger は所有しない。
#pragma once
#include <cstdarg>
#include <cstring>
#include <vector>

namespace fbzz::core {

enum class LogLevel { INFO = 0, WARNING = 1, LOG_ERROR = 2 };

class ILogSink;

class Logger {
public:
    static void Info (const char* fmt, ...);
    static void Warn (const char* fmt, ...);
    static void Error(const char* fmt, ...);

    static void InfoAt (const char* file, int line, const char* fmt, ...);
    static void WarnAt (const char* file, int line, const char* fmt, ...);
    static void ErrorAt(const char* file, int line, const char* fmt, ...);

    static void SetMinLevel(LogLevel level);

    static void AddSink(ILogSink* sink);
    static void RemoveSink(ILogSink* sink);

private:
    static LogLevel s_minLevel;
    static std::vector<ILogSink*> s_sinks;
    static void Log(LogLevel level, const char* fmt, va_list args);
};

} // namespace fbzz::core

#define FBZZ_FILENAME (strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : __FILE__)

#ifdef NDEBUG
    #define FBZZ_LOG_INFO(...)  ((void)0)
    #define FBZZ_LOG_WARN(...)  ((void)0)
    #define FBZZ_LOG_ERROR(...) ((void)0)
#else
    #define FBZZ_LOG_INFO(...)  ::fbzz::core::Logger::InfoAt (FBZZ_FILENAME, __LINE__, __VA_ARGS__)
    #define FBZZ_LOG_WARN(...)  ::fbzz::core::Logger::WarnAt (FBZZ_FILENAME, __LINE__, __VA_ARGS__)
    #define FBZZ_LOG_ERROR(...) ::fbzz::core::Logger::ErrorAt(FBZZ_FILENAME, __LINE__, __VA_ARGS__)
#endif
