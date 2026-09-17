/// @file    Format.hpp
/// @brief   バックエンド非依存のピクセル形式と、レンダーターゲットの記述。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// @note DXGI_FORMAT は各バックエンドの PRIVATE include にしか無いため、上位レイヤーが RT の
///       形式や深度有無を指定する手段としてこの列挙と RenderTargetDesc を置く。
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
    /// @brief 深度バッファを持つか。
    /// @note 既定を true にするのは、CreateRenderTarget の呼び出し側の多くが «必ず深度が付く»
    ///       前提で書かれているため。不要と分かった RT から明示的に false へ倒していく。
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
