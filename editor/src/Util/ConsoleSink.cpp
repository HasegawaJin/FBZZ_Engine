// FBZZ Engine
// ConsoleSink.cpp | fbzz::editor
// ILogSink 実装 — リングバッファにログエントリを蓄積する
#include <editor/Util/ConsoleSink.hpp>

namespace fbzz::editor {

void ConsoleSink::OnLog(const core::LogEntry& entry)
{
    if (m_entries.size() >= MAX_ENTRIES)
        m_entries.pop_front();
    m_entries.push_back(entry);
}

} // namespace fbzz::editor
