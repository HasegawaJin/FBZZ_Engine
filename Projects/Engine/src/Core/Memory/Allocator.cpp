/// @file    Allocator.cpp
/// @brief   アロケータ共通ヘルパーの実装。
/// @author  Hasegawa Jin
/// @date    2026-06-01
///
/// アラインメント検証と統計更新の意味を全アロケータで揃える。
#include "Engine/Core/Memory/Allocator.hpp"

namespace fbzz::core {

bool IsPowerOfTwo(std::size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

std::size_t AlignSize(std::size_t size, std::size_t alignment)
{
    assert(IsPowerOfTwo(alignment));

    if (!IsPowerOfTwo(alignment)) {
        return size;
    }

    const std::size_t mask = alignment - 1;
    return (size + mask) & ~mask;
}

void UpdatePeak(MemoryStats& stats)
{
    if (stats.used > stats.peakUsed) {
        stats.peakUsed = stats.used;
    }
}

} // namespace fbzz::core
