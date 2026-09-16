// FBZZ Engine
// Material/Surface/Unlit.hlsl | Material
// ライティングなし — アルベドをそのまま出力する
#ifndef UNLIT_HLSL
#define UNLIT_HLSL

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;       // RGBA ベースカラー  offset 0
    uint   textureMask;  // テクスチャフラグ   offset 16
};

Texture2D    texAlbedo   : register(TEX_ALBEDO);
SamplerState sampDefault : register(SAMPLER_DEFAULT);

// WHY: Unlit は法線・接線・ワールド座標を一切参照しない。
//      共通 PSInput を使うと不要な行列乗算と補間レジスタを毎頂点・毎ピクセルで消費するため、
//      専用の最小入力へ縮退させる。
struct UnlitPSInput
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

UnlitPSInput VSMain(VSInput v)
{
    UnlitPSInput o;
    float4 worldPos4 = mul(float4(v.position, 1.0f), world);
    o.svPosition = mul(worldPos4, viewProjection);
    o.uv         = v.uv;
    return o;
}

// WHY テクスチャを «置き換え» ではなく «乗算» するか:
//   以前はテクスチャが束縛されている間 albedo を丸ごと捨てていた。SpriteRenderer と
//   LineRenderer は色を albedo へ毎フレーム書き込む経路なので (GameplayComponentSystems の
//   applyMaterial)、テクスチャを差した瞬間に色指定が黙って効かなくなっていた。
//
// WHY アルファを 1 で固定しないか:
//   RenderState.hpp の契約は «PS は非事前乗算の色とアルファを出し、合成は BlendMode が
//   決める»。1 を返すと ALPHA_BLEND では常に不透明、ADDITIVE では src.a を掛ける式から
//   フェードが消える。透過 PNG のスプライトも、線の不透明度も、この 1 行で死んでいた。
float4 PSMain(UnlitPSInput p) : SV_Target0
{
    float4 color = albedo;
    if (textureMask & 1u)
        color *= texAlbedo.Sample(sampDefault, p.uv);
    return color;
}

#endif // UNLIT_HLSL
