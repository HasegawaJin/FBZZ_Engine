// FBZZ Engine
// RenderLayer.hpp | fbzz::renderer
// 描画レイヤーの分類
// Opaque / Transparent などの送信順制御に使う軽量 enum。
// RenderSystem と Renderer が同じ基準でソートできるようにする。
#pragma once
#include <cstdint>

// wingdi.h が OPAQUE=2, TRANSPARENT=1 を定義するため enum class の enumerator と衝突する
#ifdef OPAQUE
#undef OPAQUE
#endif
#ifdef TRANSPARENT
#undef TRANSPARENT
#endif

namespace fbzz::renderer {

    enum class RenderLayer : uint32_t {
        OPAQUE      = 0,  // 不透明オブジェクト (ソートなし、前から後へ)
        TRANSPARENT = 1,  // 半透明オブジェクト (デプスソートあり、後から前へ)
        OVERLAY     = 2,  // デバッグ描画・UI  (デプステストなし、最後に描画)
    };

} // namespace fbzz::renderer
