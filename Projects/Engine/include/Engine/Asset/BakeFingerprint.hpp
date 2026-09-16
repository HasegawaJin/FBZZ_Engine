/// @file    BakeFingerprint.hpp
/// @brief   焼いた出力の指紋 — 同じ入力なら同じ 16 桁の hex
/// @author  Hasegawa Jin
/// @date    2026-09-14
///
/// WHY 焼く側が指紋を出すか:
///   AI は «パラメータを変えた結果、絵が変わったのか» を知りたい。画像を base64 で往復させて
///   見比べるのは高くつき、しかも «変わっていない» の判定が主観になる。焼いた画素そのものから
///   16 桁を返せば、変化の有無は 1 回の文字列比較で決まる。
///   同じ指紋 = 同じバイナリ・同じ入力で同じ絵、が守れているかの検査にもそのまま使う
///   (GPU で解いた結果はドライバが変わると変わりうるので、契約は «同じ環境で» に限る)。
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
