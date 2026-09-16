/// @file    LoggerTests.cpp
/// @brief   Logger のレベルフィルタと非所有 Sink 契約を自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// 出力«内容»の検証は記録用 fake、呼ばれ«方»の検証 (回数・順序・呼ばれないこと) は
/// gmock、と使い分けている。判断基準は Docs/conventions/test.md を参照。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/LogSinks.hpp>

#include <Engine/Core/ILogSink.hpp>
#include <Engine/Core/Logger.hpp>

#include <string>

namespace fbzz::tests {

using ::testing::_;
using ::testing::Field;
using ::testing::InSequence;
using ::testing::NiceMock;

class LoggerTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        // このテストだけは DEBUG まで通す。TearDown で既定へ戻る。
        SetLogLevel(core::LogLevel::DEBUG);
    }

    // WHY メンバーにするか: Logger は sink を所有しない。ローカル変数にすると、
    //     解除より先に破棄された場合に次のテストが破棄済みポインタを呼びに行く。
    testkit::RecordingLogSink sink;
};

// --- 配送内容 ---------------------------------------------------------------

TEST_F(LoggerTest, DeliversFormattedEntriesToRegisteredSink)
{
    testkit::ScopedLogSink scoped(&sink);

    core::Logger::Info("value=%d", 42);

    ASSERT_EQ(sink.entries.size(), 1u);
    EXPECT_EQ(sink.entries.front().level, core::LogLevel::INFO);
    EXPECT_NE(sink.entries.front().message.find("42"), std::string::npos);
}

TEST_F(LoggerTest, FiltersEntriesBelowTheMinimumLevel)
{
    testkit::ScopedLogSink scoped(&sink);
    SetLogLevel(core::LogLevel::WARNING);

    core::Logger::Debug("debug");
    core::Logger::Info("info");
    core::Logger::Warn("warning");
    core::Logger::Error("error");

    ASSERT_EQ(sink.entries.size(), 2u);
    EXPECT_EQ(sink.entries[0].level, core::LogLevel::WARNING);
    EXPECT_EQ(sink.entries[1].level, core::LogLevel::LOG_ERROR);
    EXPECT_EQ(sink.CountOf(core::LogLevel::INFO), 0u);
}

TEST_F(LoggerTest, RemoveSinkStopsFurtherDelivery)
{
    core::Logger::AddSink(&sink);
    core::Logger::Info("before");
    core::Logger::RemoveSink(&sink);
    core::Logger::Info("after");

    ASSERT_EQ(sink.entries.size(), 1u);
    EXPECT_NE(sink.Find("before"), nullptr);
    EXPECT_EQ(sink.Find("after"), nullptr);
}

TEST_F(LoggerTest, AtFunctionsKeepTheMessageUsable)
{
    testkit::ScopedLogSink scoped(&sink);

    core::Logger::WarnAt("LoggerTests.cpp", 42, "manual location");

    ASSERT_EQ(sink.entries.size(), 1u);
    EXPECT_EQ(sink.entries.front().level, core::LogLevel::WARNING);
    EXPECT_NE(sink.Find("manual location"), nullptr);
}

// --- 呼ばれ方の契約 (gmock) -------------------------------------------------

class LoggerCallTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        SetLogLevel(core::LogLevel::DEBUG);
    }

    testkit::MockLogSink mock;
};

TEST_F(LoggerCallTest, DoesNotCallTheSinkForEntriesBelowTheMinimumLevel)
{
    // «呼ばれないこと» が契約そのもの。記録用 fake で «0 件だった» を見るより、
    // 呼ばれた瞬間に落ちるほうが原因の行に近いところで止まる。
    testkit::ScopedLogSink scoped(&mock);
    SetLogLevel(core::LogLevel::LOG_ERROR);

    EXPECT_CALL(mock, OnLog(_)).Times(0);

    core::Logger::Debug("dropped");
    core::Logger::Info("dropped");
    core::Logger::Warn("dropped");
}

TEST_F(LoggerCallTest, DeliversEachEntryToTheSinkExactlyOnce)
{
    testkit::ScopedLogSink scoped(&mock);

    EXPECT_CALL(mock, OnLog(Field(&core::LogEntry::level, core::LogLevel::WARNING))).Times(1);

    core::Logger::Warn("once");
}

TEST_F(LoggerCallTest, DeliversEntriesInTheOrderTheyWereLogged)
{
    testkit::ScopedLogSink scoped(&mock);

    InSequence ordered;
    EXPECT_CALL(mock, OnLog(Field(&core::LogEntry::level, core::LogLevel::INFO)));
    EXPECT_CALL(mock, OnLog(Field(&core::LogEntry::level, core::LogLevel::WARNING)));
    EXPECT_CALL(mock, OnLog(Field(&core::LogEntry::level, core::LogLevel::LOG_ERROR)));

    core::Logger::Info("first");
    core::Logger::Warn("second");
    core::Logger::Error("third");
}

TEST_F(LoggerCallTest, DeliversTheSameEntryToEveryRegisteredSink)
{
    // 2 つ目の sink は «来ても来なくてもよい» わけではないが、
    // ここで見たいのは «両方に届く» ことだけなので NiceMock で余計な呼び出しを許す。
    NiceMock<testkit::MockLogSink> second;

    testkit::ScopedLogSink scopedFirst(&mock);
    testkit::ScopedLogSink scopedSecond(&second);

    EXPECT_CALL(mock, OnLog(_)).Times(1);
    EXPECT_CALL(second, OnLog(_)).Times(1);

    core::Logger::Error("broadcast");
}

} // namespace fbzz::tests
