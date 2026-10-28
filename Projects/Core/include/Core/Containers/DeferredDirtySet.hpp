/// @file    DeferredDirtySet.hpp
/// @brief   Owner世代とkeyで通知を集約し、所有vectorへ差分を取り出す値型collector。
/// @author  Hasegawa Jin
/// @date    2026-10-23
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace fbzz::core {

/// @note Single-threaded value collector; callers serialize Mark/TakePending/Reset at their existing owner boundary.
/// @note ownerGeneration is a caller-issued nonzero lifetime, not a frame number; stale producers must pass their captured lifetime.
/// @note Keys and reason bits are copied; this type neither dispatches callbacks nor retains owner/Component pointers.
/// @note Hash/Equal must be stable while keys are pending, and equal keys must have equal hashes.
template<typename Key, typename ReasonBits = uint32_t, typename Hash = std::hash<Key>, typename Equal = std::equal_to<Key>>
class DeferredDirtySet {
    static_assert(std::is_integral_v<ReasonBits> && std::is_unsigned_v<ReasonBits>);
public:
    struct Entry {
        Key key;
        ReasonBits reasons = 0;
    };

    enum class MarkResult { ADDED, MERGED, REJECTED };

    void Reset(uint64_t ownerGeneration)
    {
        m_ownerGeneration = ownerGeneration;
        m_pending.clear();
        AdvanceEpoch();
    }

    /// @note A stale owner or an empty reason is rejected without modifying pending work.
    [[nodiscard]] MarkResult Mark(uint64_t ownerGeneration, const Key& key, ReasonBits reasons)
    {
        if (ownerGeneration == 0 || ownerGeneration != m_ownerGeneration || reasons == 0)
            return MarkResult::REJECTED;
        if (m_slots.empty()) GrowIndex();
        size_t slot = FindSlot(key);
        if (m_slots[slot].epoch == m_epoch) {
            auto& entry = m_pending[m_slots[slot].entry];
            entry.reasons = static_cast<ReasonBits>(entry.reasons | reasons);
            return MarkResult::MERGED;
        }
        if (m_pending.size() >= m_slots.size() / 2) { GrowIndex(); slot = FindSlot(key); }
        m_slots[slot] = {m_pending.size(), m_epoch};
        m_pending.push_back({key, reasons});
        return MarkResult::ADDED;
    }

    /// @note output owns the detached batch; notifications added while the caller consumes it remain pending for the next take.
    /// @note Call after finishing the previous output batch; a rejected lifetime leaves both output and pending unchanged.
    [[nodiscard]] bool TakePending(uint64_t ownerGeneration, std::vector<Entry>& output)
    {
        if (ownerGeneration == 0 || ownerGeneration != m_ownerGeneration) return false;
        output.clear();
        output.swap(m_pending);
        AdvanceEpoch();
        return true;
    }

    [[nodiscard]] bool Empty() const { return m_pending.empty(); }
    [[nodiscard]] size_t PendingCount() const { return m_pending.size(); }
    [[nodiscard]] size_t RetainedBytes() const
    {
        return m_pending.capacity() * sizeof(Entry) + m_slots.capacity() * sizeof(Slot);
    }

private:
    struct Slot { size_t entry = 0; uint64_t epoch = 0; };

    size_t FindSlot(const Key& key) const
    {
        size_t slot = m_hash(key) & (m_slots.size() - 1);
        while (m_slots[slot].epoch == m_epoch && !m_equal(m_pending[m_slots[slot].entry].key, key))
            slot = (slot + 1) & (m_slots.size() - 1);
        return slot;
    }

    /// @note Normal drains/reset do not scan high-water slot capacity; only lifetime-sized epoch wrap clears stamps.
    void AdvanceEpoch()
    {
        if (++m_epoch != 0) return;
        for (auto& slot : m_slots) slot.epoch = 0;
        m_epoch = 1;
    }

    void GrowIndex()
    {
        assert(m_slots.size() <= std::numeric_limits<size_t>::max() / 2);
        const size_t slotCount = m_slots.empty() ? 16 : m_slots.size() * 2;
        m_slots.assign(slotCount, {});
        for (size_t index = 0; index < m_pending.size(); ++index) {
            size_t slot = m_hash(m_pending[index].key) & (slotCount - 1);
            while (m_slots[slot].epoch == m_epoch) slot = (slot + 1) & (slotCount - 1);
            m_slots[slot] = {index, m_epoch};
        }
    }

    uint64_t m_ownerGeneration = 0;
    uint64_t m_epoch = 1;
    std::vector<Entry> m_pending;
    std::vector<Slot> m_slots;
    [[no_unique_address]] Hash m_hash;
    [[no_unique_address]] Equal m_equal;
};

} /// @note namespace fbzz::core
