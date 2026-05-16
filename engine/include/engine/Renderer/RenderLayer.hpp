#pragma once
#include <cstdint>

namespace fbzz::renderer {

    enum class RenderLayer : uint32_t {
        OPAQUE      = 0,  // 不透明オブジェクト (ソートなし、前から後へ)
        TRANSPARENT = 1,  // 半透明オブジェクト (デプスソートあり、後から前へ)
        OVERLAY     = 2,  // デバッグ描画・UI  (デプステストなし、最後に描画)
    };

} // namespace fbzz::renderer