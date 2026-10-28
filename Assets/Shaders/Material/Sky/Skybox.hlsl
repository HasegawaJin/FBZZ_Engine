/// @file    Skybox.hlsl
/// @brief   TextureCube を無限遠へ描くスカイボックス。
/// @author  Hasegawa Jin
/// @date    2026-06-23

/// @note Skybox.hlsl | Material/Sky
/// @note TextureCube スカイボックス — 無限遠に描画するため SV_Position.z = .w を設定する

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEXCUBE(texEnvCube, TEX_ENV_CUBE_SLOT);
SamplerState sampDefault : register(SAMPLER_DEFAULT);

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
    float3 color = texEnvCube.Sample(sampDefault, normalize(p.rayDir)).rgb;
    return float4(color, 1.0f);
}
