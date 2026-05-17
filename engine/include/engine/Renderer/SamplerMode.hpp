#pragma once

namespace fbzz::renderer
{

    enum class SamplerMode 
    {
        WRAP_LINEAR,    // 繰り返しテクスチャ・線形補間 (デフォルト)
        WRAP_POINT,     // 繰り返しテクスチャ・最近傍補間 (ピクセルアート)
        CLAMP_LINEAR,   // 端でクランプ・線形補間 (UI, スカイボックス, レンダーターゲット参照)
    };

} // namespace fbzz::renderer