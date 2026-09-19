/// @file    RenderState.hpp
/// @brief   Renderer のパイプライン状態記述。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// トポロジー・ブレンド・深度・カリングなどをバックエンド非依存で表す。
/// PipelineState の生成キーとして使うため、値型として扱う。
#pragma once

/// wingdi.h が OPAQUE=2 を定義するため、公開 API 名には接尾辞を付ける。
/// @note `#undef` に依存すると include 順で再定義される危険が残るため、名前自体を衝突しない形にする。

namespace fbzz::renderer {

    enum class RasterizerMode {
        SOLID,        ///< 通常の塗りつぶし描画 (デフォルト)
        WIREFRAME,    ///< ワイヤーフレーム
        SOLID_NOCULL, ///< 塗りつぶし・背面カリングなし (スカイドーム内面描画)
        SOLID_FRONT_CULL, ///< 選択アウトライン用
    };

    /// ブレンド方程式の唯一の正本。DX12PsoCache は「ここに書いてある式をそれぞれの API 語彙へ
    /// 翻訳するだけ」で、バックエンド間で式を変えてはならない (過去に DX12 だけ ADDITIVE の
    /// SrcBlend が ONE になり、出力アルファが消えて加算パーティクルが白飛びした)。
    ///   OPAQUE_BLEND  : ブレンドなし
    ///   ALPHA_BLEND   : rgb = src.rgb * src.a + dst.rgb * (1 - src.a)
    ///   ADDITIVE      : rgb = src.rgb * src.a + dst.rgb  (1 本の PS が blendMode だけで
    ///                   3 モードに差し替わるため非事前乗算に統一。ONE にすると PREMULTIPLIED と区別できない)
    ///   PREMULTIPLIED : rgb = src.rgb + dst.rgb * (1 - src.a)  (アルファ共通 a = src.a + dst.a*(1-src.a))
    enum class BlendMode {
        OPAQUE_BLEND, ///< 不透明 (デフォルト)
        ALPHA_BLEND, ///< アルファブレンド (半透明)
        ADDITIVE,    ///< 加算合成 (パーティクル・エフェクト)
        /// 事前乗算アルファ: out = src.rgb + dst.rgb * (1 - src.a)
        /// @note 1 枚のテクスチャの中で「発光する芯」と「煙のように背景を隠す縁」を同時に
        ///       表現できる (alpha=0 かつ RGB>0 の画素が加算として振る舞う)。Alpha と Additive
        ///       を別エミッターに分ける必要がなく、爆炎のようなコアと煙の連続表現でも崩れない。
        PREMULTIPLIED,
    };

    enum class DepthMode {
        DEPTH_ON,    ///< 深度テスト・書き込みあり (デフォルト)
        DEPTH_READ,  ///< 深度テストあり・書き込みなし (半透明オブジェクト)
        DEPTH_OFF,   ///< 深度テスト・書き込みなし (デバッグ描画, UI)
        DEPTH_SKY,   ///< 深度テスト LESS_EQUAL・書き込みなし (スカイドーム: z=w で最遠面に描画)
    };

    enum class PrimitiveTopology {
        TRIANGLE_LIST,  ///< 通常の三角形描画 (デフォルト)
        LINE_LIST,      ///< デバッグ線描画
    };

    struct PipelineStateDesc {
        RasterizerMode rasterizer = RasterizerMode::SOLID;
        BlendMode      blend      = BlendMode::OPAQUE_BLEND;
        DepthMode      depth      = DepthMode::DEPTH_ON;
    };

} // namespace fbzz::renderer
