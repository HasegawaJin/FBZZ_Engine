// FBZZ Engine
// Detail/DetailGBuffer.hlsl | VS + PS (Deferred)
// Detail Mesh / Billboard を Deferred GBuffer (MRT) へ書き出す変種。
// VS は Detail.hlsl と完全に同一（インスタンシング・ビルボード展開）。PS だけ GBuffer 出力に差し替える。
//
// WHY: forward の Detail.hlsl はアンライト（albedo をそのまま出力）だったため AO/接触影/陰影が乗らなかった。
//      GBuffer 経由にすることで DeferredLighting / GTAO/SSAO / ContactShadows / SSR が地形と同様に効く。
//
// MRT (Pipeline/Deferred/GBuffer.hlsl と一致):
//   SV_Target0: RGB=albedo(linear), A=roughness
//   SV_Target1: RGB=worldNormal*0.5+0.5, A=metallic
#include "Common/Binding.hlsli"
#include "Common/Color.hlsli"
#include "Platform/Backend.hlsli"

cbuffer CameraConstants : register(CB_CAMERA)
{
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float3   cameraPos;
    float    nearZ;
    float    farZ;
    float3   _camPad;
};

#define FBZZ_MATERIAL_CONSTANTS
cbuffer DetailMaterialCB : register(CB_MATERIAL)
{
    float    alphaCutoff;
    uint     isBillboard;
    uint     hasAlbedoTex;
    float    _detailPad;
};

struct DetailInstance
{
    float3 pos;
    float  rotY;
    float  scale;
};
StructuredBuffer<DetailInstance> g_Instances : register(t0);

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
    float4 svPos    : SV_POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

struct GBufferOut
{
    float4 albedoRoughness : SV_Target0;
    float4 normalMetallic  : SV_Target1;
};

// VS は Detail.hlsl と同一。
PsIn VSMain(VsIn v, uint instId : SV_InstanceID)
{
    DetailInstance inst = g_Instances[instId];

    float s = sin(inst.rotY);
    float c = cos(inst.rotY);
    float3x3 rot = float3x3(
         c, 0.0f, s,
         0.0f, 1.0f, 0.0f,
        -s, 0.0f, c
    );

    float3 localPos = v.position * inst.scale;

    [branch]
    if (isBillboard)
    {
        float3 right = float3(view[0][0], view[1][0], view[2][0]);
        float3 up    = float3(0.0f, 1.0f, 0.0f);
        localPos = right * v.position.x * inst.scale
                 + up    * v.position.y * inst.scale;
    }
    else
    {
        localPos = mul(rot, localPos);
    }

    float3 worldPos = localPos + inst.pos;

    PsIn o;
    o.svPos    = mul(float4(worldPos, 1.0f), viewProjection);
    o.worldPos = worldPos;

    [branch]
    if (isBillboard)
        o.normal = -normalize(worldPos - cameraPos);
    else
        o.normal = normalize(mul(rot, v.normal));

    o.uv = v.uv;
    return o;
}

GBufferOut PSMain(PsIn p)
{
    float4 col = hasAlbedoTex
        ? gAlbedo.Sample(gSampler, p.uv)
        : float4(0.8f, 0.8f, 0.8f, 1.0f);

    if (alphaCutoff > 0.0f)
        clip(col.a - alphaCutoff);

    GBufferOut o;
    // sRGB → linear（GBuffer.hlsl と同じ規約）。AO は GBuffer に持たず GTAO/SSAO が担う。
    o.albedoRoughness = float4(SRGBToLinear(col.rgb), 0.9f); // 植生は粗め
    o.normalMetallic  = float4(normalize(p.normal) * 0.5f + 0.5f, 0.0f);
    return o;
}
