/// @file    WaterRiverFlow.hlsli
/// @brief   画素間の座標変換、逆追跡、移流する筋と段差泡の経過時間。
/// @author  Hasegawa Jin
/// @date    2026-11-18
#ifndef FBZZ_WATER_RIVER_FLOW_HLSLI
#define FBZZ_WATER_RIVER_FLOW_HLSLI

/// @note WaterStandardAppearance.hlsli から読み込む。
/// @note Convert a world XZ displacement to screen-pixel axes for local derivatives and UV lookup.
float2x2 WaterRiverWorldToPixel(float2 worldDX, float2 worldDY, out float determinant)
{
    determinant = worldDX.x * worldDY.y - worldDX.y * worldDY.x;
    const float inverse = abs(determinant) > 1.0e-10f ? rcp(determinant) : 0.0f;
    return float2x2(worldDY.y, -worldDY.x, -worldDX.y, worldDX.x) * inverse;
}

/// @note A quadratic backtrace follows a locally turning current over a short interval.
/// @see https://web.mit.edu/fluids-modules/www/potential_flows/LecturesHTML/lec02/lecture2.html MIT Marine Hydrodynamics, Lagrangian Acceleration.
float2 WaterRiverBacktrace(float2 velocity, float2 acceleration, float age)
{
    return (-velocity + 0.5f * age * acceleration) * age;
}

/// @return x=生き残った泡の供給量、y=細い泡の筋への移行量。
/// @see https://agupubs.onlinelibrary.wiley.com/doi/10.1029/2012JC008147 Callaghan et al. (2012), exponential whitecap decay.
float2 WaterRiverStandingFoam(float2 source, float2 authoredVelocity, float2 velocity, float2 acceleration,
    float lifetime)
{
    const float2 downstream = authoredVelocity * rsqrt(max(dot(authoredVelocity, authoredVelocity), 1.0e-6f));
    const float forwardSpeed = dot(velocity, downstream);
    const float duration = WaterRiverStandingFoamDuration(lifetime);
    const float distance = source.y / max(source.x, 1.0e-5f);
    const float inverseSpeed = rcp(max(forwardSpeed, 0.24f));
    const float linearAge = distance * inverseSpeed;
    /// @note Invert the same short backtrace to first order along the authored current.
    const float correction = 0.5f * dot(acceleration, downstream) * inverseSpeed * min(linearAge, duration);
    const float age = linearAge * clamp(1.0f + correction, 0.76f, 1.32f);
    const float relativeAge = age / max(lifetime, 1.0e-3f);
    const float survival = exp(-relativeAge) * (1.0f - smoothstep(0.68f * duration, max(duration, 1.0e-4f), age));
    const float release = smoothstep(0.03f, 0.28f, age) * smoothstep(0.12f, 0.47f, forwardSpeed);
    return float2(source.x * release * survival, smoothstep(0.12f, 1.28f, relativeAge));
}

/// @note A longer filter footprint along the flow stretches each feature without rotating absolute coordinates.
float3 WaterRiverStreakTap(float2 position, float2x2 baseGradient, float2x2 backtraceGradient,
    float2x2 worldToPixel, float2 axis, float scale)
{
    const float2x2 tileGradient = (baseGradient + backtraceGradient) * scale;
    const float2 across = float2(-axis.y, axis.x);
    const float alongFootprint = length(mul(tileGradient, axis));
    const float acrossFootprint = length(mul(tileGradient, across));
    float3 streak = WaterNoiseTile(position * scale, axis * max(0.72f, alongFootprint),
        across * max(0.05f, acrossFootprint));
    /// @note Apply the chain rule to the baked slopes so the normals stretch with the advected pattern.
    /// @see https://jcgt.org/published/0009/03/04/ Mikkelsen (2020), surface gradient composition.
    const float2 slopeCorrection = mul(mul(backtraceGradient, streak.xy), worldToPixel);
    streak.xy += clamp(slopeCorrection, -0.35f, 0.35f);
    return streak;
}

/// @note Phase staggering keeps a long reach from resetting its streaks in unison.
/// @see https://cdn.cloudflare.steamstatic.com/apps/valve/2010/siggraph2010_vlachos_waterflow.pdf Vlachos (2010), dual phases and spatial phase offsets.
float3 WaterRiverStreakAt(float2 worldXZ, float2 flowDir, float2 velocity, float2 acceleration,
    float time, float scale, float visibility, float2x2 worldToPixel)
{
    const float period = 3.7f;
    const float jitter = 0.13f * (sin(dot(worldXZ, float2(0.073f, -0.102f)))
        + sin(dot(worldXZ, float2(0.112f, 0.061f))));
    const float2x2 baseGradient = float2x2(ddx(worldXZ), ddy(worldXZ));
    const float2x2 velocityGradient = float2x2(ddx(velocity), ddy(velocity));
    /// @note Differentiate the unwrapped phase, holding local acceleration fixed for the noise footprint.
    const float2 ageGradient = period * float2(ddx(jitter), ddy(jitter));
    [branch] if (visibility <= 0.0f) return 0.0f;

    const float first = frac(time / period + jitter);
    const float second = frac(first + 0.5f);
    const float firstAge = period * first;
    const float secondAge = period * second;
    const float2 firstRate = -velocity + firstAge * acceleration;
    const float2 secondRate = -velocity + secondAge * acceleration;
    const float2x2 firstGradient = float2x2(firstRate * ageGradient.x, firstRate * ageGradient.y)
        - firstAge * velocityGradient;
    const float2x2 secondGradient = float2x2(secondRate * ageGradient.x, secondRate * ageGradient.y)
        - secondAge * velocityGradient;
    const float firstWeight = smoothstep(0.0f, 1.0f, 1.0f - abs(2.0f * first - 1.0f));
    const float3 a = WaterRiverStreakTap(worldXZ + WaterRiverBacktrace(velocity, acceleration, firstAge),
        baseGradient, firstGradient, worldToPixel, flowDir, scale);
    const float3 b = WaterRiverStreakTap(worldXZ + WaterRiverBacktrace(velocity, acceleration, secondAge),
        baseGradient, secondGradient, worldToPixel, flowDir, scale);
    return lerp(b, a, firstWeight);
}

#endif
