// FBZZ Engine
// SunMoon.hlsl | Material/Sky
// スカイドーム上へ太陽・月ディスクだけを加算描画する

#include "Common/Constants.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Atmosphere.hlsli"

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

    float3 color = SunDisk(ray, sunDir, sunIntensity * lightIntensity);
    if (moonEnabled > 0.5f)
    {
        float3 moonDir = -sunDir;
        color += MoonDisk(ray, moonDir, moonColor, moonBrightness, moonSize);
    }

    return float4(max(color, 0.0f), 1.0f);
}
