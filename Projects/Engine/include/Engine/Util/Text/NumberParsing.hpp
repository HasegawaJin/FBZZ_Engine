/// @file    NumberParsing.hpp
/// @brief   十進整数文字列の全体一致による読み取り。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <charconv>
#include <string_view>
#include <type_traits>

namespace fbzz::util {

/// @note 空白と末尾文字を受理しない。符号・範囲・出力への書き込みは整数型の from_chars に従う。
template<typename Integer>
[[nodiscard]] bool ParseInteger(std::string_view text, Integer& value)
{
    static_assert(std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

} /// @note namespace fbzz::util
