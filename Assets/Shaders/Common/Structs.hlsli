/// @file    Structs.hlsli
/// @brief   シェーダーの頂点入力・補間値と GBuffer の共通出力。
/// @author  Hasegawa Jin
/// @date    2026-06-23
#ifndef STRUCTS_HLSLI
#define STRUCTS_HLSLI

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 tangent  : TANGENT;
    float2 uv       : TEXCOORD;
};

/// @note COLOR を宣言する変種だけ頂点末尾を読む。色なし入力と同じ Vertex バッファを共有できる。
struct VSInputColor
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 tangent  : TANGENT;
    float2 uv       : TEXCOORD;
    float4 color    : COLOR;
};

struct PSInputColor
{
    float4 svPosition : SV_POSITION;
    float3 worldPos   : TEXCOORD0;
    float3 normal     : TEXCOORD1;
    float3 tangent    : TEXCOORD2;
    float2 uv         : TEXCOORD3;
    float4 color      : COLOR;
};

struct SkinnedVSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float3 tangent     : TANGENT;
    float2 uv          : TEXCOORD;
    uint4  boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
};

struct PSInput
{
    float4 svPosition : SV_POSITION;
    float3 worldPos   : TEXCOORD0;
    float3 normal     : TEXCOORD1;
    float3 tangent    : TEXCOORD2;
    float2 uv         : TEXCOORD3;
};

struct GBufferOut
{
    float4 albedoRoughness : SV_Target0;
    float4 normalMetallic  : SV_Target1;
    /// @note 線形 HDR RGB。影・AO・材質の albedo で減衰させない。A は予約。
    float4 emission        : SV_Target2;
};

struct GBufferData
{
    float3 albedo;
    float  roughness;
    float3 worldNormal;
    float  metallic;
    float3 worldPos;
    float  ao;
};

#endif /// @note STRUCTS_HLSLI
