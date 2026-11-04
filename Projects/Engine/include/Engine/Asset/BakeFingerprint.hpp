/// @file    BakeFingerprint.hpp
/// @brief   焼いた出力の指紋 — 同じ入力なら同じ 16 桁の hex
/// @author  Hasegawa Jin
/// @date    2026-09-14

/// @note 焼いた画素そのものから 16 桁を返すことで、AI / テストは «絵が変わったか» を 1 回の
/// @note 文字列比較で判定できる。契約は «同じ環境で» に限る (GPU 結果はドライバが変わると変わりうる)。
#pragma once
#include <Engine/Api.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace fbzz::asset {

/// @note FNV-1a (64bit) を混ぜていく。暗号強度は要らない (壊れた出力を見分けるだけ)。
class BakeFingerprint {
public:
    FBZZ_ENGINE_API void Add(std::span<const std::uint8_t> bytes);
    /// @note -0.0 は 0.0 に、NaN は 1 つの値に畳んでから混ぜる (同じ絵が別の指紋にならないように)。
    FBZZ_ENGINE_API void Add(std::span<const float> values);
    FBZZ_ENGINE_API void Add(std::uint64_t value);
    FBZZ_ENGINE_API void Add(std::string_view text);

    /// @note 16 桁の小文字 hex。
    [[nodiscard]] FBZZ_ENGINE_API std::string Finish() const;

private:
    std::uint64_t m_hash = 1469598103934665603ull;
};

[[nodiscard]] FBZZ_ENGINE_API std::string BakeFingerprintOf(std::span<const std::uint8_t> bytes);
[[nodiscard]] FBZZ_ENGINE_API std::string BakeFingerprintOf(std::span<const float> values);

} /// @note namespace fbzz::asset
