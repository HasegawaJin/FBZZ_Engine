/// @file    TextureFileDecoder.hpp
/// @brief   画像ファイルを GPU 非依存の RGBA8 ミップ列へ展開する。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#pragma once
#include <Graphics/Renderer/ITexture.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::renderer {

/// @brief 展開済みテクスチャ。各段は行ピッチ width*4 の詰めた RGBA8。
/// @note ワーカースレッドの成果物として渡すため、GPU ハンドルも DX 型も持たない。
/// @see Docs/design/asset-streaming.md «CPU 読み込み・依存関係»
struct DecodedTextureRGBA8 {
    struct Mip {
        std::uint32_t              width  = 0;
        std::uint32_t              height = 0;
        std::vector<std::uint8_t>  rgba;
    };
    std::vector<Mip> mips;
    /// @note 元画像の 0 段目の寸法。上位 mip を落として展開しても元の値を持つ (スプライト矩形の換算に使う)。
    std::uint32_t    sourceWidth  = 0;
    std::uint32_t    sourceHeight = 0;

    [[nodiscard]] std::size_t ByteSize() const;

    /// @brief 転送 API へ渡すビューを作る。
    /// @return 段が無い、または寸法 0 の段があれば false。
    /// @warning out の各要素は this の画素を指す。this より長く保持しない。
    [[nodiscard]] bool ToMipData(std::vector<TextureMipData>& out) const;
};

/// @brief 2x2 の平均で半分に縮める (RGBA8、各段 1 以上)。品質段の縮小はすべてこれを通す。
[[nodiscard]] DecodedTextureRGBA8::Mip DownsampleRGBA8Box2x(const DecodedTextureRGBA8::Mip& source);

/// @brief DDS / TGA / WIC 形式の画像を RGBA8 へ展開する。DDS のミップは全段、他は 0 段目のみ。
/// @param dropTopLevels 上位から落とす段数。ミップ付きは段を捨て、1 段の画像は DownsampleRGBA8Box2x を n 回かける。
/// @return 読めない・変換できなければ false (理由は outError、null 可)。out の中身は未定義。
/// @note スレッドセーフ。COM 未初期化のスレッドでは呼び出しの間だけ MTA で初期化する (WIC のため)。
/// @note 元画像は索引を持たないので全体を読む。部分 I/O は TextureStreamCache (.fztc) が受け持つ。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex CoInitializeEx
[[nodiscard]] bool DecodeTextureFileRGBA8(const std::string& path, DecodedTextureRGBA8& out,
                                          std::string* outError = nullptr,
                                          std::uint32_t dropTopLevels = 0);

/// @brief 全ミップの緑成分を反転する。元画像とキャッシュは変更しない。
void FlipTextureGreen(DecodedTextureRGBA8& texture);

} /// @note namespace fbzz::renderer
