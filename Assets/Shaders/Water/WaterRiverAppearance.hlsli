/// @file    WaterRiverAppearance.hlsli
/// @brief   河川波の法線・白波・反射と段差を越える泡の共通模様。
/// @author  Hasegawa Jin
/// @date    2026-11-18
#ifndef FBZZ_WATER_RIVER_APPEARANCE_HLSLI
#define FBZZ_WATER_RIVER_APPEARANCE_HLSLI

/// @note WaterStandardAppearance.hlsli から読み込む。
/// @brief 流速 [m/s] と基準水深 [m] から浅瀬のさざ波と泡を強める係数を求める。
/// @note フルード数の局所近似。薄い水際と低速の水面を落とし、浅瀬を一様な白帯にしない。
/// @see https://www.hec.usace.army.mil/publications/TechnicalPapers/TP-110.pdf Subdivision Froude Number, Eq. (1).
/// @see https://doi.org/10.1029/2009WR007731 Magirl et al. (2009), breaking waves in subcritical rapids.
float WaterRapidsWeight(float speed, float depth)
{
    const float froude = speed * rsqrt(9.81f * max(depth, 0.025f));
    const float wet = smoothstep(0.025f, 0.12f, depth);
    const float moving = smoothstep(0.35f, 1.0f, speed);
    return wet * moving * smoothstep(0.45f, 1.15f, froude);
}

struct WaterRiverWavePhase
{
    float2 cycles;
    float2 rates;
    float2x2 screenGradient; ///< @brief Each row is the x/y pixel derivative of one band's phase in cycles.
};

WaterRiverWavePhase WaterRiverPhaseAt(float2 travelTimes, float rockShift, float time)
{
    WaterRiverWavePhase phase = (WaterRiverWavePhase)0;
    [branch] if (g_riverWaveParams.x <= 0.0f) return phase;
    phase.rates = rcp(float2(g_riverWaveParams.x, RIVER_SHORT_WAVE_PERIOD));
    const float2 travel = travelTimes + float2(0.0f, rockShift);
    phase.cycles = (travel - time) * phase.rates + float2(0.0f, 0.8f / (2.0f * PI));
    const float2 dx = ddx(travel) * phase.rates;
    const float2 dy = ddy(travel) * phase.rates;
    phase.screenGradient = float2x2(dx.x, dy.x, dx.y, dy.y);
    return phase;
}

/// @return xy=unresolved traveling wave gradient, z=slope variance outside the pixel bandwidth.
float3 WaterRiverWaveOptics(float4 wave, WaterRiverWavePhase phase, float2x2 worldToPixel, float streakPacket)
{
    if (all(wave.yzw <= 0.0f)) return 0.0f;
    const float mainCycles = max(abs(phase.screenGradient[0].x), abs(phase.screenGradient[0].y));
    const float shortCycles = max(abs(phase.screenGradient[1].x), abs(phase.screenGradient[1].y));
    const float3 keep = 1.0f - smoothstep(0.15f, 0.5f, float3(mainCycles, 2.0f * mainCycles, shortCycles));
    const float2 mainGradient = clamp(mul(phase.screenGradient[0], worldToPixel) * (2.0f * PI), -6.0f, 6.0f);
    const float2 shortGradient = clamp(mul(phase.screenGradient[1], worldToPixel) * (2.0f * PI), -6.0f, 6.0f);
    const float shortAmplitude = wave.w * lerp(0.42f, 1.0f, streakPacket);
    const float3 amplitudes = float3(wave.y, 2.0f * wave.z, shortAmplitude);
    const float3 visible = amplitudes * keep;
    float2 slope = 0.0f;
    [branch] if (visible.x > 0.0f || visible.y > 0.0f) {
        float sine, cosine;
        sincos((2.0f * PI) * phase.cycles.x, sine, cosine);
        slope += mainGradient * (cosine * (visible.x + 2.0f * visible.y * sine));
    }
    [branch] if (visible.z > 0.0f)
        slope += shortGradient * (visible.z * cos((2.0f * PI) * phase.cycles.y));
    const float3 loss = amplitudes * amplitudes * (1.0f - keep * keep);
    const float variance = 0.25f * ((loss.x + loss.y) * dot(mainGradient, mainGradient)
        + loss.z * dot(shortGradient, shortGradient));
    return float3(slope, variance);
}

/// @return xy=勾配のメッシュに入らなかった分、z=画素未満の傾きの分散。
/// @see https://jcgt.org/published/0009/03/04/ Mikkelsen (2020), surface gradient composition.
float3 WaterRiverStandingOptics(float phase, float amplitude, float spacing, float2 axis, float2 baseDX, float2 baseDY)
{
    /// @note 同じ峰の断面だけを微分する。隣接する画素の別の峰や強度を引くと、水深テクセルの境界に偽の反射が出る。
    const float step = 0.015f;
    const float4 upstream = WaterRiverStandingSection(phase - step);
    const float4 downstream = WaterRiverStandingSection(phase + step);
    const float heightDelta = (downstream.x + downstream.y) - (upstream.x + upstream.y);
    const float2 phaseGradient = axis / max(spacing, 0.55f);
    const float2 slope = phaseGradient * (heightDelta * amplitude / (2.0f * step));
    const float phaseFootprint = max(abs(dot(baseDX, phaseGradient)), abs(dot(baseDY, phaseGradient)));
    const float keep = 1.0f - smoothstep(0.075f, 0.28f, phaseFootprint);
    return float3(keep * slope, 0.23f * (1.0f - keep * keep) * dot(slope, slope));
}

/// @return 上り斜面、波頭、下流側の広がり、下流側の途切れを制御する重み。
float4 WaterRiverStandingReflectionProfile(float phase, float strength)
{
    const float resolved = strength * (1.0f - smoothstep(0.12f, 0.38f, fwidth(phase)));
    const float climb = smoothstep(-0.68f, -0.23f, phase) * (1.0f - smoothstep(-0.24f, 0.025f, phase));
    const float pinch = 1.0f - smoothstep(0.045f, 0.24f, abs(phase));
    const float downstream = 1.0f - smoothstep(0.79f, 1.20f, phase);
    const float spread = smoothstep(0.055f, 0.42f, phase) * downstream;
    const float breakup = smoothstep(0.23f, 0.69f, phase) * downstream;
    return resolved * float4(climb, pinch, spread, breakup);
}

/// @return xy=反射の被覆と分断量、zw=同じ流れに乗る泡の広い部分と細い芯。
float4 WaterRiverStandingStreak(float4 profile, float streakHeight, float alongSlope, float packet, float2 trail)
{
    const float heightAA = min(0.5f * fwidth(streakHeight), 0.14f);
    const float alongMagnitude = abs(alongSlope);
    const float edgeAA = min(0.5f * fwidth(alongMagnitude), 0.10f);
    [branch] if (!any(profile > 0.0f) && trail.x <= 0.001f) return float4(packet, 0.0f, 0.0f, 0.0f);

    /// @note The reflective water and the foam use contours of the same backtraced packet and tear at the same moving gaps.
    const float shoulder = smoothstep(0.49f - heightAA, 0.69f + heightAA, streakHeight);
    const float core = smoothstep(0.58f - heightAA, 0.75f + heightAA, streakHeight);
    const float wide = smoothstep(0.41f - heightAA, 0.64f + heightAA, streakHeight);
    const float trailSupport = smoothstep(0.01f, 0.38f, trail.x);
    const float spread = max(profile.z, 0.78f * trailSupport);
    const float focused = lerp(lerp(packet, shoulder, profile.x), core, profile.y);
    const float tearWeight = max(profile.w, trailSupport * lerp(0.35f, 0.90f, trail.y));
    const float split = tearWeight * lerp(0.17f, 0.98f, smoothstep(0.055f - edgeAA, 0.24f + edgeAA, alongMagnitude));
    const float connected = 1.0f - split;
    const float foamBody = lerp(lerp(shoulder, core, 0.28f * profile.y), wide, spread);
    return float4(lerp(focused, wide, spread) * connected, split, foamBody * connected, core * connected);
}

/// @note Depth-limited breaking uses the visual wave height relative to the wet column and the local Froude cue.
/// @see https://www.fhwa.dot.gov/engineering/hydraulics/pubs/hif19059.pdf HEC-25 §5.2, wave breaking in shallow water.
float WaterRiverBreakingSource(float depth, float speed, float waveAmplitude)
{
    const float heightRatio = 2.0f * waveAmplitude / max(depth, 0.025f);
    const float steep = smoothstep(0.19f, 0.55f, heightRatio);
    const float shallow = 1.0f - smoothstep(0.37f, 0.92f, depth);
    return steep * shallow * WaterRapidsWeight(speed, depth);
}

/// @return x=fresh coverage, y=age of the strongest survivor [s], z=remaining coverage, w=coverage-weighted visual age [s].
/// @see https://doi.org/10.1029/2012JC008147 Callaghan et al. (2012), exponential decay of whitecap foam.
float4 WaterRiverCrestHistory(WaterRiverWavePhase phase, float2 pixelVelocity, float lifetime)
{
    const float2 footprint = max(abs(float2(phase.screenGradient[0].x, phase.screenGradient[1].x)),
        abs(float2(phase.screenGradient[0].y, phase.screenGradient[1].y)));
    const float2 visible = 1.0f - smoothstep(0.13f, 0.42f, footprint);
    const float2 lag = frac(0.28f - phase.cycles);
    const float2 rim = visible * (1.0f - smoothstep(0.023f, 0.085f, min(lag, 1.0f - lag)));
    const float2 relativeRate = clamp(phase.rates - mul(phase.screenGradient, pixelVelocity),
        0.12f * phase.rates, 1.8f * phase.rates);
    const float duration = min(2.4f * lifetime, 3.1f);
    const float2 ages = lag / max(relativeRate, 1.0e-6f);
    const float2 endFade = 1.0f - smoothstep(0.68f * duration, max(duration, 1.0e-4f), ages);
    const float2 remaining = visible * smoothstep(0.0f, 0.14f, ages) * endFade
        * exp(-ages / max(lifetime, 1.0e-3f));
    const bool useShort = remaining.y > remaining.x;
    const float visualAge = dot(remaining, ages) / max(remaining.x + remaining.y, 1.0e-6f);
    return float4(max(rim.x, 0.88f * rim.y), useShort ? ages.y : ages.x, max(remaining.x, remaining.y), visualAge);
}

/// @note Scale both GGX axes around the same mean roughness and use the isotropic denominator guard at every aspect ratio.
/// @see https://pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory PBRT v4 §9.6, anisotropic Trowbridge–Reitz.
float3 WaterRiverSunSpecular(float3 normal, float3 viewDir, float3 lightDir, float f0,
    float roughness, float3 tangent, float3 binormal, float anisotropy)
{
    const float3 halfVector = SafeNormalize(viewDir + lightDir, normal);
    const float alphaSquared = Sq(Sq(roughness));
    const float aspectSquared = 1.0f - 0.9f * min(anisotropy, 0.95f);
    const float denominator = Sq(dot(halfVector, tangent)) * aspectSquared
        + Sq(dot(halfVector, binormal)) / aspectSquared + alphaSquared * Sq(saturate(dot(halfVector, normal)));
    const float distribution = alphaSquared / (PI * Sq(denominator) + EPSILON);
    const float viewCosine = saturate(dot(normal, viewDir));
    const float lightCosine = saturate(dot(normal, lightDir));
    const float masking = G_Smith(viewCosine, lightCosine, roughness);
    const float3 fresnel = F_Schlick(saturate(dot(viewDir, halfVector)), f0.xxx);
    return (distribution * masking * fresnel) / max(4.0f * viewCosine * lightCosine, EPSILON);
}

struct WaterRiverAppearanceState
{
    float3 waveOptics; ///< @brief xy=ワールド XZ の波の残差勾配、z=画素より細い傾きの分散。
    float2 standingWave;
    float4 standingReflection;
    float4 standingStreak;
    float2 standingTrail;
    float2 standingFoam;
    float2 flowSlope;
    float3 breakingFoam;
    float streakVisibility;
    float streakPacket;
    float crestDetail;
    float shoreFoamBreakup;
    float carriedFoam;
    float rapids;
    float foamWetness;
    float foamProximity;
};

/// @note Derivatives are evaluated with the same coverage and local velocity used by the standard pixel shader.
WaterRiverAppearanceState EvaluateWaterRiverAppearance(WaterPSInput p, WaterMaterialBlendTarget material,
    WaterRiverObstacleFlow obstacleFlow, float2 flowDir, float currentSpeed, float driftScale,
    float2 baseDX, float2 baseDY, float footprint, float2x2 worldToPixel, float2 splineTerrainDepth,
    float splineMeanDepth)
{
    WaterRiverAppearanceState result = (WaterRiverAppearanceState)0;
    const bool splineSurface = p.splineSurface.w > 0.5f;
    const float time = g_timeParams.w;
    const float authoredDepth = max(p.splineSurface.x, 0.0f);
    result.rapids = splineSurface ? WaterRapidsWeight(currentSpeed, splineMeanDepth) : 0.0f;
    const float foamThreshold = material.foam.x;
    const float foamFade = max(material.foam.y, 0.0001f);
    const float2 uvDX = ddx(p.uv);
    const float2 uvDY = ddy(p.uv);
    float2 standingSource = p.riverStandingWave.zw;
    float3 standingOptics = 0.0f;
    [branch] if (g_riverWaveParams.x > 0.0f) {
        float strength = saturate(p.riverStandingWave.x);
        const float2 axis = p.splineSurface.yz * rsqrt(max(dot(p.splineSurface.yz, p.splineSurface.yz), 1.0e-6f));
        const float2 axisInPixels = mul(worldToPixel, axis);
        const float2 atlasRate = (uvDX * axisInPixels.x + uvDY * axisInPixels.y) * g_terrainInfoParams.zw;
        float spacing = WaterRiverStandingSpacing(p.meshCellSize, length(atlasRate));
        float phase = p.riverStandingWave.y / max(strength, 1.0e-5f);
        float amplitude = p.riverStandingAmplitude;
        const float refine = (1.0f - smoothstep(0.12f, 0.25f, footprint))
            * smoothstep(0.06f, 0.14f, splineMeanDepth) * (1.0f - smoothstep(1.55f, 1.8f, splineMeanDepth));
        [branch] if (refine > 0.0f && splineTerrainDepth.x > 0.74f) {
            /// @note 手前の浅瀬でだけ断面を再評価し、異なる段差を含む三角形の補間による縞を防ぐ。
            float2 unusedSource;
            float4 sectionAtPixel;
            WaterRiverStandingWave(p.baseWorldXZ, p.splineSurface.yz, 0.0f, authoredDepth,
                splineTerrainDepth, p.meshCellSize, WaterfallWaveWeight(p.baseWorldXZ), false, unusedSource, sectionAtPixel);
            strength = lerp(strength, sectionAtPixel.x, refine);
            amplitude = lerp(amplitude, sectionAtPixel.z, refine);
            if (sectionAtPixel.x > 0.0f) {
                spacing = lerp(spacing, sectionAtPixel.y, refine);
                phase = lerp(phase, sectionAtPixel.w / sectionAtPixel.y, refine);
            }
        }
        const float4 section = WaterRiverStandingSection(phase);
        result.standingWave = strength * section.zw;
        standingOptics = WaterRiverStandingOptics(phase, amplitude, spacing, axis, baseDX, baseDY);
        result.standingReflection = WaterRiverStandingReflectionProfile(phase, strength);
        const float released = strength * smoothstep(-0.05f, 0.10f, phase);
        if (released > standingSource.x)
            standingSource = float2(released, released * max(phase * spacing, 0.0f));
    }
    const float rockPhaseShift = 0.36f * obstacleFlow.crestBend / (min(currentSpeed, 5.0f) + 1.4f);
    const WaterRiverWavePhase wavePhase = WaterRiverPhaseAt(float2(p.riverWave.x, p.riverShortTravelTime), rockPhaseShift, time);
    result.shoreFoamBreakup = 1.0f;
    result.foamWetness = smoothstep(0.045f, 0.18f, splineMeanDepth);
    result.foamProximity = 1.0f - smoothstep(0.20f, 0.65f, footprint);
    [branch] if (splineSurface) {
        const float streakScale = clamp(material.detail.x * 0.8f, 0.33f, 1.2f);
        const float wetCurrent = smoothstep(0.18f, 1.25f, currentSpeed)
            * smoothstep(0.025f, 0.14f, splineMeanDepth);
        const float2 acrossFlow = float2(-flowDir.y, flowDir.x);
        const float crossFootprint = max(abs(dot(baseDX, acrossFlow)), abs(dot(baseDY, acrossFlow)));
        /// @note At a grazing view the stripe can still be resolved across the current. Cap its range with the full footprint.
        const float streakFootprint = max(crossFootprint, 0.42f * footprint);
        result.streakVisibility = wetCurrent * (1.0f - smoothstep(0.30f, 1.10f, streakFootprint * streakScale));
        const float driftSpeed = min(currentSpeed * driftScale, 5.0f);
        const float2 velocity = flowDir * driftSpeed;
        const float2 pixelVelocity = mul(worldToPixel, velocity);
        /// @note Dv/Dt includes both the velocity variation along the streamline and the unsteady wake.
        const float driftRatio = driftSpeed / max(currentSpeed, 1.0e-4f);
        float2 acceleration = ddx(velocity) * pixelVelocity.x + ddy(velocity) * pixelVelocity.y
            + driftRatio * obstacleFlow.timeAcceleration;
        acceleration *= min(1.0f, 0.16f * driftSpeed / max(length(acceleration), 1.0e-5f));
        /// @note Keep the upstream terrain lookup on the shorter, isotropic range.
        const float transportVisibility = wetCurrent * (1.0f - smoothstep(0.30f, 1.10f, footprint * streakScale));
        [branch] if (standingSource.x > 0.001f && material.foam.z > 0.0f && material.crestFoam.w > 0.001f
            && transportVisibility > 0.0f) {
            result.standingTrail = WaterRiverStandingFoam(standingSource, p.splineSurface.yz,
                velocity, acceleration, material.crestFoam.w);
            result.standingTrail.x *= transportVisibility;
        }

        const float3 streak = WaterRiverStreakAt(p.baseWorldXZ, flowDir, velocity, acceleration,
            time, streakScale, result.streakVisibility, worldToPixel);
        result.streakPacket = smoothstep(0.43f, 0.70f, streak.z);
        /// @note Keep the ends of bright streaks sloped along the current so they visibly travel downstream.
        const float alongSlope = dot(streak.xy, flowDir);
        result.standingStreak.x = result.streakPacket;
        [branch] if (g_riverWaveParams.x > 0.0f)
            result.standingStreak = WaterRiverStandingStreak(result.standingReflection, streak.z, alongSlope, result.streakPacket, result.standingTrail);
        const float freshFoam = saturate(1.55f * result.standingWave.x)
            * result.standingStreak.z * result.foamWetness * result.foamProximity;
        const float trailingFoam = result.standingTrail.x * lerp(result.standingStreak.z, result.standingStreak.w, result.standingTrail.y);
        /// @note 砕ける波頭は crestFoamStrength を共有し、岸泡を薄く設定した川でも白く泡立つ。
        const float standingFoamStrength = material.foam.z > 0.0f ? max(material.foam.z, material.crestFoam.z) : 0.0f;
        result.standingFoam = saturate(standingFoamStrength * float2(freshFoam, trailingFoam));
        const float2 acrossSlope = streak.xy - flowDir * alongSlope;
        const float alongGain = lerp(0.34f, 0.68f, result.streakPacket) + 0.60f * result.standingReflection.x;
        const float acrossGain = (1.0f + 0.58f * result.standingReflection.y * result.standingStreak.x)
            * (1.0f - 0.30f * result.standingStreak.y);
        const float2 shapedSlope = acrossSlope * acrossGain + flowDir * (alongGain * alongSlope);
        const float standingDetail = dot(result.standingWave, float2(0.72f, 0.26f));
        result.flowSlope = shapedSlope * (0.52f * (1.0f + standingDetail) * result.streakVisibility * max(material.detail.z, 0.0f));
        /// @note The same moving patches that carry foam give the river crests an uneven profile.
        const float crestPatch = 0.65f + 0.35f * result.streakPacket * result.streakVisibility;
        result.crestDetail = (0.15f + 0.06f * result.rapids) * crestPatch * wetCurrent * saturate(material.detail.z);
        result.shoreFoamBreakup = 1.0f - 0.62f * result.streakVisibility * (1.0f - result.streakPacket);
        const float filament = transportVisibility * result.streakPacket;
        const float crestAmplitude = p.riverWave.w / RIVER_SHORT_WAVE_AMPLITUDE;
        float4 crestHistory = 0.0f;
        [branch] if (g_riverWaveParams.x > 0.0f && crestAmplitude > 0.0f && material.crestFoam.z > 0.001f
            && transportVisibility > 0.0f) {
            crestHistory = WaterRiverCrestHistory(wavePhase, pixelVelocity, material.crestFoam.w);
            crestHistory.xz *= transportVisibility;
            result.breakingFoam.x = crestHistory.x * WaterRiverBreakingSource(splineMeanDepth, currentSpeed, crestAmplitude);
        }
        const bool traceCrest = crestHistory.z > 0.001f;
        float birthDepth = splineMeanDepth;
        [branch] if (g_terrainInfoParams.x > 0.5f && splineTerrainDepth.x > 0.0f
            && (traceCrest || (material.foam.z > 0.0f && filament > 0.0f))) {
            /// @note Both foam paths share one terrain query at the selected crest age when its residual foam is visible.
            const float sourceAge = traceCrest ? crestHistory.y : 1.35f;
            const float2 upstreamPixel = mul(worldToPixel, WaterRiverBacktrace(velocity, acceleration, sourceAge));
            const float2 upstreamUv = p.uv + uvDX * upstreamPixel.x + uvDY * upstreamPixel.y;
            const float2 source = SampleWaterTerrainDepth(upstreamUv);
            const float sourceDepth = min(authoredDepth, source.y);
            birthDepth = lerp(splineMeanDepth, sourceDepth, saturate(source.x));
            const float sourceRapids = WaterRapidsWeight(currentSpeed, sourceDepth);
            /// @note Release bank foam where the traced flow moves out of the shallows.
            const float leavingBank = smoothstep(0.04f, 0.25f, splineMeanDepth - sourceDepth);
            const float bankFoam = WaterSplineShoreCoverage(sourceDepth, foamThreshold, foamFade) * leavingBank;
            result.carriedFoam = source.x * filament * max(0.73f * sourceRapids, 0.43f * bankFoam);
        }
        if (traceCrest) {
            result.breakingFoam.y = crestHistory.z * WaterRiverBreakingSource(birthDepth, currentSpeed, crestAmplitude);
            result.breakingFoam.z = smoothstep(0.16f, 1.35f, crestHistory.w / max(material.crestFoam.w, 1.0e-3f));
        }
    }
    result.waveOptics = WaterRiverWaveOptics(p.riverWave, wavePhase, worldToPixel, result.streakPacket) + standingOptics;
    return result;
}

#endif
