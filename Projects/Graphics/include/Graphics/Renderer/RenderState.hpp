/// @file    RenderState.hpp
/// @brief   Renderer のパイプライン状態記述。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note トポロジー・ブレンド・深度・カリングなどをバックエンド非依存で表す。
/// @note PipelineState の生成キーとして使うため、値型として扱う。
#pragma once

#include <cstdint>

/// @note wingdi.h が OPAQUE=2 を定義するため、公開 API 名には接尾辞を付ける。
/// @note `#undef` に依存すると include 順で再定義される危険が残るため、名前自体を衝突しない形にする。

namespace fbzz::renderer {

    enum class RasterizerMode {
        SOLID,        ///< @brief 通常の塗りつぶし描画 (デフォルト)
        WIREFRAME,    ///< @brief ワイヤーフレーム
        SOLID_NOCULL, ///< @brief 塗りつぶし・背面カリングなし (スカイドーム内面描画)
        SOLID_FRONT_CULL, ///< @brief 選択アウトライン用
        WIREFRAME_NOCULL, ///< @brief ワイヤーフレーム・背面カリングなし
    };

    /// @note ブレンド方程式の唯一の正本。DX12PsoCache は「ここに書いてある式をそれぞれの API 語彙へ
    /// @note 翻訳するだけ」で、バックエンド間で式を変えてはならない (過去に DX12 だけ ADDITIVE の
    /// @note SrcBlend が ONE になり、出力アルファが消えて加算パーティクルが白飛びした)。
    /// @note OPAQUE_BLEND  : ブレンドなし
    /// @note ALPHA_BLEND   : rgb = src.rgb * src.a + dst.rgb * (1 - src.a)
    /// @note ADDITIVE      : rgb = src.rgb * src.a + dst.rgb  (1 本の PS が blendMode だけで
    /// @note 3 モードに差し替わるため非事前乗算に統一。ONE にすると PREMULTIPLIED と区別できない)
    /// @note PREMULTIPLIED : rgb = src.rgb + dst.rgb * (1 - src.a)  (アルファ共通 a = src.a + dst.a*(1-src.a))
    enum class BlendMode {
        OPAQUE_BLEND, ///< @brief 不透明 (デフォルト)
        ALPHA_BLEND, ///< @brief アルファブレンド (半透明)
        ADDITIVE,    ///< @brief 加算合成 (パーティクル・エフェクト)
        /// @note 事前乗算アルファ: out = src.rgb + dst.rgb * (1 - src.a)
        /// @note 1 枚のテクスチャの中で「発光する芯」と「煙のように背景を隠す縁」を同時に
        /// @note 表現できる (alpha=0 かつ RGB>0 の画素が加算として振る舞う)。Alpha と Additive
        /// @note を別エミッターに分ける必要がなく、爆炎のようなコアと煙の連続表現でも崩れない。
        PREMULTIPLIED,
    };

    /// @note 比較の向きは束縛中の RT が決める (RenderTargetDesc::reversedZ)。
    /// @note 通常の Z では LESS / LESS_EQUAL、Reversed-Z では GREATER / GREATER_EQUAL になる。
    enum class DepthMode {
        DEPTH_ON,    ///< @brief 深度テスト・書き込みあり (デフォルト)
        DEPTH_READ,  ///< @brief 深度テストあり・書き込みなし (半透明オブジェクト)
        DEPTH_OFF,   ///< @brief 深度テスト・書き込みなし (デバッグ描画, UI)
        DEPTH_SKY,   ///< @brief 最遠と等しい深度も通す・書き込みなし (スカイドーム: 最遠面に描画)
    };

    enum class PrimitiveTopology {
        TRIANGLE_LIST,  ///< @brief 通常の三角形描画 (デフォルト)
        LINE_LIST,      ///< @brief デバッグ線描画
    };

    struct PipelineStateDesc {
        RasterizerMode rasterizer = RasterizerMode::SOLID;
        BlendMode      blend      = BlendMode::OPAQUE_BLEND;
        DepthMode      depth      = DepthMode::DEPTH_ON;
        /// @brief 深度バイアスの定数項。正で手前へ寄せる (同一平面に重なった面の Z-fighting 回避)。
        /// @note 単位は D32_FLOAT の 1 ULP (プリミティブ内の最大深度の指数で決まる)。符号は束縛中の RT の向きでバックエンドが裏返す。
        /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage-depth-bias (Depth Bias)
        int32_t        depthBias      = 0;
        /// @brief 深度バイアスの傾き項 (MaxDepthSlope に掛ける係数)。正で手前へ寄せる。
        float          depthBiasSlope = 0.0f;
    };

}
