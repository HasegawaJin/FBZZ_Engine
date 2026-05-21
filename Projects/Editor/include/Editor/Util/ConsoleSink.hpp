// FBZZ Engine
// ConsoleSink.hpp | fbzz::editor
// Logger のログエントリをリングバッファに蓄積し ConsolePanel に渡す
#pragma once
#include <Engine/Core/ILogSink.hpp>
#include <deque>

namespace fbzz::editor {

class ConsoleSink final : public core::ILogSink {
public:
    static constexpr size_t MAX_ENTRIES = 512;

    void OnLog(const core::LogEntry& entry) override;

    const std::deque<core::LogEntry>& GetEntries() const { return m_entries; }
    void Clear() { m_entries.clear(); }

private:
    std::deque<core::LogEntry> m_entries;
};

} // namespace fbzz::editor
