// FBZZ Engine
// Debug/SelectionMaskParticle.hlsl | fbzz::renderer
// CPUパーティクルの可視ピクセルをSelection Outline用マスクへ合成する

#include "Common/Binding.hlsli"
#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/ParticleCommon.hlsli"

Texture2D    gParticleTex : register(TEX_ALBEDO);
SamplerState gSampler     : register(SAMPLER_DEFAULT);

// Particle.hlslと同じb2レイアウトを使う。
// WHY: 通常描画とMask描画のビルボード形状が1pxでもずれると、輪郭が本体から浮いて見える。
cbuffer ParticleRenderConstants : register(CB_MATERIAL)
{
    uint  gRenderMode;
    float gStretchedVelocityScale;
    float gStretchedLengthScale;
    float gSoftParticleFadeDistance;
    uint  gSoftParticles;
    uint  gMaxParticles;
    uint  gEffectsFlags;
    float gDistortionStrength;
    float gLightingStrength;
    float gEmissiveScale;
    float gMotionVectorStrength;
    float gScreenWidth;
    float gScreenHeight;
    float gSizeAxisScaleX;
    float gSizeAxisScaleY;
    float gShadowStrength;
    uint  gVolumetricSteps;
    float gVolumetricDensity;
    float gVolumetricAnisotropy;
    float gVolumetricNoiseScale;
    uint  gGpuSortEnabled;
    float gSelfShadowStrength;
    float gParticlePad1;
    float gParticlePad2;
};

struct ParticleVSIn
{
    float3 center       : POSITION;
    float2 uv           : TEXCOORD0;
    float4 color        : COLOR;
    float  size         : TEXCOORD1;
    float  rotation     : TEXCOORD2;
    float4 uvRect       : TEXCOORD3;
    float3 velocity     : TEXCOORD4;
    float4 nextUvRect   : TEXCOORD5;
    float  spriteBlend  : TEXCOORD6;
};

struct ParticlePSIn
{
    float4 svPosition   : SV_POSITION;
    float2 uv           : TEXCOORD0;
    float2 localUv      : TEXCOORD1;
    float2 nextUv       : TEXCOORD2;
    float  spriteBlend  : TEXCOORD3;
    float4 color        : COLOR;
};

// 通常のParticle VSと同じ軸・回転・速度Stretchでクワッドを展開する。
ParticlePSIn VSMain(ParticleVSIn v)
{
    float3 right = float3(view[0][0], view[1][0], view[2][0]);
    float3 up    = float3(view[0][1], view[1][1], view[2][1]);
    float lengthScale = 1.0f;
    if (gRenderMode == 1)
    {
        float speed = length(v.velocity);
        if (speed > 1.0e-4f)
        {
            up = v.velocity / speed;
            float3 viewDir = normalize(cameraPos - v.center);
            right = normalize(cross(up, viewDir));
            lengthScale = max(gStretchedLengthScale + speed * gStretchedVelocityScale, 0.0f);
        }
    }
    else if (gRenderMode == 2)
    {
        right = float3(1.0f, 0.0f, 0.0f);
        up = float3(0.0f, 0.0f, 1.0f);
    }
    else if (gRenderMode == 3)
    {
        up = float3(0.0f, 1.0f, 0.0f);
        float3 viewDir = normalize(cameraPos - v.center);
        right = normalize(cross(up, viewDir));
    }

    float2 corner = v.uv * 2.0f - 1.0f;
    float s = sin(v.rotation);
    float c = cos(v.rotation);
    corner = float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);
    float3 worldPos = v.center
        + right * corner.x * v.size * 0.5f * gSizeAxisScaleX
        + up * corner.y * v.size * 0.5f * gSizeAxisScaleY * lengthScale;

    ParticlePSIn output;
    output.svPosition = mul(float4(worldPos, 1.0f), viewProjection);
    output.uv = lerp(v.uvRect.xy, v.uvRect.zw, v.uv);
    output.localUv = v.uv;
    output.nextUv = lerp(v.nextUvRect.xy, v.nextUvRect.zw, v.uv);
    output.spriteBlend = v.spriteBlend;
    output.color = v.color;
    return output;
}

// 矩形クワッドではなく、実際に見えているテクセルだけをMaskへ書く。
float4 PSMain(ParticlePSIn input) : SV_Target0
{
    const float radial = RadialMask(input.localUv);
    float alpha = radial * input.color.a;
    if ((gEffectsFlags & FBZZ_PFX_VOLUMETRIC) == 0u)
    {
        const float4 current = ResolveParticleTexel(
            gParticleTex.Sample(gSampler, input.uv), gEffectsFlags);
        const float4 next = ResolveParticleTexel(
            gParticleTex.Sample(gSampler, input.nextUv), gEffectsFlags);
        alpha *= lerp(current.a, next.a, saturate(input.spriteBlend));
    }
    if (alpha < 0.02f) discard;
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
