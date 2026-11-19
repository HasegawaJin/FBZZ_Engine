/// @file    WaterRiverObstacles.hlsli
/// @brief   岩・橋脚での接触泡、回り込み、後流の渦と切り抜き。
/// @author  Hasegawa Jin
/// @date    2026-11-18
#ifndef FBZZ_WATER_RIVER_OBSTACLES_HLSLI
#define FBZZ_WATER_RIVER_OBSTACLES_HLSLI

/// @note WaterStandardAppearance.hlsli から読み込む。
/// @note Reproject a nearby surface point and accept opaque geometry that emerges near that point in world space.
/// @see https://media.gdcvault.com/gdc08/slides/S6172i1.pdf GDC 2008, Shore and Foam, opaque depth as a contact cue.
float WaterRiverObstacleContact(float3 surfacePoint)
{
    const float4 clipPosition = mul(float4(surfacePoint, 1.0f), viewProjection);
    if (clipPosition.w <= 0.0f) return 0.0f;
    const float3 ndc = clipPosition.xyz / clipPosition.w;
    if (ndc.z <= 0.0f || ndc.z >= 1.0f || any(abs(ndc.xy) >= 1.0f)) return 0.0f;
    const float2 uv = NdcToUv(ndc.xy);
    const float depth = g_sceneDepth.SampleLevel(g_waterPointSampler, uv, 0.0f).r;
    if (depth <= ndc.z) return 0.0f;

    const float3 offset = ReconstructWorldPos(uv, depth, invViewProjection) - surfacePoint;
    const float heightSupport = smoothstep(0.025f, 0.14f, offset.y)
        * (1.0f - smoothstep(1.2f, 2.0f, offset.y));
    const float lateralSupport = 1.0f - smoothstep(0.20f, 0.62f, length(offset.xz));
    const float2 edge = min(uv, 1.0f - uv);
    return heightSupport * lateralSupport * smoothstep(0.0f, 0.025f, min(edge.x, edge.y));
}

/// @note A short bow band stays attached to the contact; the downstream trail uses the advecting foam texture.
float WaterRiverObstacleFoam(float3 surfacePoint, float2 slope, float2 flowDir, float speed, float foamNoise)
{
    const float2 bowOffset = 0.26f * flowDir;
    const float bow = 0.78f * WaterRiverObstacleContact(surfacePoint
        + float3(bowOffset.x, dot(slope, bowOffset), bowOffset.y));
    const float ages[3] = {0.24f, 0.55f, 0.91f};
    const float2 velocity = min(speed, 3.2f) * flowDir;
    float trail = 0.0f;
    [unroll] for (uint i = 0u; i < 3u; ++i) {
        const float2 upstream = -ages[i] * velocity;
        const float contact = WaterRiverObstacleContact(surfacePoint
            + float3(upstream.x, dot(slope, upstream), upstream.y));
        trail = max(trail, contact * (1.12f - 0.62f * ages[i]));
    }
    return max(bow, trail * lerp(0.32f, 1.0f, smoothstep(0.34f, 0.70f, foamNoise)));
}

struct WaterRiverObstacleFlow
{
    float2 velocity;
    float wake;
    bool excluded;
    float crestBend;
    float2 timeAcceleration; ///< @brief 後流速度の時間微分 [world XZ m/s²]。
};

static const float RIVER_WAKE_WAVE_NUMBER = (2.0f * PI) * 0.20f;
static const float RIVER_WAKE_PHASE_SPEED = 0.50f;

struct WaterRiverWakeVortex
{
    float2 velocity;
    float2 phaseDerivative;
};

/// @note Differentiate a compact stream function in coordinates scaled by the obstacle radius.
/// @see https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph2007-curlnoise.pdf Bridson et al. (2007), §2.1, curl of a scalar potential.
/// @see https://ntrs.nasa.gov/api/citations/19930092207/downloads/19930092207.pdf Roshko (1954), wake development behind circular cylinders.
WaterRiverWakeVortex WaterRiverWakeAt(float downstream, float lateral, float sine, float cosine)
{
    const float inverseReach = 1.0f / 2.92f;
    const float progress = saturate((downstream - 0.48f) * inverseReach);
    const float interval = progress * (1.0f - progress);
    const float envelope = 16.0f * interval * interval;
    const float envelopeSlope = 32.0f * inverseReach * interval * (1.0f - 2.0f * progress);

    const float rowCenter = lerp(0.96f, 0.45f, progress);
    const float centerSlope = (0.45f - 0.96f) * inverseReach;
    const float inverseWidth = 1.0f / 0.52f;
    const float2 signs = float2(1.0f, -1.0f);
    const float2 rowOffset = (lateral - rowCenter * signs) * inverseWidth;
    const float2 lobe = saturate(1.0f - rowOffset * rowOffset);
    const float2 profile = lobe * lobe;
    const float2 lateralSlope = -4.0f * inverseWidth * rowOffset * lobe;
    const float2 downstreamSlope = -centerSlope * signs * lateralSlope;

    const float2 pulse = 0.5f + 0.5f * sine * signs;
    const float2 circulation = signs * pulse * pulse;
    const float2 phaseSlope = pulse * cosine;
    const float2 phaseCurvature = 0.5f * cosine * cosine * signs - pulse * sine;
    const float2 acrossSlope = envelope * lateralSlope;
    const float2 alongSlope = envelopeSlope * profile + envelope * downstreamSlope;
    const float2 carrier = envelope * RIVER_WAKE_WAVE_NUMBER * profile;

    WaterRiverWakeVortex wake;
    wake.velocity = float2(dot(acrossSlope, circulation),
        -(dot(alongSlope, circulation) + dot(carrier, phaseSlope)));
    wake.phaseDerivative = float2(dot(acrossSlope, phaseSlope),
        -(dot(alongSlope, phaseSlope) + dot(carrier, phaseCurvature)));
    return wake;
}

/// @note Use only compact upright boxes intersecting the mean surface as local flow obstacles.
/// @see https://web.mit.edu/fluids-modules/www/potential_flows/Lectures/lec10/lecture10.pdf MIT Marine Hydrodynamics, Flow Around a Circular Cylinder.
WaterRiverObstacleFlow WaterRiverFlowAroundExclusions(WaterPSInput p, float2 velocity, float time)
{
    WaterRiverObstacleFlow result = {velocity, 0.0f, false, 0.0f, float2(0.0f, 0.0f)};
    FBZZWaterMediumQueryContext context;
    const uint owner = (uint)round(g_farOceanParams.w);
    uint firstRow, count;
    if (!FBZZ_WaterMediumHeader(context) || owner == 0u
        || !FBZZ_WaterExclusionRange(owner - 1u, context, firstRow, count)) {
        result.excluded = true;
        return result;
    }
    if (count == 0u) return result;

    const float speed = length(velocity);
    const float2 flow = velocity / max(speed, 0.1f);
    const float2 across = float2(-flow.y, flow.x);
    const float wakeSpeed = smoothstep(0.35f, 1.5f, speed);
    const float4 displaced = float4(p.worldPos, 1.0f);
    const float4 base = float4(p.baseWorldXZ.x, p.baseWorldY, p.baseWorldXZ.y, 1.0f);
    float2 correction = 0.0f;
    float2 wakeVelocity = 0.0f;
    float2 wakeAcceleration = 0.0f;
    float influenceSum = 0.0f;
    [loop] for (uint i = 0u; i < count; ++i) {
        const uint row = firstRow + i * FBZZ_WATER_MEDIUM_EXCLUSION_ROWS;
        const float4 y = FBZZ_WaterMediumRow(row + 1u);
        const float3 extent = FBZZ_WaterMediumRow(row + 3u).xyz;
        /// @note Clipping follows displaced geometry. Flow stays attached to the mean surface.
        const bool testCut = abs(dot(y, displaced)) <= extent.y;
        const float verticalScale = abs(y.y);
        const float maxTilt = 0.001f * verticalScale;
        const bool upright = verticalScale > 1.0e-6f && dot(y.xz, y.xz) <= maxTilt * maxTilt;
        const float surfaceInset = extent.y - abs(dot(y, base));
        const bool testFlow = speed > 0.1f && upright && surfaceInset > 0.0f;
        [branch] if (!testCut && !testFlow) continue;

        const float4 x = FBZZ_WaterMediumRow(row);
        const float4 z = FBZZ_WaterMediumRow(row + 2u);
        if (testCut && all(abs(float2(dot(x, displaced), dot(z, displaced))) <= extent.xz)) {
            result.excluded = true;
            return result;
        }
        if (!testFlow) continue;
        const float2 inverseScaleSq = float2(dot(x.xz, x.xz), dot(z.xz, z.xz));
        const float2 tilt = float2(x.y, z.y);
        if (any(inverseScaleSq <= 1.0e-12f) || any(tilt * tilt > inverseScaleSq * 1.0e-6f)) continue;
        const float2 scale = rsqrt(inverseScaleSq);
        const float2 halfSize = extent.xz * scale;
        if (any(halfSize < 0.12f) || any(halfSize > 3.0f)
            || max(halfSize.x, halfSize.y) > 2.0f * min(halfSize.x, halfSize.y)) continue;

        /// @note Invert the orthogonal horizontal rows, preserving yaw and the signs of both axes.
        const float2 local = float2(dot(x, base), dot(z, base)) / inverseScaleSq;
        const float2 delta = local.x * x.xz + local.y * z.xz;
        const float radiusSq = (4.0f / PI) * halfSize.x * halfSize.y;
        const float distanceSq = dot(delta, delta);
        if (distanceSq <= 1.0e-8f || distanceSq >= 3.7f * 3.7f * radiusSq) continue;
        const float inverseRadius = rsqrt(radiusSq);
        const float inverseDistance = rsqrt(distanceSq);
        const float2 radial = delta * inverseDistance;
        const float wetSupport = smoothstep(0.0f, 0.20f * verticalScale, surfaceInset);
        const float distanceInRadii = distanceSq * inverseDistance * inverseRadius;
        const float reach = 1.0f - smoothstep(2.4f, 3.7f, distanceInRadii);
        const float influence = wetSupport * reach * min(1.0f, radiusSq / distanceSq);
        correction += influence * (velocity - 2.0f * dot(velocity, radial) * radial);
        influenceSum += influence;

        const float alongFlow = dot(delta, flow);
        result.crestBend += alongFlow * influence;
        const float downstream = alongFlow * inverseRadius;
        const float lateral = dot(delta, across) * inverseRadius;
        [branch] if (wakeSpeed <= 0.0f || downstream <= -0.60f || downstream >= 3.4f
            || abs(lateral) > 1.5f) continue;

        const float wrap = (1.0f - smoothstep(0.07f, 0.31f, abs(distanceInRadii - 1.11f)))
            * smoothstep(0.57f, 0.91f, abs(lateral))
            * smoothstep(-0.60f, 0.02f, downstream) * (1.0f - smoothstep(0.18f, 0.88f, downstream));
        result.wake = max(result.wake, 0.82f * wetSupport * wrap);
        if (downstream <= 0.48f) continue;

        /// @note The two shear layers alternate at St = f * diameter / speed, with phase advecting downstream.
        /// @see https://ntrs.nasa.gov/api/citations/19970017057/downloads/19970017057.pdf#page=3 NASA TM-110339, Introduction.
        const float angularSpeed = RIVER_WAKE_WAVE_NUMBER * RIVER_WAKE_PHASE_SPEED * speed * inverseRadius;
        float shedding, phaseCosine;
        sincos(RIVER_WAKE_WAVE_NUMBER * downstream - angularSpeed * time, shedding, phaseCosine);
        const WaterRiverWakeVortex vortex = WaterRiverWakeAt(downstream, lateral, shedding, phaseCosine);
        const float vortexStrength = 0.17f * speed * wetSupport * wakeSpeed;
        wakeVelocity += vortexStrength * (vortex.velocity.x * flow + vortex.velocity.y * across);
        wakeAcceleration -= angularSpeed * vortexStrength
            * (vortex.phaseDerivative.x * flow + vortex.phaseDerivative.y * across);
        const float flank = lateral < 0.0f ? -shedding : shedding;
        const float detached = smoothstep(0.75f, 1.45f, downstream);
        const float wakeLength = smoothstep(0.48f, 1.05f, downstream) * (1.0f - smoothstep(1.8f, 3.4f, downstream));
        const float convergence = smoothstep(0.68f, 3.3f, downstream);
        const float edge = lerp(1.03f, 0.42f, convergence) + 0.15f * detached * flank;
        const float width = lerp(0.31f, 0.44f, convergence);
        const float wakeEdge = 1.0f - smoothstep(0.13f, width, abs(abs(lateral) - edge));
        const float foamStrength = 1.0f - 0.31f * detached * (1.0f - flank);
        const float foamAge = (downstream - 0.48f) / (inverseRadius * max(speed, 0.35f));
        const float decay = exp(-0.58f * foamAge);
        result.wake = max(result.wake, wetSupport * wakeLength * wakeEdge * foamStrength * decay);
    }
    const float vortexLimit = 0.40f * speed;
    const float vortexSpeedSq = dot(wakeVelocity, wakeVelocity);
    [branch] if (vortexSpeedSq > vortexLimit * vortexLimit) {
        const float inverseSpeed = rsqrt(vortexSpeedSq);
        const float2 direction = wakeVelocity * inverseSpeed;
        wakeAcceleration = (wakeAcceleration - direction * dot(wakeAcceleration, direction))
            * (vortexLimit * inverseSpeed);
        wakeVelocity = vortexLimit * direction;
    }
    result.velocity += correction / max(1.0f, influenceSum) + wakeVelocity;
    result.timeAcceleration = wakeAcceleration;
    result.crestBend /= max(1.0f, influenceSum);
    result.wake *= wakeSpeed;
    return result;
}

#endif
