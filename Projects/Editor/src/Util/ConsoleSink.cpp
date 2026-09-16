/// @file    ConsoleSink.cpp
/// @brief   ILogSink 実装 — リングバッファにログエントリを蓄積する。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include <Editor/Util/ConsoleSink.hpp>

namespace fbzz::editor {

void ConsoleSink::OnLog(const core::LogEntry& entry)
{
    if (m_entries.size() >= MAX_ENTRIES)
        m_entries.pop_front();
    m_entries.push_back(entry);
    ++m_nextSequence;
    ++m_revision;
}

} // namespace fbzz::editor
