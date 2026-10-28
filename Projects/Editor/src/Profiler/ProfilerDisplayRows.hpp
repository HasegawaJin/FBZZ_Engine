/// @file    ProfilerDisplayRows.hpp
/// @brief   Bounded display retention and paced ordering without modifying recorded samples.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fbzz::editor::detail {

/// @note Row owns its strings, has a stable key, and reports dynamic string storage with DynamicBytes().
/// @note TTL uses selected application frames, so pause/Freeze do not consume retention or fabricate observations.
template<class Row>
class ProfilerDisplayRows {
public:
    static constexpr uint64_t RETAIN_FRAMES = 120;
    struct Entry {
        Row value;
        uint64_t lastSeenFrame = 0;
        bool present = false;
    };

    explicit ProfilerDisplayRows(size_t maxRows = 2048, size_t maxBytes = 2 * 1024 * 1024)
        : m_maxRows(maxRows), m_maxBytes(maxBytes) {}

    void Clear()
    {
        m_rows.clear(); m_session = 0; m_serial = 0; m_context.clear();
        m_initialized = false; m_limited = false; ++m_generation;
    }

    /// @note Context/session changes clear identity; seek and Freeze changes clear values but preserve widget identity.
    void Update(uint64_t session, uint64_t serial, const std::string& context, bool frozen,
        const std::vector<Row>& incoming)
    {
        if (!m_initialized || m_session != session || m_context != context) {
            Clear(); m_initialized = true; m_session = session; m_context = context;
        } else if (serial < m_serial || frozen != m_frozen || (frozen && serial != m_serial)) {
            m_rows.clear();
        } else if (serial == m_serial) return;
        m_serial = serial; m_frozen = frozen; m_limited = false;
        m_rows.erase(std::remove_if(m_rows.begin(), m_rows.end(), [&](const Entry& entry) {
            return serial > entry.lastSeenFrame && serial - entry.lastSeenFrame > RETAIN_FRAMES;
        }), m_rows.end());
        if (m_rows.capacity() < m_maxRows) m_rows.reserve(m_maxRows);
        size_t bytes = m_rows.capacity() * sizeof(Entry) + m_context.capacity() + 1;
        /// @note The temporary lookup's nodes and buckets are conservatively charged at 128 bytes per retained row.
        bytes += m_maxRows * 128;
        std::unordered_map<std::string_view, size_t> indices;
        indices.reserve(m_maxRows);
        for (size_t i = 0; i < m_rows.size(); ++i) {
            auto& entry = m_rows[i]; entry.present = false;
            bytes += entry.value.DynamicBytes(); indices.emplace(entry.value.key, i);
        }
        for (const auto& row : incoming) {
            const auto found = indices.find(row.key);
            const size_t previous = found == indices.end() ? 0 : m_rows[found->second].value.DynamicBytes();
            /// @note Copy growth is charged before allocating; oversized rows remain explicitly omitted.
            const size_t candidate = row.DynamicBytes() * 2 + 128;
            if (bytes - previous > m_maxBytes || candidate > m_maxBytes - (std::min)(bytes - previous, m_maxBytes) ||
                (found == indices.end() && m_rows.size() == m_maxRows)) {
                m_limited = true; continue;
            }
            if (found == indices.end()) {
                m_rows.push_back({row, serial, true});
                indices.emplace(m_rows.back().value.key, m_rows.size() - 1);
                bytes += m_rows.back().value.DynamicBytes();
            } else {
                auto& entry = m_rows[found->second];
                /// @note Erase before assignment: small-string storage can invalidate the lookup's string_view.
                const size_t index = found->second; indices.erase(found);
                entry.value = row; entry.lastSeenFrame = serial; entry.present = true;
                indices.emplace(entry.value.key, index); bytes = bytes - previous + entry.value.DynamicBytes();
            }
        }
    }

    [[nodiscard]] const std::vector<Entry>& Rows() const { return m_rows; }
    [[nodiscard]] bool Limited() const { return m_limited; }
    [[nodiscard]] uint64_t Generation() const { return m_generation; }
    [[nodiscard]] uint64_t Serial() const { return m_serial; }

private:
    std::vector<Entry> m_rows;
    size_t m_maxRows;
    size_t m_maxBytes;
    uint64_t m_session = 0;
    uint64_t m_serial = 0;
    uint64_t m_generation = 0;
    std::string m_context;
    bool m_initialized = false;
    bool m_frozen = false;
    bool m_limited = false;
};

/// @note Values remain instantaneous; only display rank changes are limited to 10 Hz. Header edits apply immediately.
class ProfilerDisplayOrder {
public:
    void Clear() { m_keys.clear(); m_nextRefreshMs = 0.0; m_signature = -1; }

    template<class Row, class Key, class Less>
    void Apply(std::vector<Row>& rows, Key key, Less less, int signature, double nowMs, bool changed = false)
    {
        std::unordered_map<std::string_view, size_t> ranks;
        ranks.reserve(m_keys.size());
        for (size_t i = 0; i < m_keys.size(); ++i) ranks.emplace(m_keys[i], i);
        const auto rank = [&](const Row& row) {
            const auto found = ranks.find(key(row));
            return found == ranks.end() ? m_keys.size() : found->second;
        };
        std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) { return rank(a) < rank(b); });
        if (changed || signature != m_signature || nowMs >= m_nextRefreshMs) {
            std::stable_sort(rows.begin(), rows.end(), less);
            m_nextRefreshMs = nowMs + 100.0; m_signature = signature;
        }
        m_keys.clear(); m_keys.reserve(rows.size());
        for (const auto& row : rows) m_keys.emplace_back(key(row));
    }

private:
    std::vector<std::string> m_keys;
    double m_nextRefreshMs = 0.0;
    int m_signature = -1;
};

} /// @note namespace fbzz::editor::detail
