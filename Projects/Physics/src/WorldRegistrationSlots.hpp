/// @file    WorldRegistrationSlots.hpp
/// @brief   World の世代付き登録スロットに共通する操作。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace fbzz::physics::world_registration_detail {

/// @note 0 は未登録の世代値なので、最大値の次は 1 に戻す。
[[nodiscard]] inline uint32_t NextRegistrationGeneration(uint32_t generation)
{
    return generation == std::numeric_limits<uint32_t>::max() ? 1u : generation + 1u;
}

/// @note 古いハンドルからも寿命切れを検出できるよう、番号だけで参照する。
template<typename Slots, typename Handle>
[[nodiscard]] auto* FindSlotAtHandleIndex(Slots& slots, Handle handle)
{
    return handle.IsValid() && handle.slot <= slots.size() ? &slots[handle.slot - 1u] : nullptr;
}

template<typename Slots, typename Handle>
[[nodiscard]] auto* FindSlotWithGeneration(Slots& slots, Handle handle)
{
    auto* slot = FindSlotAtHandleIndex(slots, handle);
    return slot && slot->generation == handle.generation ? slot : nullptr;
}

template<typename Slots, typename IsVacant>
[[nodiscard]] size_t AcquireVacantSlot(Slots& slots, IsVacant isVacant)
{
    for (size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex) {
        if (isVacant(slots[slotIndex])) {
            return slotIndex;
        }
    }
    slots.emplace_back();
    return slots.size() - 1u;
}

template<typename Handle>
[[nodiscard]] Handle MakeRegistrationHandle(size_t slotIndex, uint32_t generation)
{
    return {static_cast<uint32_t>(slotIndex + 1u), generation};
}

} /// @note namespace fbzz::physics::world_registration_detail
