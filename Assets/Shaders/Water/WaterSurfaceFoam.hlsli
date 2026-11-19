/// @file    WaterSurfaceFoam.hlsli
/// @brief   岸と浅瀬に現れる泡の被覆と打ち上げ波。
/// @author  Hasegawa Jin
/// @date    2026-11-18
#ifndef FBZZ_WATER_SURFACE_FOAM_HLSLI
#define FBZZ_WATER_SURFACE_FOAM_HLSLI

/// @note WaterStandardAppearance.hlsli から読み込む。
float WaterHorizonBreakingStrength(float meanDepth, float crestRise)
{
    return smoothstep(0.42f, 0.85f, 2.0f * crestRise / max(meanDepth, 0.05f));
}

/// @brief Move swash foam across the local depth contour with the signed wave height.
/// @note The first-order contour displacement solves depthGradient · offset = -waveHeight.
/// @note Regularize flat terrain and cap the texture motion across unresolved atlas cells.
/// @see https://pubs.usgs.gov/publication/70030520 Stockdon et al. (2006), swash excursion and foreshore slope.
float2 WaterHorizonSwashOffset(float2 worldDX, float2 worldDY, float2 depthDerivative, float waveHeight)
{
    const float determinant = worldDX.x * worldDY.y - worldDX.y * worldDY.x;
    if (abs(determinant) <= max(1.0e-10f, 0.005f * length(worldDX) * length(worldDY))) return 0.0f;

    const float2 gradient = float2(worldDY.y * depthDerivative.x - worldDX.y * depthDerivative.y,
        worldDX.x * depthDerivative.y - worldDY.x * depthDerivative.x) / determinant;
    const float minimumSlopeSquared = 0.0025f;
    const float2 offset = -gradient * (waveHeight / (dot(gradient, gradient) + minimumSlopeSquared));
    return offset * min(1.0f, 2.0f / max(length(offset), 1.0e-5f));
}

/// @brief Appearance coverage for the wet edge, breaking crests and foam on retreating water.
/// @note Depth is world-vertical. The lower ramp and fwidth soften the moving wet edge at pixel scale.
/// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models GPU Gems §1.3.1, shoreline depth and wave behavior.
/// @see https://liris.cnrs.fr/Documents/Liris-5812.pdf Dupuy and Bruneton (2012), wave compression as a foam-generation cue.
/// @see https://doi.org/10.1029/2012JC008147 Callaghan et al. (2012), decay of residual foam after breaking.
float WaterHorizonShoreFoam(float meanDepth, float verticalDepth, float depthFootprint,
    float crestRise, float jacobian, float threshold, float fade, float washPattern, float4 waveHeights)
{
    const float wetRamp = max(depthFootprint, 0.025f);
    const float wet = smoothstep(0.0f, wetRamp, verticalDepth);
    const float breaking = WaterHorizonBreakingStrength(meanDepth, crestRise);
    /// @note The bright rim widens with the incoming crest and thins to a broken line as the water retreats.
    const float washWidth = max(0.35f * threshold, 0.05f) * lerp(0.45f, 1.55f, washPattern)
        * lerp(0.8f, 1.2f, breaking);
    const float washCoverage = lerp(0.16f, 0.68f, washPattern) * lerp(0.92f, 1.0f, breaking);
    const float wash = washCoverage * (1.0f - smoothstep(washWidth, washWidth + max(fade, depthFootprint), verticalDepth));
    const float bandEnd = threshold + max(fade, depthFootprint);
    const float shallowBand = 1.0f - smoothstep(threshold, bandEnd, verticalDepth);
    const float compression = smoothstep(0.02f, 0.25f, 1.0f - jacobian);
    const float currentFoam = max(wash, breaking * shallowBand * lerp(0.65f, 1.0f, compression));
    float trail = 0.0f;
    [branch]
    if (verticalDepth > 0.0f && verticalDepth < bandEnd && g_crestFoamParams.w > 0.001f) {
        [unroll]
        for (int tap = 0; tap < WATER_FOAM_TRAIL_TAPS; ++tap) {
            const float fall = waveHeights[tap] - waveHeights.w;
            /// @note Keep the current bottom estimate fixed while reconstructing the earlier water column.
            const float pastDepth = verticalDepth + fall;
            const float pastBand = 1.0f - smoothstep(threshold, bandEnd, pastDepth);
            const float retreat = smoothstep(wetRamp, wetRamp + 0.12f, fall);
            const float pastBreaker = WaterHorizonBreakingStrength(meanDepth, waveHeights[tap]) * pastBand * retreat;
            trail = max(trail, pastBreaker * exp(-0.5f * (float)(tap + 1)));
        }
        trail *= lerp(0.25f, 0.95f, smoothstep(0.28f, 0.72f, washPattern));
    }
    return wet * (currentFoam + (1.0f - currentFoam) * trail);
}

/// @brief Resolve small terrain contacts from opaque depth while keeping distant intersections tied to the terrain atlas.
/// @note View depth is converted to a world-space displacement along the original camera ray. Far lateral hits can belong to a different bank.
/// @see https://media.gdcvault.com/gdc08/slides/S6172i1.pdf GDC 2008, Shore and Foam, surface-to-world depth as a contact signal.
float WaterHorizonContactDepth(float terrainDepth, float3 toCamera, float viewRayZ, float sceneThickness, float hasBottom)
{
    const float distance = sceneThickness / max(viewRayZ, 0.05f);
    const float lateralDistance = distance * length(toCamera.xz);
    const float confidence = hasBottom * smoothstep(0.10f, 0.35f, toCamera.y)
        * (1.0f - smoothstep(1.0f, 5.0f, lateralDistance));
    return lerp(terrainDepth, distance * max(toCamera.y, 0.0f), confidence);
}

/// @note 白い泡は薄い水膜で消え、水に浸かった岸のごく近くに残る。
float WaterSplineShoreCoverage(float depth, float threshold, float fade)
{
    return smoothstep(0.02f, 0.10f, depth) * (1.0f - smoothstep(threshold, threshold + fade, depth));
}

#endif
