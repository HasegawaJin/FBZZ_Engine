// パス終了時の HDR・深度・チャンネルを表示用 RGB へ変換する。
#include "Common/Binding.hlsli"
#include "Common/Fullscreen.hlsli"

Texture2D<float4> sourceImage : register(TEX_GBUFFER0);
SamplerState pointClamp : register(SAMPLER_POINT_CLAMP);

cbuffer PreviewConstants : register(b5)
{
    float displayMode;
    float exposure;
    float rangeMin;
    float rangeMax;
    float nearPlane;
    float farPlane;
    float orthographic;
    float applyGamma;
};

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    float4 value = sourceImage.SampleLevel(pointClamp, p.uv, 0);
    if (!all(isfinite(value)))
        return float4(1, 0, 1, 1);

    float3 color = value.rgb;
    if (displayMode == 2) color = value.rrr;
    else if (displayMode == 3) color = value.ggg;
    else if (displayMode == 4) color = value.bbb;
    else if (displayMode == 5) color = value.aaa;
    else if (displayMode == 6) color = value.rgb * 0.5 + 0.5;
    else if (displayMode == 7)
    {
        // DirectX の通常 Z (near=0, far=1)。遠平面距離で正規化して値域調整へ渡す。
        float z = saturate(value.r);
        float linearZ = orthographic > 0.5
            ? (nearPlane + z * (farPlane - nearPlane)) / max(farPlane, 0.000001)
            : nearPlane / max(farPlane - z * (farPlane - nearPlane), 0.000001);
        color = linearZ.xxx;
    }

    color = saturate((color * exposure - rangeMin) / max(rangeMax - rangeMin, 0.000001));
    if (applyGamma > 0.5)
        color = pow(color, 1.0 / 2.2);
    // G-Buffer の alpha は roughness / metallic。プレビュー自身の透過には使わない。
    return float4(color, 1);
}
