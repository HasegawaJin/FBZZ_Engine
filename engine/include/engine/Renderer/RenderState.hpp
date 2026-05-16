#pragma once

namespace fbzz::renderer {

    enum class RasterizerMode {
        SOLID,       // 通常の塗りつぶし描画 (デフォルト)
        WIREFRAME,   // ワイヤーフレーム
    };

    enum class BlendMode {
        OPAQUE,      // 不透明 (デフォルト)
        ALPHA_BLEND, // アルファブレンド (半透明)
        ADDITIVE,    // 加算合成 (パーティクル・エフェクト)
    };

    enum class DepthMode {
        DEPTH_ON,    // 深度テスト・書き込みあり (デフォルト)
        DEPTH_READ,  // 深度テストあり・書き込みなし (半透明オブジェクト)
        DEPTH_OFF,   // 深度テスト・書き込みなし (デバッグ描画, UI)
    };

    struct PipelineStateDesc {
        RasterizerMode rasterizer = RasterizerMode::SOLID;
        BlendMode      blend      = BlendMode::OPAQUE;
        DepthMode      depth      = DepthMode::DEPTH_ON;
    };

} // namespace fbzz::renderer