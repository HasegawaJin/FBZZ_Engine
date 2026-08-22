// FBZZ Engine
// Debug/SelectionMaskParticleGPU.hlsl | fbzz::renderer
// GPUパーティクルの可視ピクセルをSelection Outline用マスクへ合成する

#include "Common/Binding.hlsli"
#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/ParticleCommon.hlsli"

struct GpuParticle
{
    float3 position;
    float size;
    float3 velocity;
    float age;
    float4 color;
    float lifetime;
    float rotation;
    float angularVelocity;
    float spriteSeed;
    float4 uvRect;
    float3 colorScale;
    float colorScalePad;
};

StructuredBuffer<GpuParticle> gParticles : register(SB_GPU_PARTICLES);
StructuredBuffer<uint2> gSortedParticles : register(SB_GPU_SORT);
Texture2D gParticleTex : register(TEX_ALBEDO);
SamplerState gSampler : register(SAMPLER_DEFAULT);

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
    // 以降は読まないが、b2 のレイアウトは GeometryPasses.hpp の ParticleRenderCB が正本。
    float gSmokeWrap;
    float gSmokeTransmission;
    float4 gTintColor;
    float gSmokeBackScatterPower;
    float gDistortionChromatic;
    float gParticlePad1;
    float gParticlePad2;
};

struct PsIn
{
    float4 svPosition : SV_POSITION;
    float2 uv : TEXCOORD0;
    float2 localUv : TEXCOORD1;
    float4 color : COLOR;
};

static const float2 QUAD_CORNERS[6] =
{
    float2(-0.5f,  0.5f), float2( 0.5f,  0.5f), float2(-0.5f, -0.5f),
    float2( 0.5f,  0.5f), float2( 0.5f, -0.5f), float2(-0.5f, -0.5f),
};

static const float2 QUAD_UVS[6] =
{
    float2(0.0f, 0.0f), float2(1.0f, 0.0f), float2(0.0f, 1.0f),
    float2(1.0f, 0.0f), float2(1.0f, 1.0f), float2(0.0f, 1.0f),
};

// ParticleGPU.hlslと同じStructuredBuffer・ソートindex・ビルボード展開を使う。
PsIn VSMain(uint vertexId : SV_VertexID)
{
    const uint slot = vertexId / 6;
    const uint cornerIndex = vertexId % 6;
    const uint particleIndex = gGpuSortEnabled != 0u
        ? gSortedParticles[slot].y : slot;

    PsIn output;
    if (gMaxParticles > 0 && particleIndex >= gMaxParticles)
    {
        output.svPosition = float4(0.0f, 0.0f, -2.0f, 1.0f);
        output.uv = 0.0f;
        output.localUv = 0.0f;
        output.color = 0.0f;
        return output;
    }
    const GpuParticle particle = gParticles[particleIndex];
    if (particle.age >= particle.lifetime)
    {
        output.svPosition = float4(0.0f, 0.0f, -2.0f, 1.0f);
        output.uv = 0.0f;
        output.localUv = 0.0f;
        output.color = 0.0f;
        return output;
    }

    float3 right = float3(view[0][0], view[1][0], view[2][0]);
    float3 up = float3(view[0][1], view[1][1], view[2][1]);
    float lengthScale = 1.0f;
    if (gRenderMode == 1)
    {
        const float speed = length(particle.velocity);
        if (speed > 1.0e-4f)
        {
            up = particle.velocity / speed;
            const float3 viewDirection = normalize(cameraPos - particle.position);
            right = normalize(cross(up, viewDirection));
            lengthScale = max(
                gStretchedLengthScale + speed * gStretchedVelocityScale, 0.0f);
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
        const float3 viewDirection = normalize(cameraPos - particle.position);
        right = normalize(cross(up, viewDirection));
    }

    const float2 localUv = QUAD_UVS[cornerIndex];
    float2 corner = QUAD_CORNERS[cornerIndex];
    const float sine = sin(particle.rotation);
    const float cosine = cos(particle.rotation);
    corner = float2(corner.x * cosine - corner.y * sine,
                    corner.x * sine + corner.y * cosine);
    const float3 worldPosition = particle.position
        + right * corner.x * particle.size * gSizeAxisScaleX
        + up * corner.y * particle.size * gSizeAxisScaleY * lengthScale;

    output.svPosition = mul(float4(worldPosition, 1.0f), viewProjection);
    output.uv = lerp(particle.uvRect.xy, particle.uvRect.zw, localUv);
    output.localUv = localUv;
    output.color = particle.color;
    return output;
}

float4 PSMain(PsIn input) : SV_Target0
{
    float alpha = RadialMask(input.localUv) * input.color.a;
    if ((gEffectsFlags & FBZZ_PFX_VOLUMETRIC) == 0u)
        alpha *= ResolveParticleTexel(
            gParticleTex.Sample(gSampler, input.uv), gEffectsFlags).a;
    if (alpha < 0.02f) discard;
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
