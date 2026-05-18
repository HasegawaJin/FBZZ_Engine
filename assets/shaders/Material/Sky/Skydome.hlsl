// FBZZ Engine
// Skydome.hlsl | Material/Sky
// Rayleigh + Mie 大気散乱による手続き型スカイドーム

#include "Common/Constants.hlsli"
#include "Platform/DX11.hlsli"
#include "Rendering/Atmosphere.hlsli"
#include "Rendering/ToneMap.hlsli"

struct SkyVSInput
{
    float3 position : POSITION;
};

struct SkyPSInput
{
    float4 svPosition : SV_POSITION;
    float3 rayDir     : TEXCOORD0;
};

SkyPSInput VSMain(SkyVSInput v)
{
    SkyPSInput o;
    float3x3 rotView = (float3x3)view;
    float4   clip    = mul(float4(mul(v.position, rotView), 1.0f), projection);
    o.svPosition = clip.xyww;
    o.rayDir     = v.position;
    return o;
}

float4 PSMain(SkyPSInput p) : SV_Target0
{
    float3 ray    = normalize(p.rayDir);
    float3 sunDir = normalize(-lightDir);

    float3 sky = ComputeAtmosphericScattering(
        ray, sunDir,
        rayleighScattering, mieScattering, mieG,
        sunIntensity);
    sky += SunDisk(ray, sunDir, sunIntensity);

    // Skydome 自身でトーンマップして LDR に落とす
    sky = ToneMap_ACES(sky * exposure);

    return float4(sky, 1.0f);
}
