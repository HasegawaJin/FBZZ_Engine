// FBZZ Engine
// Material/Effects/SkinnedMeshTrail.hlsl | Material
// MeshTrailComponent 用 Skinned Mesh 残像シェーダー
// PSO: SOLID_NOCULL or SOLID + ALPHA_BLEND + DEPTH_READ

#include "Common/Binding.hlsli"

#define FBZZ_MATERIAL_CONSTANTS
cbuffer MeshTrailConstants : register(CB_MATERIAL)
{
    float4 trailColor;
};

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"

Texture2D    gMeshTrailTex : register(TEX_ALBEDO);
SamplerState gSampler      : register(SAMPLER_DEFAULT);

struct MeshTrailPSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

float4x4 BlendSkinMatrix(SkinnedVSInput v)
{
    return boneMatrices[v.boneIndices.x] * v.boneWeights.x
         + boneMatrices[v.boneIndices.y] * v.boneWeights.y
         + boneMatrices[v.boneIndices.z] * v.boneWeights.z
         + boneMatrices[v.boneIndices.w] * v.boneWeights.w;
}

MeshTrailPSIn VSMain(SkinnedVSInput v)
{
    MeshTrailPSIn o;
    float4x4 skin = BlendSkinMatrix(v);
    float4 localPos = mul(float4(v.position, 1.0f), skin);
    float3 localN = normalize(mul(v.normal, (float3x3)skin));
    float3 localT = normalize(mul(v.tangent, (float3x3)skin));
    float4 worldPos = mul(localPos, world);
    o.svPosition = mul(worldPos, viewProjection);
    o.uv = v.uv + (localN.xy + localT.xy) * 1e-8f;
    return o;
}

float4 PSMain(MeshTrailPSIn input) : SV_Target0
{
    return gMeshTrailTex.Sample(gSampler, input.uv) * trailColor;
}
