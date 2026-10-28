/// @file    SchoolingJewelsAiProfiles.cs
/// @brief   Meter-scale authoring presets for mid-water schools and loose floor-following groups.
/// @author  Hasegawa Jin
/// @date    2026-10-11
using System;
using Fbzz;
using Fbzz.AI;

namespace Fbzz.Examples;

/// @note These are tunable game movement presets, not claims about the diet or speed of real species.
/// @note Model manifest: +Z forward, +Y up, scale 1; lengths Neon=.08m, Rummy=.10m, Espei=.09m, Cory=.12m.
public enum SchoolingJewelsKind { NEON, RUMMY, ESPEI, CORY }

public static class SchoolingJewelsAiProfiles
{
    /// @note Manifest schools: Neon 24-80, Rummy 30-100, Espei 18-64, Cory 6-18. PhaseOffset=0..1; AnimationRateJitter=.9..1.1.
    public static AiCrowdProfile Neon() => MidWater(0.08f, 0.13f, 0.42f, 0.10f);
    public static AiCrowdProfile Rummy() => MidWater(0.10f, 0.18f, 0.58f, 0.12f);
    public static AiCrowdProfile Espei() => MidWater(0.09f, 0.12f, 0.38f, 0.11f);

    /// @note Use ApplyHabitat with cached Terrain floor and ceiling to constrain the center to the manifest height band.
    /// @note Radius .0216m gives a desired center .0716m above Floor; safety keeps the body above the supplied envelope.
    public static AiCrowdProfile Cory()
    {
        AiCrowdProfile profile = AiCrowdProfile.CreateFloorFollowing();
        profile.BodyRadius = 0.12f * 0.18f;
        profile.NeighborRadius = 0.5f; profile.SeparationRadius = 0.16f;
        profile.Cohesion = 0.35f; profile.Alignment = 0.45f; profile.Separation = 1.3f;
        profile.GoalWeight = 0.7f; profile.CruiseSpeed = 0.06f; profile.FleeSpeed = 0.24f;
        profile.MaximumAcceleration = 0.45f; profile.MaximumTurnRate = 2;
        profile.ArrivalRadius = 0.04f; profile.SlowingRadius = 0.25f;
        profile.FloorOffset = 0.05f; profile.FloorAttraction = 3;
        return profile;
    }

    /// @note Floor and Ceiling are collision envelopes; BodyRadius converts manifest center heights into those envelope planes.
    /// @return False without changing the agent when the water/terrain envelope cannot fit the authored band.
    public static bool ApplyHabitat(ref AiCrowdAgent agent, AiCrowdProfile profile, SchoolingJewelsKind kind, float floor, float ceiling)
    {
        if (!float.IsFinite(floor) || !float.IsFinite(ceiling) || !float.IsFinite(profile.BodyRadius) || profile.BodyRadius < 0) return false;
        (float minimum, float maximum) = kind switch {
            SchoolingJewelsKind.NEON => (0.7f, 3.8f), SchoolingJewelsKind.RUMMY => (0.9f, 4.5f),
            SchoolingJewelsKind.ESPEI => (0.5f, 3), SchoolingJewelsKind.CORY => (0.02f, 0.14f), _ => (float.NaN, float.NaN) };
        float low = MathF.Max(floor, floor + minimum - profile.BodyRadius);
        float high = MathF.Min(ceiling, floor + maximum + profile.BodyRadius);
        if (!float.IsFinite(low) || !float.IsFinite(high) || low + profile.BodyRadius > high - profile.BodyRadius) return false;
        if (agent.Floor != low || agent.Ceiling != high) agent.Flags |= AiCrowdFlags.REQUEST_DECISION;
        agent.Floor = low; agent.Ceiling = high;
        return true;
    }

    static AiCrowdProfile MidWater(float bodyLength, float cruiseSpeed, float burstSpeed, float separation)
    {
        AiCrowdProfile profile = AiCrowdProfile.CreateSwimming();
        profile.BodyRadius = bodyLength * 0.18f;
        profile.NeighborRadius = separation * 3.8f; profile.SeparationRadius = separation;
        profile.Cohesion = 0.8f; profile.Alignment = 1; profile.Separation = 1.4f;
        profile.GoalWeight = 0.7f; profile.CruiseSpeed = cruiseSpeed; profile.FleeSpeed = burstSpeed;
        profile.MaximumAcceleration = 0.9f; profile.MaximumTurnRate = 3;
        profile.ArrivalRadius = bodyLength * 0.5f; profile.SlowingRadius = 0.3f;
        return profile;
    }
}
