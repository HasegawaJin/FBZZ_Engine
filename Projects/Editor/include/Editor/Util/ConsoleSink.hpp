/// @file    ConsoleSink.hpp
/// @brief   Logger のログエントリをリングバッファに蓄積し ConsolePanel に渡す。
/// @author  Hasegawa Jin
/// @date    2026-05-21
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
    /// ログ内容をコピーせず、MCP等の差分購読に使える単調増加カーソルを返す。
    std::uint64_t GetOldestSequence() const { return m_entries.empty() ? m_nextSequence : m_nextSequence - m_entries.size(); }
    std::uint64_t GetLatestSequence() const { return m_nextSequence - 1; }
    void Clear() { m_entries.clear(); ++m_revision; }

    /// バッファ内容が変化するたびに増える世代番号。
    /// @note ConsolePanel はフィルタ結果 (Collapse 集約含む) をキャッシュし、変わったかだけを
    ///       安価に判定したい。追加と Clear の両方で進むため、件数比較では拾えない Clear→再追加も検出できる。
    std::uint64_t GetRevision() const { return m_revision; }

private:
    std::deque<core::LogEntry> m_entries;
    /// @note Clear やリングバッファ破棄後も値を戻さず、古いカーソルの取りこぼしを検出可能にする。
    std::uint64_t m_nextSequence = 1;
    std::uint64_t m_revision     = 0;
};

} // namespace fbzz::editor
