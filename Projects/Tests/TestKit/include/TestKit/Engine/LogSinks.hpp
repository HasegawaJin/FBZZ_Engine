/// @file    LogSinks.hpp
/// @brief   Logger のテスト用 sink。記録用 fake と gmock 版。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// 使い分けは Docs/conventions/test.md の «GoogleMock を使う基準» に従う。
///   - 出力の内容を後から検査したい  -> RecordingLogSink
///   - 呼ばれる回数 / 順序 / 呼ばれないことが契約 -> MockLogSink
#pragma once

#include <Engine/Core/ILogSink.hpp>
#include <Engine/Core/Logger.hpp>

#include <gmock/gmock.h>

#include <string>
#include <vector>

namespace fbzz::testkit {

/// 受け取ったログを溜めるだけの sink。
class RecordingLogSink final : public core::ILogSink {
public:
    void OnLog(const core::LogEntry& entry) override { entries.push_back(entry); }

    /// message に needle を含む最初のエントリ。無ければ nullptr。
    const core::LogEntry* Find(const std::string& needle) const;

    /// level のエントリ数。
    std::size_t CountOf(core::LogLevel level) const;

    void Clear() { entries.clear(); }

    std::vector<core::LogEntry> entries;
};

/// 呼ばれ方そのものを検証する sink。
/// Logger は sink を所有しないため、fixture のメンバとして Logger より長生きさせること。
class MockLogSink : public core::ILogSink {
public:
    MOCK_METHOD(void, OnLog, (const core::LogEntry& entry), (override));
};

/// @brief Logger への登録と解除を対にする。テストが途中で ASSERT で抜けても解除される。
/// @note Logger は非所有ポインタを保持するので、解除し忘れると次のテストで破棄済みオブジェクトを呼び原因不明のクラッシュになる。
class ScopedLogSink {
public:
    explicit ScopedLogSink(core::ILogSink* sink) : m_sink(sink) { core::Logger::AddSink(m_sink); }
    ~ScopedLogSink() { core::Logger::RemoveSink(m_sink); }

    ScopedLogSink(const ScopedLogSink&)            = delete;
    ScopedLogSink& operator=(const ScopedLogSink&) = delete;

private:
    core::ILogSink* m_sink;
};

} // namespace fbzz::testkit
