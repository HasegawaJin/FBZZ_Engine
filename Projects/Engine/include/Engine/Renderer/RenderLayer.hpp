// FBZZ Engine
// RenderLayer.hpp | fbzz::renderer
// 描画レイヤーの分類
// RenderLayer と Unity 風 RenderQueue の基準値を定義する。
#pragma once
#include <cstdint>

// wingdi.h が OPAQUE=2, TRANSPARENT=1 を定義するため、公開 API 名には接尾辞を付ける。
// WHY: #undef に依存すると include 順で再定義される危険が残るため、名前自体を衝突しない形にする。

namespace fbzz::renderer {

    enum class RenderLayer : uint32_t {
        OPAQUE_LAYER      = 0,  // 不透明オブジェクト (ソートなし、前から後へ)
        TRANSPARENT_LAYER = 1,  // 半透明オブジェクト (デプスソートあり、後から前へ)
        OVERLAY_LAYER     = 2,  // デバッグ描画・UI  (デプステストなし、最後に描画)
    };

    // RenderQueue — MaterialAsset::renderQueue に入れる Unity 風の描画順プリセット。
    // WHY: 値を enum class に閉じると Custom VFX や水面の微調整で 3000 + 10 のような
    //      差し込みができない。あえて int32_t 定数にし、標準帯とユーザー定義帯だけを共有する。
    struct RenderQueue {
        static constexpr int32_t BACKGROUND   = 1000;
        static constexpr int32_t GEOMETRY     = 2000;
        static constexpr int32_t ALPHA_TEST   = 2450;
        static constexpr int32_t TRANSPARENT_QUEUE = 3000;
        static constexpr int32_t CUSTOM       = 3500;
        static constexpr int32_t OVERLAY      = 4000;

        static constexpr int32_t CUSTOM_MIN   = 3100;
        static constexpr int32_t CUSTOM_MAX   = 3999;
    };

} // namespace fbzz::renderer
