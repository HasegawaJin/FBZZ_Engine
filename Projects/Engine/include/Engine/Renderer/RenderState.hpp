// FBZZ Engine
// RenderState.hpp | fbzz::renderer
// Renderer のパイプライン状態記述
// トポロジー・ブレンド・深度・カリングなどをバックエンド非依存で表す。
// PipelineState の生成キーとして使うため、値型として扱う。
#pragma once

// wingdi.h が OPAQUE=2 を定義するため、公開 API 名には接尾辞を付ける。
// WHY: #undef に依存すると include 順で再定義される危険が残るため、名前自体を衝突しない形にする。

namespace fbzz::renderer {

    enum class RasterizerMode {
        SOLID,        // 通常の塗りつぶし描画 (デフォルト)
        WIREFRAME,    // ワイヤーフレーム
        SOLID_NOCULL, // 塗りつぶし・背面カリングなし (スカイドーム内面描画)
        SOLID_FRONT_CULL, // 選択アウトライン用
    };

    enum class BlendMode {
        OPAQUE_BLEND, // 不透明 (デフォルト)
        ALPHA_BLEND, // アルファブレンド (半透明)
        ADDITIVE,    // 加算合成 (パーティクル・エフェクト)
    };

    enum class DepthMode {
        DEPTH_ON,    // 深度テスト・書き込みあり (デフォルト)
        DEPTH_READ,  // 深度テストあり・書き込みなし (半透明オブジェクト)
        DEPTH_OFF,   // 深度テスト・書き込みなし (デバッグ描画, UI)
        DEPTH_SKY,   // 深度テスト LESS_EQUAL・書き込みなし (スカイドーム: z=w で最遠面に描画)
    };

    enum class PrimitiveTopology {
        TRIANGLE_LIST,  // 通常の三角形描画 (デフォルト)
        LINE_LIST,      // デバッグ線描画
    };

    struct PipelineStateDesc {
        RasterizerMode rasterizer = RasterizerMode::SOLID;
        BlendMode      blend      = BlendMode::OPAQUE_BLEND;
        DepthMode      depth      = DepthMode::DEPTH_ON;
    };

} // namespace fbzz::renderer
