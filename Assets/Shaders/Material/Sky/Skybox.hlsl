// FBZZ Engine
// Skybox.hlsl | Material/Sky
// TextureCube スカイボックス — 無限遠に描画するため SV_Position.z = .w を設定する

#include "Common/Constants.hlsli"
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
    // ビュー行列から平行移動を除いて回転のみ適用し、空を無限遠に固定する
    float3x3 rotView = (float3x3)view;
    float4   clip    = mul(float4(mul(v.position, rotView), 1.0f), projection);

    /// @note z = 0 は Reversed-Z の最遠。DEPTH_SKY (GREATER_EQUAL) で空いた画素だけを埋め、他のジオメトリに隠れる。
    o.svPosition = float4(clip.xy, 0.0f, clip.w);
    o.rayDir     = v.position;
    return o;
}

float4 PSMain(SkyPSInput p) : SV_Target0
{
    float3 color = texEnvCube.Sample(sampDefault, normalize(p.rayDir)).rgb;
    return float4(color, 1.0f);
}
