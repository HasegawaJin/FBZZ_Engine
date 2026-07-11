// FBZZ Engine
// Foliage/FoliageGBuffer.hlsl | VS + PS (Deferred)
// 樹木・大型植生を Deferred GBuffer (MRT) へ書き出す変種。
// VS は Foliage.hlsl と同一（インスタンシング・Y 回転）。PS だけ GBuffer 出力に差し替える。
//
// WHY: forward の Foliage.hlsl は簡易 N·L だったため AO/接触影/SSR が乗らなかった。GBuffer 経由で
//      DeferredLighting / GTAO/SSAO / ContactShadows / SSR を地形・メッシュと同様に適用する。
//
// MRT: SV_Target0=albedo(linear)+roughness / SV_Target1=worldNormal*0.5+0.5+metallic
#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Color.hlsli"
#include "Platform/DX11.hlsli"

cbuffer FoliageMaterialCB : register(CB_MATERIAL)
{
    float4 baseColor;
    uint   hasAlbedo;
    float  alphaCutoff;
    float2 _foliagePad;
};

struct FoliageInstance
{
    float3 pos;
    float  rotY;
    float  scale;
};
StructuredBuffer<FoliageInstance> g_Instances : register(t0);

Texture2D    gAlbedo  : register(TEX_ALBEDO);
SamplerState gSampler : register(SAMPLER_DEFAULT);

struct VsIn
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 tangent  : TANGENT;
    float2 uv       : TEXCOORD0;
};

struct PsIn
{
    float4 svPos  : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
};

struct GBufferOut
{
    float4 albedoRoughness : SV_Target0;
    float4 normalMetallic  : SV_Target1;
};

// VS は Foliage.hlsl と同一。
PsIn VSMain(VsIn input, uint instanceId : SV_InstanceID)
{
    FoliageInstance instance = g_Instances[instanceId];
    const float s = sin(instance.rotY);
    const float c = cos(instance.rotY);
    const float3x3 rotation = float3x3(
         c, 0.0f, s,
         0.0f, 1.0f, 0.0f,
        -s, 0.0f, c);

    const float3 worldPosition =
        mul(rotation, input.position * instance.scale) + instance.pos;

    PsIn output;
    output.svPos  = mul(float4(worldPosition, 1.0f), viewProjection);
    output.normal = normalize(mul(rotation, input.normal));
    output.uv     = input.uv;
    return output;
}

GBufferOut PSMain(PsIn input)
{
    // baseColor は線形 tint として扱い、テクスチャは sRGB→linear する（GBuffer.hlsl と同じ規約）。
    float3 albedo = baseColor.rgb;
    float  alpha  = baseColor.a;
    if (hasAlbedo)
    {
        float4 tex = gAlbedo.Sample(gSampler, input.uv);
        albedo *= SRGBToLinear(tex.rgb);
        alpha  *= tex.a;
    }
    if (alphaCutoff > 0.0f)
        clip(alpha - alphaCutoff);

    GBufferOut o;
    o.albedoRoughness = float4(albedo, 0.85f);
    o.normalMetallic  = float4(normalize(input.normal) * 0.5f + 0.5f, 0.0f);
    return o;
}
