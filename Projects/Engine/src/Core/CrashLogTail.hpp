/// @file    CrashLogTail.hpp
/// @brief   クラッシュ時にヒープを使わず列挙できる固定長のログ末尾。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Engine/Core/ILogSink.hpp>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fbzz::core {

/// @note Logger が配送を直列化する。書き手は同時に 1 本で、読み手は書き手の停止後に列挙する。
/// @note クラッシュ経路はロックを取らず best-effort で読むため、進行中の 1 行は欠けうる。
class CrashLogTail final : public ILogSink {
public:
    static constexpr size_t LINE_COUNT = 200;
    static constexpr size_t LINE_BYTES = 512;

    void OnLog(const LogEntry& entry) override
    {
        const uint32_t index = m_next.load(std::memory_order_relaxed);
        char* line = m_lines[index % LINE_COUNT];
        const char* prefix = LevelPrefix(entry.level);
        const size_t prefixLen = std::strlen(prefix);
        std::memcpy(line, prefix, prefixLen);

        /// @note UTF-8 の継続バイトの手前まで戻し、文字の途中で切らない。
        /// @see https://datatracker.ietf.org/doc/html/rfc3629#section-3 RFC 3629 section 3 UTF-8 definition
        size_t length = std::min(entry.message.size(), LINE_BYTES - 1 - prefixLen);
        while (length > 0 && length < entry.message.size()
               && (static_cast<unsigned char>(entry.message[length]) & 0xC0u) == 0x80u)
            --length;
        std::memcpy(line + prefixLen, entry.message.data(), length);
        line[prefixLen + length] = '\0';
        m_next.store(index + 1, std::memory_order_release);
    }

    /// @brief 保持している行を古い順に渡す。文字列の所有権は移さない。
    template <class Fn>
    void ForEachLine(Fn&& fn) const
    {
        const uint32_t next = m_next.load(std::memory_order_acquire);
        const uint32_t count = std::min<uint32_t>(next, static_cast<uint32_t>(LINE_COUNT));
        for (uint32_t i = next - count; i != next; ++i)
            fn(m_lines[i % LINE_COUNT]);
    }

    void Clear() { m_next.store(0, std::memory_order_release); }

private:
    static const char* LevelPrefix(LogLevel level)
    {
        switch (level) {
        case LogLevel::DEBUG:     return "[DEBUG] ";
        case LogLevel::INFO:      return "[INFO]  ";
        case LogLevel::WARNING:   return "[WARN]  ";
        case LogLevel::LOG_ERROR: return "[ERROR] ";
        }
        return "";
    }

    char m_lines[LINE_COUNT][LINE_BYTES] = {};
    std::atomic<uint32_t> m_next{0};
};
}
