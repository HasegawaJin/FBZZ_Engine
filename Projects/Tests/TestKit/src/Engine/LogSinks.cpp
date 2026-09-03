/// @file    LogSinks.cpp
/// @brief   テスト用 Logger sink の実装。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <TestKit/Engine/LogSinks.hpp>

namespace fbzz::testkit {

const core::LogEntry* RecordingLogSink::Find(const std::string& needle) const
{
    for (const core::LogEntry& entry : entries) {
        if (entry.message.find(needle) != std::string::npos) return &entry;
    }
    return nullptr;
}

std::size_t RecordingLogSink::CountOf(core::LogLevel level) const
{
    std::size_t count = 0;
    for (const core::LogEntry& entry : entries) {
        if (entry.level == level) ++count;
    }
    return count;
}

} // namespace fbzz::testkit
