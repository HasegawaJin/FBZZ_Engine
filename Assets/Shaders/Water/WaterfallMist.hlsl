/// @file    WaterfallMist.hlsl
/// @brief   Texture-free soft spray with the existing CPU/GPU particle contract.
/// @author  Hasegawa Jin
/// @date    2026-11-06
/// @see Docs/design/particle-authoring.md Particle lifetime, depth fading and distance budgets.
#include "Material/Effects/ParticleMaterial.hlsli"
#include "Common/Space.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;
};

FBZZ_TEX2D(waterfallSceneDepth, TEX_DEPTH_SLOT);

float4 PSMain(ParticlePSIn p) : SV_Target0
{
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
