// FBZZ Engine
// Material/Effects/Trail.hlsl | Material
// TrailComponent 用リボンメッシュシェーダー
// PSO: SOLID_NOCULL + ALPHA_BLEND + DEPTH_READ

#include "Common/Binding.hlsli"

#define FBZZ_MATERIAL_CONSTANTS
cbuffer TrailConstants : register(CB_MATERIAL)
{
    // colorStart / colorEnd は CPU 側でリニア化済み (GeometryPasses.hpp の TrailCB を参照)。
    float4 colorStart;
    float4 colorEnd;
    float  uvScrollSpeed;
    float  uvTiling;
    float  trailTime;
    // bit0 = テクスチャが sRGB エンコード。
    uint   gTrailFlags;
};

#include "Common/Color.hlsli"
#include "Common/Constants.hlsli"
#include "Platform/Backend.hlsli"

#define FBZZ_TRAIL_SRGB_TEXTURE 1u

Texture2D    gTrailTex : register(TEX_ALBEDO);
SamplerState gSampler  : register(SAMPLER_DEFAULT);

struct TrailVSIn
{
    float3 position : POSITION;
    float  age      : TEXCOORD0;
    float  v        : TEXCOORD1;
    float  u        : TEXCOORD2;
};

struct TrailPSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
    float  age        : TEXCOORD1;
};

TrailPSIn VSMain(TrailVSIn input)
{
    TrailPSIn output;
    output.svPosition = mul(float4(input.position, 1.0f), viewProjection);
    output.uv = float2(input.u, input.v);
    output.age = input.age;
    return output;
}

float4 PSMain(TrailPSIn input) : SV_Target0
{
    // WHY: uvTiling を先に掛けることで、テクスチャの繰り返し回数とスクロール速度を独立して調整できる。
    float2 uv = float2(input.uv.x * uvTiling + trailTime * uvScrollSpeed, input.uv.y);
    float4 tex = gTrailTex.Sample(gSampler, uv);
    // 素材は他のマテリアルと同じ規約でシェーダー側がリニア化する
    // (このエンジンは _SRGB フォーマットの SRV を作らない)。
    if ((gTrailFlags & FBZZ_TRAIL_SRGB_TEXTURE) != 0u) tex.rgb = SRGBToLinear(tex.rgb);
    float4 color = lerp(colorEnd, colorStart, saturate(input.age));
    return tex * color;
}
