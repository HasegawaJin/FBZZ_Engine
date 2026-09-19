/// @file    FlipbookMips.hpp
/// @brief   フリップブック Atlas のミップ — コマを跨がず、アルファの意味に合わせて縮める
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// @note PNG は 1 段しか読まれず遠景の粒子がエイリアスでちらつくため専用ミップを作る。汎用縮小は
///       隣コマの混入とストレートアルファの黒縁を招くため、コマ境界が偶数座標に来る段までに限り、
///       色はアルファで重み付けして縮める。
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace fbzz::asset {

/// Atlas の画素が何を表しているか。縮め方が変わる。
enum class FlipbookMipContent : std::uint8_t {
    StraightSrgb,       ///< sRGB の色 + ストレートアルファ。色はアルファで重み付けする
    PremultipliedSrgb,  ///< sRGB の事前乗算色。リニアへ戻して平均する
    CoverageWeighted,   ///< リニアのデータ + 被覆率 (A)。RGB は A で重み付けする (6-way Positive)
    Plain,              ///< リニアのデータ。そのまま平均する (Motion Vector・6-way Negative)
};

struct FlipbookMipLevel {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

/// 作れるミップの段数 (0 段目を含む)。コマの 1 辺が 4 px を下回る手前か、奇数になる手前で止める。
/// 4 px で止めるのは BC 圧縮のブロックと、それより小さいコマは隣へ滲むだけで意味が無いため。
[[nodiscard]] std::uint32_t FlipbookMipLevelCount(std::uint32_t tileWidth, std::uint32_t tileHeight);

/// 0 段目 (入力そのもの) から FlipbookMipLevelCount 段ぶんを返す。
/// width / height がコマの寸法で割り切れなければ 0 段目だけを返す。
[[nodiscard]] std::vector<FlipbookMipLevel> BuildFlipbookMips(std::span<const std::uint8_t> rgba,
                                                              std::uint32_t width, std::uint32_t height,
                                                              std::uint32_t tileWidth, std::uint32_t tileHeight,
                                                              FlipbookMipContent content);

} // namespace fbzz::asset
