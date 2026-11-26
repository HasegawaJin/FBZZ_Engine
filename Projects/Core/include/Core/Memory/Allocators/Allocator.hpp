/// @file    Allocator.hpp
/// @brief   カスタムアロケータ共通インターフェースと補助 API。
/// @author  Hasegawa Jin
/// @date    2026-06-01
/// @note 各アロケータを同じ呼び出し形で扱い、所有権と統計取得の入口を揃える。
#pragma once
#include <Core/Api.hpp>

#include "Core/Memory/Diagnostics/AllocationInfo.hpp"

#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>

namespace fbzz::core {

/// @brief アロケータの使用量を外部へ公開するための共通統計。
/// @note エディタやデバッグログから、断片化や一時メモリの過剰使用を早期に発見できるようにする。
struct MemoryStats {
    std::size_t capacity        = 0; ///< 管理領域の総容量（バイト）。
    std::size_t used            = 0; ///< 現在の使用量（バイト）。アロケータの占有量には位置合わせの余白も含む。
    std::size_t peakUsed        = 0; ///< 記録した使用量の最大値（バイト）。タグを合算する場合は各タグの最大値の合計。
    std::size_t allocationCount = 0; ///< 直近のリセット以降に成功した確保の回数。
    std::size_t freeCount       = 0; ///< 直近のリセット以降に記録した個別解放の回数。
    std::size_t activeCount     = 0; ///< 個別の確保と解放を追跡する場合の未解放件数。
};

/// @brief すべてのアロケータが満たす最小契約。
/// @note Allocate は容量不足時 nullptr、Free は実装によって no-op または解放、Reset は一括再利用を行う。
class Allocator {
public:
    virtual ~Allocator() = default;

    Allocator(const Allocator&)            = delete;
    Allocator& operator=(const Allocator&) = delete;

    [[nodiscard]] virtual void* Allocate(std::size_t size,
                                         std::size_t alignment = alignof(std::max_align_t)) = 0;
    virtual void Free(void* ptr) = 0;
    virtual void Reset() = 0;

    [[nodiscard]] virtual bool Owns(const void* ptr) const = 0;
    [[nodiscard]] virtual MemoryStats GetStats() const = 0;

protected:
    Allocator() = default;
    Allocator(Allocator&&) noexcept = default;
    Allocator& operator=(Allocator&&) noexcept = default;
};

/// @brief 2 の累乗アラインメントかを調べる共通ヘルパー。
/// @note std::align とビット丸めは 2 の累乗を前提とするため、assert と戻り値の両方で防御する。
[[nodiscard]] FBZZ_CORE_API bool IsPowerOfTwo(std::size_t value);

/// @brief size を alignment 境界へ切り上げる。固定長プールの stride 計算など、次要素の先頭も正しく整列させたい場面で使う。
[[nodiscard]] FBZZ_CORE_API std::size_t AlignSize(std::size_t size, std::size_t alignment);

/// @brief 統計のピーク使用量を更新する。
/// @note 各アロケータで同じピーク計算を重複させず、意味を統一する。
FBZZ_CORE_API void UpdatePeak(MemoryStats& stats);

/// @brief allocator からメモリを確保して T を構築する。
/// @note 直接 new を使わず、アロケータ所有の領域へオブジェクトを配置できるようにする。
template <class T, class... Args>
[[nodiscard]] T* CreateObject(Allocator& allocator, Args&&... args)
{
    void* memory = allocator.Allocate(sizeof(T), alignof(T));
    if (memory == nullptr) {
        return nullptr;
    }

    return std::construct_at(static_cast<T*>(memory), std::forward<Args>(args)...);
}

/// @brief T のデストラクタを呼んでから、対応するアロケータへメモリを返す。
/// @note LinearAllocator の Free は no-op なので、破棄だけ行い実メモリは Reset で再利用される。
template <class T>
void DestroyObject(Allocator& allocator, T* ptr)
{
    if (ptr == nullptr) {
        return;
    }

    std::destroy_at(ptr);
    allocator.Free(ptr);
}

} // namespace fbzz::core
