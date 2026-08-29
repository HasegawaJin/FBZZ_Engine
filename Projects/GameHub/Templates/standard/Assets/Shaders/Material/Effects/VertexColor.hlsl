// FBZZ Engine
// Material/Effects/VertexColor.hlsl | Material
// 頂点カラーを乗せる Unlit — MeshBuilder で組んだ手続きメッシュ用
//
// WHY 専用シェーダーが要るか:
//   renderer::Vertex は色を末尾に持つが、COLOR を宣言しないシェーダーの入力レイアウトには
//   その要素が現れない (DX11Shader / DX12Shader がリフレクション結果を宣言順に詰めるため)。
//   つまり «頂点カラーが効くかどうか» はシェーダー側の宣言で決まる。Surface/* を使う限り
//   色は無視されるので、効かせたいメッシュにはこのマテリアルを当てる。

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

// 法線・接線・ワールド座標は参照しないので、Unlit と同じく最小の補間だけ渡す。
struct VertexColorPSInput
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
    float4 color      : COLOR;
};

VertexColorPSInput VSMain(VSInputColor v)
{
    VertexColorPSInput o;
    float4 worldPos4 = mul(float4(v.position, 1.0f), world);
    o.svPosition = mul(worldPos4, viewProjection);
    o.uv         = v.uv;
    o.color      = v.color;
    return o;
}

// 3 つの色をすべて乗算する (頂点 × マテリアル × テクスチャ)。
// 頂点カラーの既定は白なので、色を書かないメッシュは Unlit と同じ絵になる。
//
// WHY アルファを 1 で固定しないか: RenderState.hpp の契約どおり、PS は非事前乗算の
//      色とアルファを返し、合成方法は .mat の blend_mode が決める。帯の先端を薄くする
//      ようなフェードは、この alpha がそのまま通ることで成立する。
float4 PSMain(VertexColorPSInput p) : SV_Target0
{
    float4 color = albedo * p.color;
    if (textureMask & 1u)
        color *= texAlbedo.Sample(sampDefault, p.uv);
    return color;
}
