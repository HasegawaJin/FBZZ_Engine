/// @file    Format.hpp
/// @brief   バックエンド非依存のピクセル形式と、レンダーターゲットの記述。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// DXGI_FORMAT は各バックエンドの PRIVATE include にしか無いので、上位レイヤーが
/// 形式を指定する手段が無かった。RT は RGBA16F + D32 の決め打ちで、
/// «深度を一度も読まないポストプロセスの中継 RT» まで全画面ぶんの深度を抱えていた。
#pragma once

#include <cstdint>

namespace fbzz::renderer {

/// カラーの格納形式。値そのものは保存しない (バックエンドが対応表を持つ)。
enum class Format {
    RGBA16F,     ///< 既定。HDR カラー。1 画素 8 バイト
    RGBA8,       ///< トーンマップ後の LDR / マスク。1 画素 4 バイト
    R11G11B10F,  ///< アルファの要らない HDR。1 画素 4 バイト
    RG16F,       ///< 2 成分 (モーションベクターなど)。1 画素 4 バイト
    R16F,        ///< 1 成分の中間精度。1 画素 2 バイト
    R8,          ///< 1 成分のマスク。1 画素 1 バイト
};

/// レンダーターゲットの «形». 寸法は別引数で渡す (呼び出し側の書き味を変えないため)。
struct RenderTargetDesc {
    /// 同時出力カラーバッファ数。0 = 深度専用。
    uint32_t colorCount = 1;
    /// カラーの形式。colorCount = 0 のときは意味を持たない。
    Format   format = Format::RGBA16F;
    /// 深度バッファを持つか。
    ///
    /// WHY 既定を true にするか: 既存の CreateRenderTarget は «必ず深度が付く» 前提で
    ///     書かれている。既定を false にすると、深度を読んでいる箇所が黙って壊れる。
    ///     要らないと分かった RT から明示的に false へ倒していく。
    bool     withDepth = true;
};

/// 1 画素あたりのバイト数。VRAM 見積もりに使う。
[[nodiscard]] constexpr uint32_t BytesPerPixel(Format format)
{
    switch (format) {
    case Format::RGBA16F:    return 8;
    case Format::RGBA8:      return 4;
    case Format::R11G11B10F: return 4;
    case Format::RG16F:      return 4;
    case Format::R16F:       return 2;
    case Format::R8:         return 1;
    }
    return 8;
}

} // namespace fbzz::renderer
