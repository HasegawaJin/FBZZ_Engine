/// @file    StbFontImpl.cpp
/// @brief   stb_truetype / stb_rect_pack の実装を展開する唯一の翻訳単位。
/// @author  Hasegawa Jin
/// @date    2026-08-19
/// @note stb 系のシングルヘッダライブラリは IMPLEMENTATION マクロを定義した翻訳単位が 1 つだけ必要。
/// @note Editor 側の `StbImage.cpp` と同じ形にして「実装を持つ .cpp はここ」という所在を明確にする。
/// @note 他の .cpp はマクロ無しで `<stb_truetype.h>` を include すれば宣言だけを得る。

#define STB_TRUETYPE_IMPLEMENTATION
#define STB_RECT_PACK_IMPLEMENTATION

/// @note stb のデフォルトは assert.h の assert をそのまま使う。エンジンのエラー方針
/// @note       (回復不可能なら assert / 回復可能なら bool) と揃うため、そのまま採用する。
#include <stb_rect_pack.h>
#include <stb_truetype.h>
