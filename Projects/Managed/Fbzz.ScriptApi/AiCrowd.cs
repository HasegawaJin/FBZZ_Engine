/// @file    AiCrowd.cs
/// @brief   Caller-owned crowd snapshots, movement policies and one-call native simulation.
/// @author  Hasegawa Jin
/// @date    2026-10-11
using System;
using System.Runtime.InteropServices;

namespace Fbzz.AI;

public enum AiCrowdIntent : uint { WANDER, SEEK, ARRIVE, PURSUE, FLEE, HOLD, EXTERNAL }
public enum AiCrowdMode : uint { CRUISE, FLEE, PURSUE, ARRIVE, HOLD }
[Flags] public enum AiCrowdFlags : uint { NONE = 0, ACTIVE = 1, REQUEST_DECISION = 2, OVERRIDE_STEERING = 4 }
[Flags] public enum AiCrowdEvents : uint { NONE = 0, MODE_CHANGED = 1, THREAT_ENTERED = 2, THREAT_LEFT = 4, ARRIVED = 8, CONSTRAINED = 16, RECOVERED = 32 }

/// @note Distances are world meters, intervals seconds and velocities meters per second; one crowd has one habitat.
[StructLayout(LayoutKind.Sequential, Pack = 4)]
public struct AiCrowdSettings
{
    public Vector3 Minimum, Maximum, Observer;
    public float NearDistance, MidDistance, NearInterval, MidInterval, FarInterval, MaximumDelta, DeltaSeconds;
    public uint MaximumDecisions, CandidateLimit, NeighborLimit, DecisionCursor;
    public float FearDecay, ThreatMemory, AvoidanceLookahead;
    internal uint Reserved;

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Coordinate = "world", Summary = "Create a bounded habitat with 10/4/1 Hz decisions and continuous steering.")]
    public static AiCrowdSettings CreateDefault() => new()
    {
        Minimum = new(-50, -20, -50), Maximum = new(50, 0, 50), NearDistance = 25, MidDistance = 85,
        NearInterval = 0.1f, MidInterval = 0.25f, FarInterval = 1, MaximumDelta = 0.1f,
        MaximumDecisions = 256, CandidateLimit = 36, NeighborLimit = 12,
        FearDecay = 0.5f, ThreatMemory = 1.5f, AvoidanceLookahead = 0.5f
    };
}

/// @note MaximumTurnRate is radians per second; steering remains acceleration-limited between decisions.
[StructLayout(LayoutKind.Sequential, Pack = 4)]
public struct AiCrowdProfile
{
    public float NeighborRadius, SeparationRadius, Cohesion, Alignment, Separation, GoalWeight;
    public float CruiseSpeed, FleeSpeed, MaximumAcceleration, MaximumTurnRate, ArrivalRadius, SlowingRadius;
    public float BodyRadius, FloorOffset, FloorAttraction;
    public uint FollowFloor;

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Summary = "Create freely swimming or flying movement; species may adjust public fields.")]
    public static AiCrowdProfile CreateSwimming() => new()
    {
        NeighborRadius = 2, SeparationRadius = 0.6f, Cohesion = 0.4f, Alignment = 0.5f, Separation = 1.2f,
        GoalWeight = 1, CruiseSpeed = 1, FleeSpeed = 2, MaximumAcceleration = 3, MaximumTurnRate = 2,
        ArrivalRadius = 0.2f, SlowingRadius = 2, BodyRadius = 0.15f, FloorOffset = 0.3f, FloorAttraction = 2
    };

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Summary = "Create movement along a caller-sampled floor envelope; this does not perform terrain queries.")]
    public static AiCrowdProfile CreateFloorFollowing()
    {
        AiCrowdProfile profile = CreateSwimming();
        profile.FollowFloor = 1;
        return profile;
    }
}

/// @note Floor and Ceiling are cached world-Y envelopes; Seed, timers and threat memory belong to each slot.
/// @note SensedIndex identifies a slot among bounded tested candidates, not a globally closest target; DecisionAge is its freshness in seconds.
[StructLayout(LayoutKind.Sequential, Pack = 4)]
public struct AiCrowdAgent
{
    public Vector3 Position, Velocity, DesiredVelocity, Goal, Target, TargetVelocity, ExternalVelocity, DangerPosition;
    public float Floor, Ceiling, Fear, DecisionIn, DecisionAge, ThreatIn, StuckSeconds;
    public uint Seed, Group, Profile;
    public AiCrowdIntent Intent;
    public AiCrowdFlags Flags;
    public AiCrowdEvents Events;
    public AiCrowdMode Mode;
    public uint SenseGroup;
    public int SensedIndex;
    public float SensedDistance;

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Coordinate = "world", ParameterUnits = new[] { "position=m" })]
    public static AiCrowdAgent Create(Vector3 position, uint profile = 0, uint group = 0, uint seed = 1) => new()
    {
        Position = position, Goal = position, Floor = -20, Ceiling = 0, Seed = seed, Profile = profile,
        Group = group, Flags = AiCrowdFlags.ACTIVE, SenseGroup = uint.MaxValue, SensedIndex = -1
    };
}

/// @note Group=UInt32.MaxValue affects every group; strength is a zero-to-one fear value.
[StructLayout(LayoutKind.Sequential, Pack = 4)]
public struct AiCrowdThreat
{
    public Vector3 Position;
    public float Radius, Strength, Memory;
    public uint Group;
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Coordinate = "world", ParameterUnits = new[] { "position=m", "radius=m", "memory=s", "strength=ratio" })]
    public static AiCrowdThreat Create(Vector3 position, float radius = 5, float strength = 1, float memory = 1.5f, uint group = uint.MaxValue)
        => new() { Position = position, Radius = radius, Strength = strength, Memory = memory, Group = group };
}

/// @note These world-space spheres provide bounded local avoidance, not navigation or a physics collision shape.
[StructLayout(LayoutKind.Sequential, Pack = 4)]
public struct AiCrowdObstacle
{
    public Vector3 Position;
    public float Radius;
}

[StructLayout(LayoutKind.Sequential, Pack = 4)]
public struct AiCrowdStats
{
    public uint Decisions, CandidateTests, Neighbors, DeferredDecisions, Constraints, Recoveries, ThreatReactions, OccupiedCells, DecisionCursor;
    public float MaximumAcceleration, MaximumTurn;
}

/// @note No native identity, GameObject or callback is retained; this managed object owns two fixed-size snapshots.
/// @note Step and managed edits are single-threaded; reacquire Snapshot after any successful Step.
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine Data-oriented batches and decision LOD.
public sealed class AiCrowd
{
    AiCrowdAgent[] m_agents, m_next;
    readonly AiCrowdProfile[] m_profiles;
    readonly AiCrowdThreat[] m_threats = new AiCrowdThreat[AiCrowdWire.MAX_THREATS];
    readonly AiCrowdObstacle[] m_obstacles = new AiCrowdObstacle[AiCrowdWire.MAX_OBSTACLES];
    int m_threatCount, m_obstacleCount;
    bool m_inFlight;
    AiCrowdSettings m_settings = AiCrowdSettings.CreateDefault();
    AiCrowdStats m_stats;

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "ArgumentOutOfRangeException_for_agents_outside_0_to_4096_or_profiles_outside_1_to_64", Lifetime = "caller_owned_managed_object")]
    public AiCrowd(int agentCount, int profileCount = 1)
    {
        if (agentCount < 0 || agentCount > AiCrowdWire.MAX_AGENTS) throw new ArgumentOutOfRangeException(nameof(agentCount));
        if (profileCount < 1 || profileCount > AiCrowdWire.MAX_PROFILES) throw new ArgumentOutOfRangeException(nameof(profileCount));
        m_agents = new AiCrowdAgent[agentCount]; m_next = new AiCrowdAgent[agentCount];
        m_profiles = new AiCrowdProfile[profileCount];
        Array.Fill(m_profiles, AiCrowdProfile.CreateSwimming());
        for (int index = 0; index < agentCount; index++) m_agents[index] = AiCrowdAgent.Create(default, seed: (uint)index + 1);
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")] public int Count => m_agents.Length;
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")] public int ProfileCount => m_profiles.Length;
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")] public int ThreatCount => m_threatCount;
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")] public int ObstacleCount => m_obstacleCount;
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Write = "any")] public AiCrowdSettings Settings { get => m_settings; set { RequireIdle(); m_settings = value; } }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")] public AiCrowdStats Stats => m_stats;
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Lifetime = "borrowed_until_next_successful_Step")] public ReadOnlySpan<AiCrowdAgent> Snapshot => m_agents;
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "ArgumentOutOfRangeException_for_invalid_slot")] public AiCrowdAgent GetAgent(int index) => m_agents[Slot(index, Count)];
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "ArgumentOutOfRangeException_for_invalid_slot")] public void SetAgent(int index, AiCrowdAgent agent) { RequireIdle(); m_agents[Slot(index, Count)] = agent; }
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "ArgumentOutOfRangeException_for_invalid_slot")] public AiCrowdProfile GetProfile(int index) => m_profiles[Slot(index, ProfileCount)];
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "ArgumentOutOfRangeException_for_invalid_slot")] public void SetProfile(int index, AiCrowdProfile profile) { RequireIdle(); m_profiles[Slot(index, ProfileCount)] = profile; }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "false_over_64_without_changes")]
    public bool SetThreats(ReadOnlySpan<AiCrowdThreat> threats)
    {
        RequireIdle();
        if (threats.Length > m_threats.Length) return false;
        threats.CopyTo(m_threats); m_threatCount = threats.Length; return true;
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "false_over_256_without_changes")]
    public bool SetObstacles(ReadOnlySpan<AiCrowdObstacle> obstacles)
    {
        RequireIdle();
        if (obstacles.Length > m_obstacles.Length) return false;
        obstacles.CopyTo(m_obstacles); m_obstacleCount = obstacles.Length; return true;
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Coordinate = "world", Failure = "false_for_short_buffer_without_changes", ParameterUnits = new[] { "positions=m" })]
    public bool CopyPositions(Span<Vector3> positions)
    {
        if (positions.Length < Count) return false;
        for (int index = 0; index < Count; index++) positions[index] = m_agents[index].Position;
        return true;
    }

    /// @note One native transfer; successful output replaces the snapshot atomically. Events describe only this step and invoke no script callbacks.
    /// @note Movement delta above MaximumDelta is clamped; decision and threat clocks retain the supplied elapsed seconds.
    [ApiContract(Purity = ApiPurity.ENGINE, RunModes = new[] { "Play" }, Call = "live", Coordinate = "world", ParameterUnits = new[] { "deltaSeconds=s" }, Failure = "false_for_invalid_numeric_or_policy_input_without_changes;_invalid_scope_or_unavailable_bridge_throws", Lifetime = "current_callback_thread")]
    public bool Step(float deltaSeconds)
    {
        RequireIdle();
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        if (scope.Bridge is not IScriptAiCrowdNativeBridge bridge) throw new NotSupportedException("This Host does not support AiCrowd.");
        AiCrowdSettings settings = m_settings;
        settings.DeltaSeconds = deltaSeconds;
        m_inFlight = true;
        try
        {
            if (!bridge.StepCrowd(scope, settings, m_profiles, m_agents, m_threats.AsSpan(0, m_threatCount),
                m_obstacles.AsSpan(0, m_obstacleCount), m_next, out AiCrowdStats stats)) return false;
            (m_agents, m_next) = (m_next, m_agents);
            settings.DecisionCursor = stats.DecisionCursor; m_settings = settings; m_stats = stats;
            return true;
        }
        finally { m_inFlight = false; }
    }

    void RequireIdle() { if (m_inFlight) throw new InvalidOperationException("AiCrowd is already stepping."); }
    static int Slot(int index, int count) => (uint)index < (uint)count ? index : throw new ArgumentOutOfRangeException(nameof(index));
}

internal interface IScriptAiCrowdNativeBridge
{
    bool StepCrowd(ScriptExecutionScope scope, AiCrowdSettings settings, ReadOnlySpan<AiCrowdProfile> profiles,
        ReadOnlySpan<AiCrowdAgent> agents, ReadOnlySpan<AiCrowdThreat> threats, ReadOnlySpan<AiCrowdObstacle> obstacles,
        Span<AiCrowdAgent> output, out AiCrowdStats stats);
}

internal static class AiCrowdWire
{
    internal const int VERSION = 1, HEADER_BYTES = 48, SETTINGS_BYTES = 96, PROFILE_BYTES = 64, AGENT_BYTES = 164,
        THREAT_BYTES = 28, OBSTACLE_BYTES = 16, STATS_BYTES = 44, MAX_AGENTS = 4096, MAX_PROFILES = 64,
        MAX_THREATS = 64, MAX_OBSTACLES = 256, MAX_PACKET_BYTES = 1024 * 1024;
}
