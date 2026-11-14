/// @file    ParallaxOcclusion.hlsli
/// @brief   Per-pixel height-field intersection for standard PBR materials.
/// @author  Hasegawa Jin
/// @date    2026-11-13
#ifndef FBZZ_PARALLAX_OCCLUSION_HLSLI
#define FBZZ_PARALLAX_OCCLUSION_HLSLI

struct ParallaxSurface
{
    float2 uv;
    float occlusion;
    float3 normalOffset;
};

/// @brief Sample neighbouring heights for the local slope and ambient visibility.
float4 SampleParallaxNeighbours(Texture2D heightMap, SamplerState surfaceSampler,
    float2 uv, float2 uvDx, float2 uvDy, float2 alongU, float2 alongV)
{
    return saturate(float4(
        heightMap.SampleGrad(surfaceSampler, uv + alongU, uvDx, uvDy).r,
        heightMap.SampleGrad(surfaceSampler, uv - alongU, uvDx, uvDy).r,
        heightMap.SampleGrad(surfaceSampler, uv + alongV, uvDx, uvDy).r,
        heightMap.SampleGrad(surfaceSampler, uv - alongV, uvDx, uvDy).r));
}

/// @param depth Maximum recess in world metres. The height map's linear red channel is white at the mesh surface.
/// @note Position and tiled UV derivatives are taken before divergent flow. The UV Jacobian handles mirrored UVs and non-uniform scale.
/// @see https://advances.realtimerendering.com/s2006/Tatarchuk-POM.pdf Height Field Profile: Ray Intersection.
ParallaxSurface TraceParallaxOcclusion(uint heightIndex, SamplerState surfaceSampler,
    ParallaxSurface surface, float2 uvDx, float2 uvDy, float3 worldDx, float3 worldDy,
    float3 towardCamera, float depth)
{
    const float3 plane = cross(worldDx, worldDy);
    const float areaSquared = dot(plane, plane);
    if (areaSquared <= 1e-24f) return surface;
    const float inverseArea = rsqrt(areaSquared);
    const float3 planeNormal = plane * inverseArea;
    const float viewAngle = abs(dot(planeNormal, towardCamera));
    const float3 pixelX = cross(worldDy, planeNormal) * inverseArea;
    const float3 pixelY = cross(planeNormal, worldDx) * inverseArea;
    const float reliefDepth = min(depth, 0.2f);
    const float coverage = reliefDepth * sqrt(max(dot(pixelX, pixelX), dot(pixelY, pixelY)));
    const float distanceFade = smoothstep(0.25f, 1.25f, coverage);
    if (distanceFade <= 0.0f) return surface;

    /// @note The tiled UV gradients are in UV per world metre, including mirrored charts and nonuniform scale.
    const float3 gradU = uvDx.x * pixelX + uvDy.x * pixelY;
    const float3 gradV = uvDx.y * pixelX + uvDy.y * pixelY;
    const float3 uDirection = SafeNormalize(gradU, SafeNormalize(gradV, float3(0, 0, 0)));
    const float3 vDirection = cross(planeNormal, uDirection);
    const float2 alongU = 0.5f * reliefDepth * float2(dot(gradU, uDirection), dot(gradV, uDirection));
    const float2 alongV = 0.5f * reliefDepth * float2(dot(gradU, vDirection), dot(gradV, vDirection));
    if (max(dot(alongU, alongU), dot(alongV, alongV)) <= 1e-12f) return surface;

    const float2 viewSlope = float2(dot(gradU, towardCamera), dot(gradV, towardCamera));
    float2 rayOffset = -reliefDepth * viewSlope / max(viewAngle, 0.15f);
    rayOffset *= min(1.0f, 0.25f / max(length(rayOffset), 1e-6f));
    const float rayFade = distanceFade * smoothstep(0.04f, 0.15f, viewAngle);
    const float2 baseUv = surface.uv;
    Texture2D heightMap = BindlessTexture2D(heightIndex);
    float hitHeight = saturate(heightMap.SampleGrad(surfaceSampler, baseUv, uvDx, uvDy).r);
    [branch]
    if (hitHeight < 1.0f && rayFade > 0.0f && dot(rayOffset, rayOffset) > 1e-12f)
    {
        const int stepCount = (int)ceil(lerp(8.0f, 24.0f, saturate(1.0f - viewAngle)));
        const float stepSize = rcp((float)stepCount);
        float previousGap = hitHeight - 1.0f;
        [loop]
        for (int step = 1; step <= stepCount; ++step)
        {
            const float rayDepth = min((float)step * stepSize, 1.0f);
            const float sampleHeight = saturate(heightMap.SampleGrad(surfaceSampler,
                baseUv + rayOffset * rayDepth, uvDx, uvDy).r);
            const float gap = rayDepth + sampleHeight - 1.0f;
            if (gap >= 0.0f || step == stepCount)
            {
                const float hitDepth = rayDepth - stepSize * saturate(gap / max(gap - previousGap, 1e-6f));
                surface.uv += rayOffset * (hitDepth * rayFade);
                hitHeight = saturate(heightMap.SampleGrad(surfaceSampler, surface.uv, uvDx, uvDy).r);
                break;
            }
            previousGap = gap;
        }
    }

    const float4 neighbours = SampleParallaxNeighbours(heightMap, surfaceSampler,
        surface.uv, uvDx, uvDy, alongU, alongV);
    const float4 horizon = saturate((neighbours - hitHeight) * 2.0f);
    surface.occlusion = 1.0f - (0.75f * distanceFade) * dot(horizon, float4(0.25f, 0.25f, 0.25f, 0.25f));
    /// @note The sample pair spans reliefDepth metres, which cancels the height-to-metres multiplier in the slope.
    surface.normalOffset = ((neighbours.y - neighbours.x) * uDirection
        + (neighbours.w - neighbours.z) * vDirection) * distanceFade;
    return surface;
}

/// @note Slot 5 is the height map in the standard material contract. Do not resolve its descriptor before checking the mask.
static const uint FBZZ_PARALLAX_HEIGHT_SLOT = 5u;

ParallaxSurface ResolveParallaxSurface(uint heightIndex, SamplerState surfaceSampler,
    float2 uv, float2 uvDx, float2 uvDy, float3 worldPos, float3 worldDx, float3 worldDy,
    float depth, uint materialTextureMask)
{
    ParallaxSurface surface = {uv, 1.0f, float3(0, 0, 0)};
    [branch]
    if (depth > 0.0f && (materialTextureMask & (1u << FBZZ_PARALLAX_HEIGHT_SLOT)) && IsBindlessValid(heightIndex))
    {
        const float3 viewRay = isOrthographic > 0.5f
            ? -float3(view[0][2], view[1][2], view[2][2]) : cameraPos - worldPos;
        const float3 towardCamera = SafeNormalize(viewRay, float3(0.0f, 0.0f, 0.0f));
        return TraceParallaxOcclusion(heightIndex, surfaceSampler, surface, uvDx, uvDy,
            worldDx, worldDy, towardCamera, depth);
    }
    return surface;
}

float3 ApplyParallaxSurfaceNormal(float3 normal, ParallaxSurface surface, float authoredNormalWeight)
{
    const float weight = lerp(0.75f, 0.25f, saturate(authoredNormalWeight));
    const float3 slope = surface.normalOffset - normal * dot(normal, surface.normalOffset);
    return any(slope != 0.0f) ? SafeNormalize(normal + slope * weight, normal) : normal;
}

#endif
