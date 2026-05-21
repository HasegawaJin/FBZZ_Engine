// FBZZ Engine
// Structs.hlsli | Common
// 頂点・GBuffer 構造体定義。C++ 側 Mesh.hpp の Vertex と一致させること
#ifndef STRUCTS_HLSLI
#define STRUCTS_HLSLI

// ---- 頂点入力 (全メッシュ共通) ------------------------------------------
struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 tangent  : TANGENT;  // 法線マップ対応 (Step 6a で Mesh.hpp にも追加)
    float2 uv       : TEXCOORD;
};

// ---- VS → PS 共通転送構造体 ---------------------------------------------
struct PSInput
{
    float4 svPosition : SV_POSITION;
    float3 worldPos   : TEXCOORD0;
    float3 normal     : TEXCOORD1;
    float3 tangent    : TEXCOORD2;
    float2 uv         : TEXCOORD3;
};

// ---- GBuffer MRT 出力 ---------------------------------------------------
struct GBufferOut
{
    float4 albedoRoughness : SV_Target0;  // albedo(RGB) + roughness(A)
    float4 normalMetallic  : SV_Target1;  // world normal(RGB) + metallic(A)
};

// ---- Lighting パスが GBuffer から復元したデータ --------------------------
struct GBufferData
{
    float3 albedo;
    float  roughness;
    float3 worldNormal;
    float  metallic;
    float3 worldPos;   // invViewProjection + depth から復元
    float  ao;         // SSAO テクスチャから読む
};

#endif // STRUCTS_HLSLI