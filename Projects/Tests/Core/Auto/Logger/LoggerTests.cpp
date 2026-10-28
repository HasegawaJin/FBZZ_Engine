/// @file    LoggerTests.cpp
/// @brief   Logger のレベルフィルタと非所有 Sink 契約を自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
/// @see Docs/conventions/test.md 記録用 fake と配送回数を検証する mock の使い分け。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/LogSinks.hpp>

#include <Engine/Core/ILogSink.hpp>
#include <Engine/Core/Logger.hpp>
#include <Core/ExternalCallbackBoundary.hpp>

#include <string>
#include <thread>

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
        /// @note このテストだけは DEBUG まで通す。TearDown で既定へ戻る。
        SetLogLevel(core::LogLevel::DEBUG);
    }

    /// @note Logger は sink を所有しない。ローカル変数にすると解除より先に破棄され、次のテストが破棄済みポインタを呼びに行く。
    testkit::RecordingLogSink sink;
};

/// @name 配送内容

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

/// @name 呼ばれ方の契約 (gmock)

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
    /// @note 配送しない契約は呼ばれた時点で検出する。
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
    /// @note 配送以外の mock 呼び出しはこの契約の対象外。
    NiceMock<testkit::MockLogSink> second;

    testkit::ScopedLogSink scopedFirst(&mock);
    testkit::ScopedLogSink scopedSecond(&second);

    EXPECT_CALL(mock, OnLog(_)).Times(1);
    EXPECT_CALL(second, OnLog(_)).Times(1);

    core::Logger::Error("broadcast");
}

TEST_F(LoggerCallTest, CompletesOwnerThreadObserversBeforeSinkDelivery)
{
    testkit::ScopedLogSink registered(&mock);
    std::vector<int> order;
    auto append = [](void* context) {
        static_cast<std::vector<int>*>(context)->push_back(1);
        return true;
    };
    core::ExternalCallbackScope outer(append, &order);
    auto complete = [](void* context) {
        static_cast<std::vector<int>*>(context)->push_back(2);
        return false;
    };
    {
        core::ExternalCallbackScope inner(complete, &order);
        EXPECT_CALL(mock, OnLog(_)).WillOnce([&](const core::LogEntry&) {
            EXPECT_EQ(order, (std::vector<int>{1, 2}));
        });
        core::Logger::Info("completed or invalidated before callback");
    }
    order.clear();
    EXPECT_CALL(mock, OnLog(_)).WillOnce([&](const core::LogEntry&) {
        EXPECT_EQ(order, (std::vector<int>{1}));
    });
    core::Logger::Info("outer scope remains active");
}

TEST_F(LoggerCallTest, FilteredEntriesDoNotCompletePendingWork)
{
    testkit::ScopedLogSink registered(&mock);
    int observations = 0;
    core::ExternalCallbackScope scope([](void* context) {
        ++*static_cast<int*>(context);
        return true;
    }, &observations);
    SetLogLevel(core::LogLevel::LOG_ERROR);
    EXPECT_CALL(mock, OnLog(_)).Times(0);
    core::Logger::Info("filtered");
    EXPECT_EQ(observations, 0);
}

TEST_F(LoggerCallTest, ObserverScopesStayOnTheirOwnerThread)
{
    int ownerObservations = 0;
    core::ExternalCallbackScope owner([](void* context) {
        ++*static_cast<int*>(context);
        return true;
    }, &ownerObservations);
    bool workerSucceeded = false;
    int workerObservations = 0;
    std::thread worker([&] {
        core::ExternalCallbackScope scope([](void* context) {
            ++*static_cast<int*>(context);
            return false;
        }, &workerObservations);
        workerSucceeded = core::NotifyBeforeExternalCallback();
    });
    worker.join();
    EXPECT_FALSE(workerSucceeded);
    EXPECT_EQ(workerObservations, 1);
    EXPECT_EQ(ownerObservations, 0);
    EXPECT_TRUE(core::NotifyBeforeExternalCallback());
    EXPECT_EQ(ownerObservations, 1);
}

} /// @note namespace fbzz::tests
