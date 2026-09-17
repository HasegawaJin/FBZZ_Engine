/// @file    Logger.hpp
/// @brief   ファイル名と行番号を含むログ出力。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// マクロ経由で呼ぶと呼び出し位置を LogEntry に残せる。
/// ログ出力先は非所有参照として扱い、Logger は所有しない。
#pragma once
#include <cstdarg>
#include <cstring>
#include <vector>

namespace fbzz::core {

/// @brief DEBUG < INFO < WARNING < LOG_ERROR の順で重要度が上がる。
/// @note SetMinLevel(INFO) がデフォルト。DEBUG はエディタの Console で個別に有効化する。
enum class LogLevel { DEBUG = 0, INFO = 1, WARNING = 2, LOG_ERROR = 3 };

class ILogSink;

class Logger {
public:
    static void Debug(const char* fmt, ...);
    static void Info (const char* fmt, ...);
    static void Warn (const char* fmt, ...);
    static void Error(const char* fmt, ...);

    static void DebugAt(const char* file, int line, const char* fmt, ...);
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

/// @note FBZZ_LOG_DEBUG は Release で無効 (パフォーマンスに敏感な詳細ログ)。INFO/WARN/ERROR は Debug・Release 共通で有効。リリースビルドでも重要なイベント・警告・エラーは確認できるようにし、DEBUG だけを開発専用とする。
#ifdef NDEBUG
    #define FBZZ_LOG_DEBUG(...) ((void)0)
#else
    #define FBZZ_LOG_DEBUG(...) ::fbzz::core::Logger::DebugAt(FBZZ_FILENAME, __LINE__, __VA_ARGS__)
#endif

#define FBZZ_LOG_INFO(...)  ::fbzz::core::Logger::InfoAt (FBZZ_FILENAME, __LINE__, __VA_ARGS__)
#define FBZZ_LOG_WARN(...)  ::fbzz::core::Logger::WarnAt (FBZZ_FILENAME, __LINE__, __VA_ARGS__)
#define FBZZ_LOG_ERROR(...) ::fbzz::core::Logger::ErrorAt(FBZZ_FILENAME, __LINE__, __VA_ARGS__)
