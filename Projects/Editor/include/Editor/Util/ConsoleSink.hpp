// FBZZ Engine
// ConsoleSink.hpp | fbzz::editor
// Logger のログエントリをリングバッファに蓄積し ConsolePanel に渡す
#pragma once
#include <Engine/Core/ILogSink.hpp>
#include <cstdint>
#include <deque>

namespace fbzz::editor {

class ConsoleSink final : public core::ILogSink {
public:
    static constexpr size_t MAX_ENTRIES = 512;

    void OnLog(const core::LogEntry& entry) override;

    const std::deque<core::LogEntry>& GetEntries() const { return m_entries; }
    // ログ内容をコピーせず、MCP等の差分購読に使える単調増加カーソルを返す。
    std::uint64_t GetOldestSequence() const { return m_entries.empty() ? m_nextSequence : m_nextSequence - m_entries.size(); }
    std::uint64_t GetLatestSequence() const { return m_nextSequence - 1; }
    void Clear() { m_entries.clear(); }

private:
    std::deque<core::LogEntry> m_entries;
    // WHY: Clearやリングバッファ破棄後も値を戻さず、古いカーソルの取りこぼしを検出可能にする。
    std::uint64_t m_nextSequence = 1;
};

} // namespace fbzz::editor
