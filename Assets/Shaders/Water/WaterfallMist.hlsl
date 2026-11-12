/// @file    WaterfallMist.hlsl
/// @brief   Texture-free soft spray with the existing CPU/GPU particle contract.
/// @author  Hasegawa Jin
/// @date    2026-11-06
/// @see Docs/design/particle-authoring.md Particle lifetime, depth fading and distance budgets.
#define FBZZ_PARTICLE_CUSTOM_VS
#include "Material/Effects/ParticleMaterial.hlsli"
#include "Common/Space.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Rendering/ParticleLighting.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;
    /// @note Extinction in inverse world metres. Zero selects the soft droplet sprite.
    float volumeDensity;
};

FBZZ_TEX2D(waterfallSceneDepth, TEX_DEPTH_SLOT);
FBZZ_TEX2D_T(float, waterfallShadowMap, TEX_SHADOW_SLOT);
SamplerComparisonState waterfallShadowSampler : register(SAMPLER_SHADOW);

ParticlePSIn VSMain(ParticleVSIn v)
{
    ParticlePSIn p = ParticleBillboardVS(v);
    if (volumeDensity <= 0.0f) return p;

    /// @note Rasterize a conservative front proxy. The pixel shader limits integration to the actual sphere and opaque depth.
    const float3 right = float3(view[0][0], view[1][0], view[2][0]);
    const float3 up = float3(view[0][1], view[1][1], view[2][1]);
    const float3 forward = float3(view[0][2], view[1][2], view[2][2]);
    const float3 center = mul(float4(p.center, 1.0f), view).xyz;
    const float2 uv = float2(v.uv.x, 1.0f - v.uv.y);
    const float front = max(nearZ * 1.001f, center.z - p.radius);
    const float back = max(front, center.z + p.radius);
    const float ratio = isOrthographic > 0.5f ? 1.0f : front / back;
    const float2 low = center.xy - p.radius;
    const float2 high = center.xy + p.radius;
    const float2 corner = lerp(min(low, low * ratio), max(high, high * ratio), uv);
    p.worldPos = cameraPos + right * corner.x + up * corner.y + forward * front;
    p.svPosition = mul(float4(p.worldPos, 1.0f), viewProjection);
    return p;
}

/// @see https://pbr-book.org/4ed/Volume_Scattering/Transmittance Beer transmittance along the visible interval.
float4 ShadeWaterfallVolume(ParticlePSIn p)
{
    const float3 forward = float3(view[0][2], view[1][2], view[2][2]);
    const float3 ray = isOrthographic > 0.5f ? forward : normalize(p.worldPos - cameraPos);
    const float invForward = rcp(max(dot(ray, forward), 0.0001f));
    const float proxyDepth = dot(p.worldPos - cameraPos, forward);
    const float3 sphere = p.worldPos - p.center;
    const float offset = dot(sphere, ray);
    const float radius = max(p.radius, 0.001f);
    const float discriminant = offset * offset - dot(sphere, sphere) + radius * radius;
    if (discriminant <= 0.0f) discard;
    const float extent = sqrt(discriminant);
    const float sceneNdc = gSoftParticles != 0 ? waterfallSceneDepth.Load(int3(int2(p.svPosition.xy), 0)).r : 0.0f;
    const float sceneDepth = LinearizeDepth(sceneNdc, nearZ, farZ, isOrthographic);
    const float begin = max(-offset - extent, (nearZ - proxyDepth) * invForward);
    const float end = min(-offset + extent, (sceneDepth - proxyDepth) * invForward);
    if (end <= begin) discard;
    const float centerDepth = dot(p.center - cameraPos, forward);
    const float pixels = radius * abs(projection[1][1]) * gScreenHeight
        / (isOrthographic > 0.5f ? 1.0f : max(centerDepth, nearZ));
    const uint steps = pixels >= 70.0f ? 5u : (pixels >= 18.0f ? 3u : 1u);
    const float stepLength = (end - begin) / float(steps);
    float opticalDepth = 0.0f;
    [loop] for (uint i = 0u; i < steps; ++i) {
        const float t = begin + (float(i) + 0.5f) * stepLength;
        const float3 q = (sphere + ray * t) / radius;
        const float envelope = saturate(1.0f - dot(q, q));
        const float eddies = steps == 1u ? 0.76f : 0.76f + 0.24f * sin(dot(q, float3(5.0f, 4.0f, 6.0f))
            + sin(q.x * 8.0f - q.y * 5.0f + cloudShadowTime * 0.7f));
        opticalDepth += envelope * envelope * eddies;
    }
    const float alpha = (1.0f - exp(-opticalDepth * volumeDensity * stepLength)) * p.color.a * albedo.a;
    const float3 toSun = normalize(-lightDir + float3(0.0f, -0.0001f, 0.0f));
    const float3 samplePos = p.worldPos + ray * (0.5f * (begin + end));
    float shadow = 1.0f;
    [branch] if (steps > 1u && (gEffectsFlags & FBZZ_PFX_RECEIVE_SHADOW) != 0u && lightIntensity > 0.0f)
        shadow = ComputeShadow(waterfallShadowMap, waterfallShadowSampler, samplePos,
            lightViewProjection, shadowMapTexelSize, shadowBias, toSun, toSun);
    const float phase = FBZZ_HenyeyGreenstein(dot(ray, toSun), 0.46f);
    const float3 lighting = max(ambientColor, 0.025f) + lightColor * max(lightIntensity, 0.0f) * phase * shadow;
    return FinishParticleFog(float4(albedo.rgb * p.color.rgb * lighting * alpha, alpha),
        p.svPosition.xy, p.svPosition.z, sceneNdc);
}

float4 PSMain(ParticlePSIn p) : SV_Target0
{
    if (volumeDensity > 0.0f) return ShadeWaterfallVolume(p);
    const float2 d = p.localUv * 2.0f - 1.0f;
    const float radius = dot(d, d);
    const float breakup = 0.77f + 0.23f * sin(d.x * 14.0f + sin(d.y * 11.0f));
    float alpha = exp(-radius * 3.3f) * (1.0f - smoothstep(0.35f, 1.0f, radius)) * breakup;
    const float particleDepth = LinearizeDepth(p.svPosition.z, nearZ, farZ, isOrthographic);
    if (gSoftParticles != 0) {
        const float sceneDepth = waterfallSceneDepth.Load(int3(int2(p.svPosition.xy), 0)).r;
        const float linearDepth = LinearizeDepth(sceneDepth, nearZ, farZ, isOrthographic);
        alpha *= saturate((linearDepth - particleDepth) / max(gSoftParticleFadeDistance, 0.001f));
    }
    alpha *= ParticleCameraFade(particleDepth);
    const float3 lighting = max(ambientColor, 0.025f) + lightColor * max(lightIntensity, 0.0f) * 0.25f;
    return float4(albedo.rgb * p.color.rgb * lighting, alpha * albedo.a * p.color.a);
}
