/// @file    AssetHandle.hpp
/// @brief   世代番号付き型安全アセットハンドル。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// AssetManager のスロットプールへの安全な参照。id=0 は常に Null。
#pragma once
#include <cstdint>

namespace fbzz::asset {

template<typename T>
struct AssetHandle {
    uint32_t id  = 0;
    uint32_t gen = 0;

    [[nodiscard]] bool IsValid()         const { return id != 0; }
    explicit operator bool()             const { return IsValid(); }
    bool operator==(const AssetHandle&) const = default;

    static AssetHandle Null() { return {}; }
};

} // namespace fbzz::asset
