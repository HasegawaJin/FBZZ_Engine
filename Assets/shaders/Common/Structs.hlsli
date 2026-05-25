// FBZZ Engine
// Structs.hlsli | Common
// Shared shader input and interpolator structures
#ifndef STRUCTS_HLSLI
#define STRUCTS_HLSLI

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 tangent  : TANGENT;
    float2 uv       : TEXCOORD;
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

#endif // STRUCTS_HLSLI
