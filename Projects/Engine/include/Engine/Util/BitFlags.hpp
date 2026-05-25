// FBZZ Engine
// BitFlags.hpp | fbzz::util
// enum class を型安全に扱うビットフラグ
// レイヤーマスクや状態フラグを整数演算へ安全に変換する薄いラッパー。
// ヘッダーオンリーで constexpr 利用を前提にする。
#pragma once
#include <type_traits>

namespace fbzz::util {

template<typename E>
struct BitFlags {
    static_assert(std::is_enum_v<E>, "BitFlags<E> requires an enum type");
    using T = std::underlying_type_t<E>;

    T value = 0;

    constexpr BitFlags() = default;
    constexpr explicit BitFlags(T raw)  : value(raw) {}
    constexpr BitFlags(E flag)          : value(static_cast<T>(flag)) {}

    // ── ビット操作 ────────────────────────────────────────────────────────
    constexpr BitFlags& Set(E flag)    { value |=  static_cast<T>(flag); return *this; }
    constexpr BitFlags& Clear(E flag)  { value &= ~static_cast<T>(flag); return *this; }
    constexpr BitFlags& Toggle(E flag) { value ^=  static_cast<T>(flag); return *this; }
    constexpr void      Reset()        { value = 0; }

    // ── 検査 ──────────────────────────────────────────────────────────────
    constexpr bool Has   (E flag)        const { return (value & static_cast<T>(flag)) != 0; }
    constexpr bool HasAll(BitFlags mask) const { return (value & mask.value) == mask.value; }
    constexpr bool HasAny(BitFlags mask) const { return (value & mask.value) != 0; }
    constexpr bool None  ()              const { return value == 0; }

    // ── 演算子 ────────────────────────────────────────────────────────────
    constexpr BitFlags  operator| (E flag)     const { return BitFlags(value |  static_cast<T>(flag)); }
    constexpr BitFlags  operator& (E flag)     const { return BitFlags(value &  static_cast<T>(flag)); }
    constexpr BitFlags  operator^ (E flag)     const { return BitFlags(value ^  static_cast<T>(flag)); }
    constexpr BitFlags& operator|=(E flag)           { value |=  static_cast<T>(flag); return *this; }
    constexpr BitFlags& operator&=(E flag)           { value &=  static_cast<T>(flag); return *this; }
    constexpr BitFlags  operator~ ()           const { return BitFlags(~value); }
    constexpr bool      operator==(BitFlags o) const { return value == o.value; }
    constexpr bool      operator!=(BitFlags o) const { return value != o.value; }
    constexpr explicit  operator bool()        const { return value != 0; }
};

// BitFlags 同士の合成もサポート
template<typename E>
constexpr BitFlags<E> operator|(BitFlags<E> a, BitFlags<E> b) { return BitFlags<E>(a.value | b.value); }
template<typename E>
constexpr BitFlags<E> operator&(BitFlags<E> a, BitFlags<E> b) { return BitFlags<E>(a.value & b.value); }

} // namespace fbzz::util
