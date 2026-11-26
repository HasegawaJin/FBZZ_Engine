/// @file    Allocator.cpp
/// @brief   アロケータ共通ヘルパーの実装。
/// @author  Hasegawa Jin
/// @date    2026-06-01
/// @note アラインメント検証と統計更新の意味を全アロケータで揃える。
#include "Core/Memory/Allocators/Allocator.hpp"

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

    // 例えば境界が 8 バイトなら 7 を足して下位 3 ビットを消す。これで元のサイズ以上となる最小の 8 の倍数へ切り上がる。
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
