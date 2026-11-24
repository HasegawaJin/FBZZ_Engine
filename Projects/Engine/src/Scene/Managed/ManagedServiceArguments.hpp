/// @file    ManagedServiceArguments.hpp
/// @brief   マネージドサービスの検証済み固定形式パケット。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace fbzz::scene::managed_detail {

/// @note 浮動小数点要素の有限性と文字列のバイト範囲はディスパッチの前に検証する。
struct ManagedServiceArguments {
    uint64_t relatedId = 0;
    int64_t integer = 0;
    std::vector<float> values;
    std::vector<std::string> text;
};

[[nodiscard]] inline bool FitsInt(int64_t integer)
{ return integer >= std::numeric_limits<int>::min() && integer <= std::numeric_limits<int>::max(); }

[[nodiscard]] inline bool MatchesServiceArguments(const ManagedBridgeRequest& request, const ManagedServiceArguments& arguments,
    uint32_t expectedFlags, std::size_t floatCount, std::size_t stringCount)
{
    return request.flags == expectedFlags && arguments.values.size() == floatCount && arguments.text.size() == stringCount
        && arguments.integer == 0 && arguments.relatedId == 0;
}

}
