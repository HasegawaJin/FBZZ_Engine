/// @file    BakeFingerprint.hpp
/// @brief   焼いた出力の指紋 — 同じ入力なら同じ 16 桁の hex
/// @author  Hasegawa Jin
/// @date    2026-09-14
///
/// 焼いた画素そのものから 16 桁を返すことで、AI / テストは «絵が変わったか» を 1 回の
/// 文字列比較で判定できる。契約は «同じ環境で» に限る (GPU 結果はドライバが変わると変わりうる)。
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace fbzz::asset {

/// FNV-1a (64bit) を混ぜていく。暗号強度は要らない (壊れた出力を見分けるだけ)。
class BakeFingerprint {
public:
    void Add(std::span<const std::uint8_t> bytes);
    /// -0.0 は 0.0 に、NaN は 1 つの値に畳んでから混ぜる (同じ絵が別の指紋にならないように)。
    void Add(std::span<const float> values);
    void Add(std::uint64_t value);
    void Add(std::string_view text);

    /// 16 桁の小文字 hex。
    [[nodiscard]] std::string Finish() const;

private:
    std::uint64_t m_hash = 1469598103934665603ull;
};

[[nodiscard]] std::string BakeFingerprintOf(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::string BakeFingerprintOf(std::span<const float> values);

} // namespace fbzz::asset
