/// @file    RayHybridDiffuseGI.cs.hlsl
/// @brief   Production primary/secondary diffuse response and environment accounting GPU regression.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#define CSMain ProductionReflectionEntry
#include "RayTracing/RayReflection.cs.hlsl"
#undef CSMain

cbuffer TestPointConstants : register(b1)
{
    float4 testPositionMetallic;
    float4 testRoughnessAoEmissionMedium;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 pixel : SV_DispatchThreadID)
{
    if (pixel.y || pixel.x >= 4u) return;
    SurfaceHit hit = (SurfaceHit)0;
    hit.position = testPositionMetallic.xyz;
    hit.normal = hit.geometricNormal = float3(0, 0, 1);
    hit.surface.baseColor = 1;
    hit.surface.metallic = testPositionMetallic.w;
    hit.surface.roughness = testRoughnessAoEmissionMedium.x;
    hit.surface.emission = testRoughnessAoEmissionMedium.z.xxx;
    hit.surface.supported = 1u;
    hit.ambientOcclusion = testRoughnessAoEmissionMedium.y;
    hit.inMedium = (uint)testRoughnessAoEmissionMedium.w;
    hit.mediumAttenuationColor = float3(0.5f, 1, 0);
    hit.mediumAttenuationDistance = 1;
    float3 view = hit.normal;
    float3 value = 0;
    uint rng = 0x7135u;
    if (pixel.x == 0u) {
        TextureCube irradiance = ResourceDescriptorHeap[FbzzPixelSlot(16)];
        DiffuseGI gi = FBZZ_DiffuseIrradianceFromSky(hit.position, hit.normal,
            irradiance.SampleLevel(linearClamp, hit.normal, 0).rgb, linearClamp);
        TextureCube prefilter = ResourceDescriptorHeap[FbzzPixelSlot(17)];
        Texture2D<float4> brdf = ResourceDescriptorHeap[FbzzPixelSlot(18)];
        value = EvaluateIBLFromIrradiance(hit.normal, view, hit.surface.baseColor.rgb, hit.surface.metallic,
            hit.surface.roughness, hit.ambientOcclusion, gi.diffuse, gi.specularOcclusion, prefilter, brdf,
            0, iblDiffuseScale, iblSpecularScale, linearClamp, linearClamp) * max(iblIntensity, 0);
    } else if (pixel.x == 1u) value = SharedDiffuseIndirect(hit, view);
    else if (pixel.x == 2u) value = HitRadiance(hit, view, rng);
    else value = RayEnvironmentLighting(hit, view, rng);
    RWTexture2D<float4> output = ResourceDescriptorHeap[FbzzUavSlot(0)];
    output[pixel.xy] = float4(all(isfinite(value)) ? value : 0, all(isfinite(value)) ? 1 : 0);
}
