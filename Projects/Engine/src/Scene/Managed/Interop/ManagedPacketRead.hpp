/// @file    ManagedPacketRead.hpp
/// @brief   Managed 値 packet の境界検査と POD 読み取り。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <cstddef>
#include <cstring>
#include <string_view>

namespace fbzz::scene::managed_detail {

/// @pre T は packet の固定幅 POD 表現。型・endianness・値の妥当性は呼び出し元が検証する。
/// @return 範囲外なら false。offset と value は変更しない。
template<typename T>
[[nodiscard]] bool ReadManagedValue(std::string_view bytes, size_t& offset, T& value)
{
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

} /// @note namespace fbzz::scene::managed_detail
