// パス終了時の HDR・深度・チャンネルを表示用 RGB へ変換する。
#include "Common/Binding.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D_T(float4, sourceImage, TEX_GBUFFER0_SLOT);
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
        /// @note カメラ深度は Reversed-Z (near=1, far=0)。視空間 Z を遠平面距離で正規化して値域調整へ渡す。
        /// @see Common/Space.hlsli の LinearizeDepth (同じ式を farPlane で割ったもの)
        float z = saturate(value.r);
        float linearZ = orthographic > 0.5
            ? (farPlane - z * (farPlane - nearPlane)) / max(farPlane, 0.000001)
            : nearPlane / max(nearPlane + z * (farPlane - nearPlane), 0.000001);
        color = linearZ.xxx;
    }

    color = saturate((color * exposure - rangeMin) / max(rangeMax - rangeMin, 0.000001));
    if (applyGamma > 0.5)
        color = pow(color, 1.0 / 2.2);
    // G-Buffer の alpha は roughness / metallic。プレビュー自身の透過には使わない。
    return float4(color, 1);
}
