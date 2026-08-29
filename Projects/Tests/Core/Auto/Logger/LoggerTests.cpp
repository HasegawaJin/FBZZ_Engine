/// @file    LoggerTests.cpp
/// @brief   Logger のレベルフィルタと非所有 Sink 契約を自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <gtest/gtest.h>

#include <Engine/Core/ILogSink.hpp>
#include <Engine/Core/Logger.hpp>

#include <string>
#include <vector>

namespace fbzz::tests {

class RecordingSink final : public core::ILogSink {
public:
    void OnLog(const core::LogEntry& entry) override { entries.push_back(entry); }
    std::vector<core::LogEntry> entries;
};

class LoggerTest : public ::testing::Test {
protected:
    void SetUp() override { core::Logger::SetMinLevel(core::LogLevel::DEBUG); }

    void TearDown() override
    {
        core::Logger::RemoveSink(&sink);
        core::Logger::SetMinLevel(core::LogLevel::INFO);
    }

    RecordingSink sink;
};

TEST_F(LoggerTest, DeliversFormattedEntriesToRegisteredSink)
{
    core::Logger::AddSink(&sink);
    core::Logger::Info("value=%d", 42);

    ASSERT_EQ(sink.entries.size(), 1u);
    EXPECT_EQ(sink.entries.front().level, core::LogLevel::INFO);
    EXPECT_NE(sink.entries.front().message.find("42"), std::string::npos);
}

TEST_F(LoggerTest, FiltersEntriesBelowTheMinimumLevel)
{
    core::Logger::AddSink(&sink);
    core::Logger::SetMinLevel(core::LogLevel::WARNING);

    core::Logger::Debug("debug");
    core::Logger::Info("info");
    core::Logger::Warn("warning");
    core::Logger::Error("error");

    ASSERT_EQ(sink.entries.size(), 2u);
    EXPECT_EQ(sink.entries[0].level, core::LogLevel::WARNING);
    EXPECT_EQ(sink.entries[1].level, core::LogLevel::LOG_ERROR);
}

TEST_F(LoggerTest, RemoveSinkStopsFurtherDelivery)
{
    core::Logger::AddSink(&sink);
    core::Logger::Info("before");
    core::Logger::RemoveSink(&sink);
    core::Logger::Info("after");

    ASSERT_EQ(sink.entries.size(), 1u);
    EXPECT_NE(sink.entries.front().message.find("before"), std::string::npos);
}

TEST_F(LoggerTest, AtFunctionsKeepTheMessageUsable)
{
    core::Logger::AddSink(&sink);
    core::Logger::WarnAt("LoggerTests.cpp", 42, "manual location");

    ASSERT_EQ(sink.entries.size(), 1u);
    EXPECT_EQ(sink.entries.front().level, core::LogLevel::WARNING);
    EXPECT_NE(sink.entries.front().message.find("manual location"), std::string::npos);
}

} // namespace fbzz::tests
