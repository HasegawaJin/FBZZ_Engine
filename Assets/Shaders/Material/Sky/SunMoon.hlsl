/// @file    SunMoon.hlsl
/// @brief   スカイドームの上へ太陽・月ディスクだけを加算描画する。
/// @author  Hasegawa Jin
/// @date    2026-07-01
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
    /// @note z = 0 は Reversed-Z の最遠。DEPTH_SKY (GREATER_EQUAL) で空いた画素だけを埋め、他のジオメトリに隠れる。
    o.svPosition = float4(clip.xy, 0.0f, clip.w);
    o.rayDir     = v.position;
    return o;
}

float4 PSMain(SkyPSInput p) : SV_Target0
{
    float3 ray    = normalize(p.rayDir);
    float3 sunDir = normalize(-lightDir);

    /// @note 明るさは skyDimmer で取る。Skydome と同じ軸に揃え、太陽ディスクと空の明るさを一致させる。
    /// @note 地平線より下は SunDisk / MoonDisk が画素ごとに隠す (Atmosphere.hlsli の HorizonMask)。
    float3 color = SunDisk(ray, sunDir, sunIntensity * skyDimmer);
    if (moonEnabled > 0.5f)
    {
        float3 moonDir = -sunDir;
        color += MoonDisk(ray, moonDir, moonColor, moonBrightness, moonSize);
    }

    return float4(max(color, 0.0f), 1.0f);
}
