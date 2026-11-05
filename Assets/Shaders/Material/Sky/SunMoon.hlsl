/// @file    SunMoon.hlsl
/// @brief   スカイドームの上へ太陽・月ディスクだけを加算描画する。
/// @author  Hasegawa Jin
/// @date    2026-07-01
#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
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
    o.svPosition = SkyClipPosition(v.position, view, projection);
    o.rayDir = v.position;
    return o;
}

float4 PSMain(SkyPSInput p) : SV_Target0
{
    float3 ray    = normalize(p.rayDir);
    float3 sunDir = normalize(-lightDir);

    /// @note 明るさは skyDimmer で取る。Skydome と同じ軸に揃え、太陽ディスクと空の明るさを一致させる。
    /// @note 光源の色・方向を共有し、観測高度に応じた地平線で画素ごとに隠す。
    float3 color = SunDisk(ray, sunDir, sunIntensity * skyDimmer, cameraPos.y, planetRadius) * max(lightColor, 0.0f);
    if (moonEnabled > 0.5f)
    {
        float3 moonDir = -sunDir;
        color += MoonDisk(ray, moonDir, moonColor, moonBrightness, moonSize, cameraPos.y, planetRadius);
    }

    return float4(max(color, 0.0f), 1.0f);
}
