// FBZZ Engine
// Tests/Logger/main.cpp
// Logger モジュール単体テスト
// ILogSink のテスト実装をメモリバッファに向け、フィルタリング・複数シンク・DebugAt を検証する。
#include <cstdio>
#include <vector>
#include <string>

#include <Engine/Core/Logger.hpp>
#include <Engine/Core/ILogSink.hpp>

#include "../TestHelper.hpp"

using namespace fbzz::core;

// ─── テスト用シンク ────────────────────────────────────────────────────────────

// ログエントリをメモリに蓄積するシンク
class TestLogSink : public ILogSink {
public:
    std::vector<LogEntry> entries;
    void OnLog(const LogEntry& entry) override { entries.push_back(entry); }
    void Clear() { entries.clear(); }
    int Count(LogLevel level) const {
        int n = 0;
        for (const auto& e : entries) if (e.level == level) ++n;
        return n;
    }
};

// ─── 基本 Pub/Sub ─────────────────────────────────────────────────────────────

static void TestLogger_BasicSink()
{
    std::printf("\n=== Logger: Basic Sink ===\n");

    TestLogSink sink;
    Logger::SetMinLevel(LogLevel::DEBUG);
    Logger::AddSink(&sink);

    Logger::Info("hello %s", "world");
    check(sink.entries.size() == 1, "Logger: AddSink receives log entry");
    check(sink.entries[0].level == LogLevel::INFO,
          "Logger: Info() sets level to INFO");
    check(!sink.entries[0].message.empty(),
          "Logger: log message is non-empty");

    Logger::RemoveSink(&sink);
    sink.Clear();

    Logger::Info("after remove");
    check(sink.entries.empty(), "Logger: RemoveSink stops delivery");
}

// ─── 全レベル ──────────────────────────────────────────────────────────────────

static void TestLogger_AllLevels()
{
    std::printf("\n=== Logger: All Levels ===\n");

    TestLogSink sink;
    Logger::SetMinLevel(LogLevel::DEBUG);
    Logger::AddSink(&sink);

    Logger::Debug("debug msg");
    Logger::Info ("info msg");
    Logger::Warn ("warn msg");
    Logger::Error("error msg");

    checkF(static_cast<float>(sink.Count(LogLevel::DEBUG)),
           "Logger: Debug() delivered",
           static_cast<float>(sink.Count(LogLevel::DEBUG)), "== 1");
    checkF(static_cast<float>(sink.Count(LogLevel::INFO)),
           "Logger: Info() delivered",
           static_cast<float>(sink.Count(LogLevel::INFO)), "== 1");
    checkF(static_cast<float>(sink.Count(LogLevel::WARNING)),
           "Logger: Warn() delivered",
           static_cast<float>(sink.Count(LogLevel::WARNING)), "== 1");
    checkF(static_cast<float>(sink.Count(LogLevel::LOG_ERROR)),
           "Logger: Error() delivered",
           static_cast<float>(sink.Count(LogLevel::LOG_ERROR)), "== 1");

    Logger::RemoveSink(&sink);
}

// ─── SetMinLevel フィルタリング ────────────────────────────────────────────────

static void TestLogger_MinLevelFilter()
{
    std::printf("\n=== Logger: SetMinLevel ===\n");

    TestLogSink sink;
    Logger::AddSink(&sink);

    // WARNING 以上のみ通す
    Logger::SetMinLevel(LogLevel::WARNING);
    Logger::Debug("suppressed debug");
    Logger::Info ("suppressed info");
    Logger::Warn ("passed warn");
    Logger::Error("passed error");

    check(sink.Count(LogLevel::DEBUG)     == 0, "Logger: Debug suppressed by MinLevel=WARNING");
    check(sink.Count(LogLevel::INFO)      == 0, "Logger: Info suppressed by MinLevel=WARNING");
    check(sink.Count(LogLevel::WARNING)   == 1, "Logger: Warn passes MinLevel=WARNING");
    check(sink.Count(LogLevel::LOG_ERROR) == 1, "Logger: Error passes MinLevel=WARNING");

    sink.Clear();

    // LOG_ERROR のみ通す
    Logger::SetMinLevel(LogLevel::LOG_ERROR);
    Logger::Warn ("suppressed warn");
    Logger::Error("passed error");

    check(sink.Count(LogLevel::WARNING)   == 0, "Logger: Warn suppressed by MinLevel=ERROR");
    check(sink.Count(LogLevel::LOG_ERROR) == 1, "Logger: Error passes MinLevel=ERROR");

    Logger::RemoveSink(&sink);
    Logger::SetMinLevel(LogLevel::INFO);
}

// ─── 複数シンク ────────────────────────────────────────────────────────────────

static void TestLogger_MultipleSinks()
{
    std::printf("\n=== Logger: Multiple Sinks ===\n");

    TestLogSink sinkA, sinkB;
    Logger::SetMinLevel(LogLevel::DEBUG);
    Logger::AddSink(&sinkA);
    Logger::AddSink(&sinkB);

    Logger::Info("broadcast");
    check(sinkA.entries.size() == 1, "Logger: sinkA receives broadcast");
    check(sinkB.entries.size() == 1, "Logger: sinkB receives broadcast");

    // sinkA だけ削除 → sinkB だけ受け取る
    Logger::RemoveSink(&sinkA);
    Logger::Info("only B");
    check(sinkA.entries.size() == 1, "Logger: removed sinkA does not receive further logs");
    check(sinkB.entries.size() == 2, "Logger: sinkB receives second log");

    Logger::RemoveSink(&sinkB);
}

// ─── DebugAt / InfoAt — ファイルと行番号を含む ────────────────────────────────

static void TestLogger_AtMethods()
{
    std::printf("\n=== Logger: *At methods (file + line) ===\n");

    TestLogSink sink;
    Logger::SetMinLevel(LogLevel::DEBUG);
    Logger::AddSink(&sink);

    Logger::InfoAt("TestFile.cpp", 42, "line %d", 42);
    check(sink.entries.size() == 1, "Logger: InfoAt delivers entry");

    // メッセージにファイル名か行番号の文字列が含まれるか
    const std::string& msg = sink.entries[0].message;
    check(!msg.empty(), "Logger: InfoAt message is non-empty");

    sink.Clear();
    Logger::WarnAt("AnotherFile.cpp", 99, "warning from line 99");
    check(sink.Count(LogLevel::WARNING) == 1, "Logger: WarnAt level is WARNING");

    Logger::RemoveSink(&sink);
    Logger::SetMinLevel(LogLevel::INFO);
}

// ─── フォーマット文字列 ────────────────────────────────────────────────────────

static void TestLogger_FormatString()
{
    std::printf("\n=== Logger: Format String ===\n");

    TestLogSink sink;
    Logger::SetMinLevel(LogLevel::INFO);
    Logger::AddSink(&sink);

    Logger::Info("value: %d, float: %.2f, str: %s", 7, 3.14f, "abc");
    check(sink.entries.size() == 1, "Logger: formatted message delivered");
    check(!sink.entries[0].message.empty(), "Logger: formatted message is non-empty");

    Logger::RemoveSink(&sink);
}

// ─── エントリポイント ─────────────────────────────────────────────────────────

int main()
{
    std::printf("FBZZ Logger Tests\n");
    std::printf("=================\n");

    TestLogger_BasicSink();
    TestLogger_AllLevels();
    TestLogger_MinLevelFilter();
    TestLogger_MultipleSinks();
    TestLogger_AtMethods();
    TestLogger_FormatString();

    std::printf("\n=================\n");
    std::printf("Results: %d passed, %d failed\n", g_passed, g_failed);

    return g_failed == 0 ? 0 : 1;
}
