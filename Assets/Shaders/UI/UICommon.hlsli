/// @file    UICommon.hlsli
/// @brief   UI の定数バッファ・頂点・染め色の共通契約。
/// @author  Hasegawa Jin
/// @date    2026-08-23

/// @note UICommon.hlsli | UI
/// @note UI シェーダーの共通契約。UI 用シェーダーは必ずこれを include し、
/// @note Common/Constants.hlsli は include しない。

/// @note 契約をファイルにするか:
/// @note これまで UI は UISprite.hlsl / UIText.hlsl の 2 本が cbuffer とレジスタを
/// @note それぞれ手書きしていた。UI シェーダーを 1 本足すたびに b0 のレイアウトと
/// @note s5 というマジックナンバーを写経することになり、写し間違えても
/// @note 「絵が出ない」以上の情報が出ない。宣言を 1 箇所に閉じる。

/// @note マテリアルパラメータを MaterialConstants (b2) に置くか:
/// @note DX11Shader::BuildDescriptor() は cbuffer を **名前** で探す
/// @note (GetConstantBufferByName("MaterialConstants"))。この名前とレジスタに
/// @note 合わせておくだけで、既存の .mat / ShaderDescriptor / Inspector が
/// @note そのまま UI マテリアルにも効く。UI 専用の反射経路を作る必要が無い。
#ifndef UI_COMMON_HLSLI
#define UI_COMMON_HLSLI

#ifdef CONSTANTS_HLSLI
#error "UI shaders must not include Common/Constants.hlsli: it declares CameraConstants at b0, which the UI pass uses for UIConstants. Include UI/UICommon.hlsli only."
#endif

#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"

/// @note UISystem が毎ドロー埋める。マテリアル側からは読み取り専用。
cbuffer UIConstants : register(CB_UI)
{
    float4x4 g_Ortho;    ///< @note Canvas 空間 → clip 空間
    float4   g_Color;    ///< @note UIImage.color / UIText.color (頂点色は無い)
    float4   g_UVRect;   ///< @note xy = uvMin, zw = uvMax (アトラス内の矩形)
    /// @note xy = この矩形の Canvas ピクセル寸法, zw = 予約。
    /// @note 必要か: 角丸も枠線も影も「何ピクセルぶん」で決まる。0..1 の UV しか
    /// @note 無いと、同じ半径指定でも矩形の縦横比で角の形が変わってしまう。
    float4   g_Rect;
};

/// @note マテリアルを持たない既定シェーダー (UISprite / UIText) は
/// @note MaterialConstants を宣言しない。宣言するのはパラメータを公開したい側だけ。
/// @note 宣言する場合は必ずこの名前とレジスタを使う。

/// @note cbuffer MaterialConstants : register(CB_MATERIAL)
/// @note {
/// @note float4 tintColor;   // offset  0
/// @note float  cornerRadius;// offset 16
/// @note ...
/// @note };

/// @note 添字は MaterialConstants ではなく描画ごとの添字ブロック (b14) の t0 枠から引く。
/// @note UISystem は既定シェーダーにもマテリアルにも call.textures[0] でアトラス / 画像を差す。
/// @note 既定シェーダーは MaterialConstants を持たないので、そちらに添字を置くと誰も書かない。
FBZZ_TEX2D(g_Texture, TEX_ALBEDO_SLOT);
SamplerState g_Sampler : register(SAMPLER_UI);

struct UIVertexInput
{
    float2 pos : POSITION;
    /// @note 矩形内の正規化座標 (0,0)=左上 〜 (1,1)=右下。アトラス UV ではない。
    float2 uv  : TEXCOORD0;
    /// @note 頂点ごとの色。g_Color に **掛かる** (置き換えではない)。

    /// @note 要るか: g_Color は 1 ドローに 1 つしか無いので、それだけだと
    /// @note 「1 つの文字列の中で一部の語だけ色を変える」が原理的に書けない。
    /// @note 語ごとにドローを割ると、1 行の文章で何十ドローにもなる。
    /// @note 頂点に色を載せれば 1 ドローのまま文字ごとに色を変えられる。
    /// @note 色を使わない描画では UISystem が白 (1,1,1,1) を積む。
    float4 color : COLOR;
};

struct UIPixelInput
{
    float4 pos      : SV_POSITION;
    /// @note アトラス UV。g_UVRect で切り出した実際のサンプリング座標。
    float2 uv       : TEXCOORD0;
    /// @note 矩形内 0..1。図形を描くのはこちらを使う (アトラスの位置に依存しない)。
    float2 localUv  : TEXCOORD1;
    /// @note 頂点色の補間結果。PSMain は最終色にこれを掛けること。
    float4 color    : COLOR;
};

/// @note 全 UI シェーダーで共通の頂点処理。マテリアル側は PSMain だけ書けばよい。
UIPixelInput UIVertexMain(UIVertexInput input)
{
    UIPixelInput output;
    output.pos     = mul(float4(input.pos, 0.0f, 1.0f), g_Ortho);
    output.localUv = input.uv;
    output.uv      = g_UVRect.xy + input.uv * (g_UVRect.zw - g_UVRect.xy);
    output.color   = input.color;
    return output;
}

/// @note UI 要素の最終的な染め色。g_Color (要素の色) と頂点色を畳んだもの。

/// @note 関数にするか: マテリアル側が g_Color だけを掛けて頂点色を掛け忘れると、
/// @note リッチテキストの色指定がそのマテリアルでだけ黙って無視される。
/// @note 「掛けるべきもの」を 1 つの名前にしておけば、書き写す対象が 1 つで済む。
float4 UITint(UIPixelInput input)
{
    return g_Color * input.color;
}

/// @note 矩形内の位置をピクセル単位で返す。中心が原点。
float2 UILocalPixels(float2 localUv)
{
    return (localUv - 0.5f) * g_Rect.xy;
}

/// @note 矩形の半寸法 (ピクセル)。
float2 UIHalfSize()
{
    return g_Rect.xy * 0.5f;
}

/// @note 文字の atlas UV は入力のまま渡し、Sprite 用の g_UVRect を再適用しない。
UIPixelInput UIAtlasVertexMain(UIVertexInput input)
{
    UIPixelInput output;
    output.pos     = mul(float4(input.pos, 0.0f, 1.0f), g_Ortho);
    output.uv      = input.uv;
    output.localUv = input.uv;
    /// @note リッチテキストの色はここを通る。1 ドローに詰めた文字ごとに違う値が乗る。
    output.color   = input.color;
    return output;
}

#endif ///< @note UI_COMMON_HLSLI
