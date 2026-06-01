// FBZZ Engine
// RenderLayer.hpp | fbzz::renderer
// 描画レイヤーの分類
// RenderLayer と Unity 風 RenderQueue の基準値を定義する。
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

    // RenderQueue — MaterialComponent::renderQueue に入れる Unity 風の描画順プリセット。
    // WHY: 値を enum class に閉じると Custom VFX や水面の微調整で 3000 + 10 のような
    //      差し込みができない。あえて int32_t 定数にし、標準帯とユーザー定義帯だけを共有する。
    struct RenderQueue {
        static constexpr int32_t BACKGROUND   = 1000;
        static constexpr int32_t GEOMETRY     = 2000;
        static constexpr int32_t ALPHA_TEST   = 2450;
        static constexpr int32_t TRANSPARENT  = 3000;
        static constexpr int32_t CUSTOM       = 3500;
        static constexpr int32_t OVERLAY      = 4000;

        static constexpr int32_t CUSTOM_MIN   = 3100;
        static constexpr int32_t CUSTOM_MAX   = 3999;
    };

} // namespace fbzz::renderer
