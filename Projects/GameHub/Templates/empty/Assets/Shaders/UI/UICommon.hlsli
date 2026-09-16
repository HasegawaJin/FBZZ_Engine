// FBZZ Engine
// UICommon.hlsli | UI
// UI シェーダーの共通契約。UI 用シェーダーは必ずこれを include し、
// Common/Constants.hlsli は include しない。
//
// WHY 契約をファイルにするか:
//   これまで UI は UISprite.hlsl / UIText.hlsl の 2 本が cbuffer とレジスタを
//   それぞれ手書きしていた。UI シェーダーを 1 本足すたびに b0 のレイアウトと
//   s5 というマジックナンバーを写経することになり、写し間違えても
//   「絵が出ない」以上の情報が出ない。宣言を 1 箇所に閉じる。
//
// WHY マテリアルパラメータを MaterialConstants (b2) に置くか:
//   DX11Shader::BuildDescriptor() は cbuffer を **名前** で探す
//   (GetConstantBufferByName("MaterialConstants"))。この名前とレジスタに
//   合わせておくだけで、既存の .mat / ShaderDescriptor / Inspector が
//   そのまま UI マテリアルにも効く。UI 専用の反射経路を作る必要が無い。
#ifndef UI_COMMON_HLSLI
#define UI_COMMON_HLSLI

#ifdef CONSTANTS_HLSLI
#error "UI shaders must not include Common/Constants.hlsli: it declares CameraConstants at b0, which the UI pass uses for UIConstants. Include UI/UICommon.hlsli only."
#endif

#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"

// UISystem が毎ドロー埋める。マテリアル側からは読み取り専用。
cbuffer UIConstants : register(CB_UI)
{
    float4x4 g_Ortho;    // Canvas 空間 → clip 空間
    float4   g_Color;    // UIImage.color / UIText.color (頂点色は無い)
    float4   g_UVRect;   // xy = uvMin, zw = uvMax (アトラス内の矩形)
    // xy = この矩形の Canvas ピクセル寸法, zw = 予約。
    // WHY 必要か: 角丸も枠線も影も「何ピクセルぶん」で決まる。0..1 の UV しか
    //     無いと、同じ半径指定でも矩形の縦横比で角の形が変わってしまう。
    float4   g_Rect;
};

// マテリアルを持たない既定シェーダー (UISprite / UIText) は
// MaterialConstants を宣言しない。宣言するのはパラメータを公開したい側だけ。
// 宣言する場合は必ずこの名前とレジスタを使う。
//
//   cbuffer MaterialConstants : register(CB_MATERIAL)
//   {
//       float4 tintColor;   // offset  0
//       float  cornerRadius;// offset 16
//       ...

    // bindless のテクスチャ添字。Material::Upload が毎フレーム書き込む。
    // ここに宣言した枠だけが Inspector に出る (Common/MaterialTextures.hlsli)。
    uint texAlbedoIndex;
//   };

FBZZ_MATERIAL_TEX(g_Texture, texAlbedoIndex);
SamplerState g_Sampler : register(SAMPLER_UI);

struct UIVertexInput
{
    float2 pos : POSITION;
    // 矩形内の正規化座標 (0,0)=左上 〜 (1,1)=右下。アトラス UV ではない。
    float2 uv  : TEXCOORD0;
    // 頂点ごとの色。g_Color に **掛かる** (置き換えではない)。
    //
    // WHY 要るか: g_Color は 1 ドローに 1 つしか無いので、それだけだと
    //     「1 つの文字列の中で一部の語だけ色を変える」が原理的に書けない。
    //     語ごとにドローを割ると、1 行の文章で何十ドローにもなる。
    //     頂点に色を載せれば 1 ドローのまま文字ごとに色を変えられる。
    // NOTE: 色を使わない描画では UISystem が白 (1,1,1,1) を積む。
    float4 color : COLOR;
};

struct UIPixelInput
{
    float4 pos      : SV_POSITION;
    // アトラス UV。g_UVRect で切り出した実際のサンプリング座標。
    float2 uv       : TEXCOORD0;
    // 矩形内 0..1。図形を描くのはこちらを使う (アトラスの位置に依存しない)。
    float2 localUv  : TEXCOORD1;
    // 頂点色の補間結果。PSMain は最終色にこれを掛けること。
    float4 color    : COLOR;
};

// 全 UI シェーダーで共通の頂点処理。マテリアル側は PSMain だけ書けばよい。
UIPixelInput UIVertexMain(UIVertexInput input)
{
    UIPixelInput output;
    output.pos     = mul(float4(input.pos, 0.0f, 1.0f), g_Ortho);
    output.localUv = input.uv;
    output.uv      = g_UVRect.xy + input.uv * (g_UVRect.zw - g_UVRect.xy);
    output.color   = input.color;
    return output;
}

// UI 要素の最終的な染め色。g_Color (要素の色) と頂点色を畳んだもの。
//
// WHY 関数にするか: マテリアル側が g_Color だけを掛けて頂点色を掛け忘れると、
//     リッチテキストの色指定がそのマテリアルでだけ黙って無視される。
//     「掛けるべきもの」を 1 つの名前にしておけば、書き写す対象が 1 つで済む。
float4 UITint(UIPixelInput input)
{
    return g_Color * input.color;
}

// 矩形内の位置をピクセル単位で返す。中心が原点。
float2 UILocalPixels(float2 localUv)
{
    return (localUv - 0.5f) * g_Rect.xy;
}

// 矩形の半寸法 (ピクセル)。
float2 UIHalfSize()
{
    return g_Rect.xy * 0.5f;
}

#endif // UI_COMMON_HLSLI