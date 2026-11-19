/// @file    WaterRiverWaves.hlsli
/// @brief   河川の進行波と段差に固定される波の頂点評価。
/// @author  Hasegawa Jin
/// @date    2026-11-18
#ifndef FBZZ_WATER_RIVER_WAVES_HLSLI
#define FBZZ_WATER_RIVER_WAVES_HLSLI

/// @note WaterSurface.hlsli から読み込む。
/// @note Bend the traveling phase locally using the dipole term of a circular obstacle, with bounded overlap.
/// @see https://web.mit.edu/fluids-modules/www/potential_flows/Lectures/lec10/lecture10.pdf MIT Marine Hydrodynamics, Flow Around a Circular Cylinder.
float2 WaterRiverWaveRockWarp(float3 basePos, float2 flowDirection)
{
    const uint count = min((uint)g_riverWaveParams.y, (uint)WATER_RIVER_WAVE_ROCK_COUNT);
    float offset = 0.0f;
    float rockWeight = 0.0f;
    float weightSum = 0.0f;
    [loop] for (uint i = 0u; i < count; ++i) {
        const float4 rock = g_riverWaveRocks[2u * i];
        const float2 delta = basePos.xz - rock.xz;
        const float radiusSq = rock.w * rock.w;
        const float distanceSq = dot(delta, delta);
        if (distanceSq >= 3.7f * 3.7f * radiusSq) continue;
        const float inset = g_riverWaveRocks[2u * i + 1u].x - abs(basePos.y - rock.y);
        if (inset <= 0.0f) continue;
        const float reach = 1.0f - smoothstep(2.1f * 2.1f, 3.7f * 3.7f, distanceSq / radiusSq);
        const float influence = smoothstep(0.0f, 0.20f, inset) * reach
            * radiusSq / max(distanceSq, radiusSq);
        offset += dot(delta, flowDirection) * influence;
        rockWeight = max(rockWeight, influence);
        weightSum += influence;
    }
    return float2(offset / max(weightSum, 1.0f), rockWeight);
}

static const float RIVER_SHORT_WAVE_PERIOD = 1.15f;
static const float RIVER_SHORT_WAVE_AMPLITUDE = 0.26f; ///< @note 基本波に対する短波の振幅比。
static const float RIVER_DISPLACEMENT_LIMIT = 0.15f; ///< @note 描画専用の河川波を収める既存の垂直境界 [m]。
static const float RIVER_STANDING_MAX_FROUDE_SQ = 5.76f;
static const uint RIVER_STANDING_FOAM_TAPS = 4u; ///< @note 固定波と共有した 3 点に追加する上流水深の上限。

float WaterRiverStandingFoamDuration(float lifetime)
{
    return min(2.6f * lifetime, 4.0f);
}

/// @param atlasRate 流れに沿って進む 1 m あたりの水深テクセル数。
float WaterRiverStandingSpacing(float2 meshCellSize, float atlasRate)
{
    return clamp(max(max(meshCellSize.x, meshCellSize.y), 1.15f / max(atlasRate, 0.01f)), 0.55f, 6.0f);
}

/// @return x=固定波の強さ、y=上流の泡を探すための波頭の強さ、z=水深に応じた高さの尺度 [m]。
/// @see https://agupubs.onlinelibrary.wiley.com/doi/10.1029/2009WR007731 Magirl et al. (2009), §3.6, stationary rapids waves over depth perturbations.
float3 WaterRiverStandingBed(float3 depths, float coverage, float speedSq, float seamWeight)
{
    const float relief = depths.x - depths.z;
    const float shelfDepth = min(depths.x, min(depths.y, depths.z));
    [branch] if (abs(relief) <= 0.045f || coverage <= 0.74f || shelfDepth <= 0.06f || depths.x >= 1.8f) return 0.0f;
    const float bend = sign(relief) * ((depths.x - depths.y) - (depths.y - depths.z));
    const float froudeSq = speedSq / (9.81f * shelfDepth);
    const float nearCritical = smoothstep(0.30f, 0.88f, froudeSq) * (1.0f - smoothstep(2.89f, RIVER_STANDING_MAX_FROUDE_SQ, froudeSq));
    const float wetShelf = smoothstep(0.06f, 0.20f, shelfDepth) * (1.0f - smoothstep(0.70f, 1.45f, shelfDepth));
    const float strength = seamWeight * nearCritical * wetShelf * smoothstep(0.74f, 0.97f, coverage)
        * smoothstep(0.045f, 0.21f, abs(relief)) * (1.0f - smoothstep(1.25f, 1.8f, depths.x));
    return float3(strength, strength * smoothstep(0.025f, 0.19f, bend), min(0.145f, 0.55f * shelfDepth));
}

/// @param phase 波頭を原点とし、下流が正。距離は段差の探索間隔で正規化する。
/// @return x=広い断面、y=下流面を狭める差分、zw=波頭と下流側の谷の包絡。
float4 WaterRiverStandingSection(float phase)
{
    const float rise = smoothstep(-0.74f, 0.0f, phase);
    const float broad = rise * (1.0f - smoothstep(0.0f, 0.50f, phase));
    const float lip = rise * (1.0f - smoothstep(0.0f, 0.29f, phase));
    const float trough = smoothstep(0.35f, 0.76f, phase) * (1.0f - smoothstep(0.79f, 1.21f, phase));
    return float4(broad - 0.26f * trough, lip - broad, lip, trough);
}

float WaterRiverStandingLipMesh(float spacing, float2 meshCellSize)
{
    return WaveMeshFade(1.16f * spacing, meshCellSize);
}

float WaterRiverStandingDepthCrossing(float3 depths)
{
    const float midDepth = (depths.x + depths.z) * 0.5f;
    const bool nearCrossing = (depths.x - depths.z) * (depths.y - midDepth) <= 0.0f;
    const float fromDepth = nearCrossing ? depths.x : depths.y;
    const float toDepth = nearCrossing ? depths.y : depths.z;
    return (nearCrossing ? 0.0f : 1.0f) + saturate((midDepth - fromDepth) / (toDepth - fromDepth)) - 0.74f;
}

/// @return x=波頭を原点とする断面座標、y=段差の局所的な確度。
float2 WaterRiverStandingPhase(float2 baseUv, float2 uvStep, float3 depths, float authoredDepth)
{
    const float relief = depths.x - depths.z;
    const float orientation = sign(relief);
    const float midDepth = (depths.x + depths.z) * 0.5f;
    const bool nearCrossing = orientation * (depths.y - midDepth) <= 0.0f;
    float from = nearCrossing ? 0.0f : 1.0f;
    float to = from + 1.0f;
    float fromDepth = nearCrossing ? depths.x : depths.y;
    float toDepth = nearCrossing ? depths.y : depths.z;
    float coverage = 1.0f;
    [unroll] for (uint probe = 0u; probe < 2u; ++probe) {
        const float midpoint = (from + to) * 0.5f;
        const float2 bed = SampleWaterTerrainDepth(baseUv + midpoint * uvStep);
        const float depth = clamp(min(authoredDepth, bed.y), min(fromDepth, toDepth), max(fromDepth, toDepth));
        coverage = min(coverage, bed.x * smoothstep(0.015f, 0.06f, bed.y));
        if (orientation * (depth - midDepth) > 0.0f) {
            from = midpoint;
            fromDepth = depth;
        } else {
            to = midpoint;
            toDepth = depth;
        }
    }
    const float contrast = orientation * (fromDepth - toDepth);
    const float t = contrast > 1.0e-5f ? saturate(orientation * (fromDepth - midDepth) / contrast) : 0.5f;
    /// @note A linear bed ramp has unit slope ratio. Require localized relief before shaping a crest.
    const float slopeRatio = 8.0f * contrast / max(abs(relief), 0.045f);
    const float bend = abs((depths.x - depths.y) - (depths.y - depths.z));
    const float stepWeight = max(smoothstep(1.15f, 2.05f, slopeRatio), smoothstep(0.025f, 0.15f, bend));
    return float2(lerp(from, to, t) - 0.74f, stepWeight * smoothstep(0.74f, 0.97f, coverage));
}

/// @param foamSource x=供給量、y=補間用に供給量を掛けた流下距離 [m]。
/// @param sectionData x=強さ、y=探索間隔 [m]、z=強さを適用した高さ [m]、w=峰から下流への符号付き距離 [m]。
/// @return x=メッシュの高さ [m]、y=波頭の強さ。
float2 WaterRiverStandingWave(float2 worldXZ, float2 velocity, float2 transportVelocity, float authoredDepth,
    float2 terrainDepth, float2 meshCellSize, float seamWeight, bool allowFoamTrace,
    out float2 foamSource, out float4 sectionData)
{
    foamSource = 0.0f;
    sectionData = 0.0f;
    const float speedSq = dot(velocity, velocity);
    const float depth = min(authoredDepth, terrainDepth.y);
    const bool standingVisible = depth < 1.8f && speedSq < (9.81f * RIVER_STANDING_MAX_FROUDE_SQ) * depth;
#if defined(FBZZ_WATER_GEOMETRY_ONLY) || defined(FBZZ_WATER_MEDIUM_CAPTURE)
    const bool traceFoam = false;
#else
    const bool traceFoam = allowFoamTrace && g_foamParams.z > 0.0f && g_crestFoamParams.w > 0.001f;
#endif
    /// @note 段差波は流れで発生するため、進行波の振幅は使わない。呼び出し元は河川波が有効な水面に限る。
    [branch] if (terrainDepth.x <= 0.74f || depth <= 0.06f || seamWeight <= 0.0f || speedSq <= 0.25f
        || (!standingVisible && !traceFoam)) return 0.0f;

    /// @note Use the authored current and the undeformed position so wakes and moving waves cannot move the crest.
    const float2 baseUv = WaterTerrainUvAtWorldXZ(worldXZ);
    const float2 upstreamUvPerMeter = WaterTerrainUvAtWorldXZ(worldXZ - velocity * rsqrt(speedSq)) - baseUv;
    const float atlasRate = length(upstreamUvPerMeter * g_terrainInfoParams.zw);
    const float spacing = WaterRiverStandingSpacing(meshCellSize, atlasRate);
    const float2 uvStep = upstreamUvPerMeter * spacing;
    const float2 nearUv = baseUv + uvStep;
    float2 nearBed = SampleWaterTerrainDepth(nearUv);
    [branch] if (nearBed.x <= 0.74f || nearBed.y <= 0.06f) return 0.0f;
    float2 farBed = SampleWaterTerrainDepth(nearUv + uvStep);
    [branch] if (farBed.x <= 0.74f || farBed.y <= 0.06f) return 0.0f;
    const float3 depths = min(float3(depth, nearBed.y, farBed.y), authoredDepth);
    const float3 bed = WaterRiverStandingBed(depths,
        min(terrainDepth.x, min(nearBed.x, farBed.x)), speedSq, seamWeight);
    float2 profile = 0.0f;
    [branch] if (bed.x > 1.0e-4f) {
        const float2 phase = WaterRiverStandingPhase(baseUv, uvStep, depths, authoredDepth);
        const float strength = bed.x * phase.y;
        const float amplitude = strength * bed.z;
        const float4 section = WaterRiverStandingSection(phase.x);
        const float meshLip = WaterRiverStandingLipMesh(spacing, meshCellSize);
        profile = float2(amplitude * (section.x + meshLip * section.y), strength * section.z);
        sectionData = float4(strength, spacing, amplitude, spacing * phase.x);
    }
    [branch] if (!traceFoam) return profile;

    /// @note Move the same three-point stencil upstream one sample at a time. Stop at dry or missing cells.
    const float searchSpeed = min(5.0f, max(sqrt(speedSq), 1.7f * length(transportVelocity)));
    const float reach = searchSpeed * WaterRiverStandingFoamDuration(g_crestFoamParams.w);
    float weightSum = profile.y;
    float distanceSum = profile.y * max(sectionData.w, 0.0f);
    float strength = profile.y;
    [loop] for (uint tap = 1u; tap <= RIVER_STANDING_FOAM_TAPS; ++tap) {
        const float sampleDistance = spacing * tap;
        if (sampleDistance > reach + spacing) break;
        const float2 upstreamBed = SampleWaterTerrainDepth(baseUv + (tap + 2u) * uvStep);
        if (upstreamBed.x <= 0.74f || upstreamBed.y <= 0.06f) break;
        const float3 upstreamDepths = min(float3(nearBed.y, farBed.y, upstreamBed.y), authoredDepth);
        const float coverage = min(nearBed.x, min(farBed.x, upstreamBed.x));
        const float emission = WaterRiverStandingBed(upstreamDepths, coverage, speedSq, seamWeight).y;
        if (emission > 0.0f) {
            /// @note 発生位置を探索点の中央と見なすと、粗い地形で泡が数 m 分早く消える。
            const float distance = spacing * (tap + WaterRiverStandingDepthCrossing(upstreamDepths));
            const float support = 1.0f - smoothstep(reach, reach + spacing, distance);
            const float weight = emission * support;
            weightSum += weight;
            distanceSum += weight * distance;
            strength = max(strength, weight);
        }
        nearBed = farBed;
        farBed = upstreamBed;
    }
    foamSource = float2(strength, strength * distanceSum / max(weightSum, 1.0e-5f));
    return profile;
}

/// @see https://web.mit.edu/fluids-modules/waves/www/material/chap-3.pdf Akylas and Mei, Eq. (1.21), finite-depth gravity-wave dispersion.
float2 WaterRiverPhaseSpeeds(float speed, float depth)
{
    const float2 waveNumber = (2.0f * PI) / max(g_riverWaveParams.zw, 0.1f);
    return min(speed, 5.0f) + sqrt((9.81f / waveNumber) * tanh(min(depth * waveNumber, 8.0f)));
}

/// @return x=mesh displacement [m], yzw=unresolved fundamental, harmonic and shorter band's amplitudes [m].
float4 WaterRiverWaveVertex(WaterVSInput v, float3 basePos, float2 velocity, WaterWaveDepthState depthState,
    float2 meshCellSize, float time, float seamWeight, out float2 travelTimes)
{
    travelTimes = v.riverTravelTimes;
    const float period = g_riverWaveParams.x;
    const float speed = length(velocity);
    const float authoredSpeed = length(v.splineData.yz);
    const float authoredDepth = max(v.splineData.x, 0.0f);
    const float depth = lerp(authoredDepth, min(authoredDepth, depthState.terrainDepth.y), saturate(depthState.terrainDepth.x));
    if (period <= 0.0f || authoredSpeed <= 0.18f || speed <= 0.18f || depth <= 0.02f) return 0.0f;
    const float waveEnergy = smoothstep(0.01f, 0.05f, g_reflectParams.w);
    if (waveEnergy <= 0.0f || seamWeight <= 0.0f) return 0.0f;
    const float2 phaseSpeed = WaterRiverPhaseSpeeds(authoredSpeed, depth);
    const float2 inverseSpeed = rcp(max(phaseSpeed, 0.25f));
    const float2 rockWarp = WaterRiverWaveRockWarp(basePos, velocity / speed);
    /// @note Limit local bathymetry to a bounded spatial delay so depth variation never distorts phase as time grows.
    float2 terrainDelay = 0.0f;
    if (depth < authoredDepth) {
        const float2 authoredInverse = rcp(max(WaterRiverPhaseSpeeds(authoredSpeed, authoredDepth), 0.25f));
        const float2 reach = clamp(0.45f * g_riverWaveParams.zw, 0.55f, 2.4f);
        terrainDelay = min(reach * (inverseSpeed - authoredInverse), 0.27f * float2(period, RIVER_SHORT_WAVE_PERIOD));
    }
    travelTimes += saturate(authoredSpeed / 0.4f) * (terrainDelay + 0.70f * rockWarp.x * inverseSpeed);
    const float shallow = max(1.0f - smoothstep(0.20f, 1.4f, depth), 1.0f - depthState.horizontal.x);
    const float wet = smoothstep(0.02f, 0.20f, depth) * smoothstep(0.01f, 0.21f, depthState.horizontal.x);
    const float moving = smoothstep(0.18f, 0.92f, speed);
    const float amplitude = min(0.32f * depth, min(RIVER_DISPLACEMENT_LIMIT, 0.062f + 0.052f * shallow + 0.036f * rockWarp.y))
        * wet * moving * seamWeight * waveEnergy;
    const float harmonic = lerp(0.09f, 0.20f, max(shallow, rockWarp.y));
    const float fundamentalAmplitude = amplitude / (1.0f + harmonic);
    const float harmonicAmplitude = harmonic * fundamentalAmplitude;
    const float wavelength = period * phaseSpeed.x;
    const float mainMesh = WaveMeshFade(wavelength, meshCellSize);
    const float harmonicMesh = WaveMeshFade(0.5f * wavelength, meshCellSize);
    const float phase = (2.0f * PI) * (travelTimes.x - time) / period;
    const float crest = sin(phase);
    const float height = fundamentalAmplitude * mainMesh * crest
        + harmonicAmplitude * harmonicMesh * (2.0f * crest * crest - 1.0f);
    return float4(height, fundamentalAmplitude * (1.0f - mainMesh),
        harmonicAmplitude * (1.0f - harmonicMesh), RIVER_SHORT_WAVE_AMPLITUDE * amplitude);
}

#endif
