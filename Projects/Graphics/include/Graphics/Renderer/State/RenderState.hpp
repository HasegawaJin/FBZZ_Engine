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
        SOLID,            ///< 塗りつぶしと背面カリングを行う既定の経路。
        WIREFRAME,        ///< 背面をカリングし、三角形を辺だけで描く。
        SOLID_NOCULL,     ///< スカイドーム内面など、両面を塗りつぶす。
        SOLID_FRONT_CULL, ///< 選択アウトラインなどで正面をカリングする。
        WIREFRAME_NOCULL, ///< 両面の三角形を辺だけで描く。
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
        OPAQUE_BLEND, ///< 既存の出力色を新しいソース色で置き換える。
        ALPHA_BLEND,  ///< ソースのアルファで半透明合成する。
        ADDITIVE,     ///< ソース色にアルファを掛けて既存の出力色へ加算する。
        /// @note 事前乗算アルファ: out = src.rgb + dst.rgb * (1 - src.a)
        /// @note 1 枚のテクスチャの中で「発光する芯」と「煙のように背景を隠す縁」を同時に
        /// @note 表現できる (alpha=0 かつ RGB>0 の画素が加算として振る舞う)。Alpha と Additive
        /// @note を別エミッターに分ける必要がなく、爆炎のようなコアと煙の連続表現でも崩れない。
        PREMULTIPLIED, ///< ソース色にアルファを掛けた状態の入力を半透明合成する。
    };

    /// @note 比較の向きは束縛中の RT が決める (RenderTargetDesc::reversedZ)。
    /// @note 通常の Z では LESS / LESS_EQUAL、Reversed-Z では GREATER / GREATER_EQUAL になる。
    enum class DepthMode {
        DEPTH_ON,   ///< 深度テストと書き込みを行う既定の経路。
        DEPTH_READ, ///< 深度をテストするが書き込まない。半透明などで使う。
        DEPTH_OFF,  ///< 深度のテストも書き込みも省く。UI などで使う。
        DEPTH_SKY,  ///< 最遠と等しい深度も通し、深度を書き込まない。
    };

    enum class PrimitiveTopology {
        TRIANGLE_LIST,  ///< 3 頂点ごとに独立した三角形を作る。
        LINE_LIST,      ///< 2 頂点ごとに独立した線分を作る。
        TRIANGLE_STRIP, ///< 32 ビットインデックスの 0xFFFFFFFF でストリップを再開する。
    };

    struct PipelineStateDesc {
        RasterizerMode rasterizer = RasterizerMode::SOLID; ///< 面の塗り方と正面・背面のカリング方式。
        BlendMode      blend      = BlendMode::OPAQUE_BLEND; ///< 出力色を既存のレンダーターゲットと合成する方式。
        DepthMode      depth      = DepthMode::DEPTH_ON; ///< 描画先の深度をテスト・更新する方式。
        /// @note 単位は D32_FLOAT の 1 ULP (プリミティブ内の最大深度の指数で決まる)。符号は束縛中の RT の向きでバックエンドが裏返す。
        /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage-depth-bias (Depth Bias)
        int32_t        depthBias      = 0; ///< 深度バイアスの定数項。正値はカメラ側へ寄せる。
        float          depthBiasSlope = 0.0f; ///< 最大深度勾配に掛ける深度バイアス係数。正値はカメラ側へ寄せる。
    };

} // namespace fbzz::renderer
