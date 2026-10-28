/// @file    AiCrowdExample.cs
/// @brief   Script-side policy changes without native code or per-agent callbacks.
/// @author  Hasegawa Jin
/// @date    2026-10-11
using System;
using Fbzz;
using Fbzz.AI;

namespace Fbzz.Examples;

/// @note This helper receives authored positions and cached terrain envelopes; it creates no scene objects.
public sealed class AiCrowdExample
{
    readonly AiCrowd m_crowd;
    readonly Vector3[] m_positions;

    public AiCrowdExample(ReadOnlySpan<Vector3> authoredPositions)
    {
        m_crowd = new AiCrowd(authoredPositions.Length, 2);
        m_positions = new Vector3[authoredPositions.Length];
        m_crowd.SetProfile(1, AiCrowdProfile.CreateFloorFollowing());
        for (int index = 0; index < authoredPositions.Length; index++)
            m_crowd.SetAgent(index, AiCrowdAgent.Create(authoredPositions[index], group: 1, seed: (uint)index + 100));
    }

    /// @note Call when a special script condition changes; immediate threat steering is independent of decision LOD.
    public void SetAlarm(bool frightened, Vector3 danger)
    {
        Span<AiCrowdThreat> threats = stackalloc AiCrowdThreat[1];
        if (frightened) threats[0] = AiCrowdThreat.Create(danger, radius: 8, group: 1);
        m_crowd.SetThreats(frightened ? threats : ReadOnlySpan<AiCrowdThreat>.Empty);
    }

    /// @note Script may choose any target or a target returned by its own BT condition.
    public void Pursue(int slot, Vector3 target, Vector3 targetVelocity)
    {
        AiCrowdAgent agent = m_crowd.GetAgent(slot);
        agent.Intent = AiCrowdIntent.PURSUE; agent.Target = target; agent.TargetVelocity = targetVelocity;
        agent.Flags |= AiCrowdFlags.REQUEST_DECISION; m_crowd.SetAgent(slot, agent);
    }

    /// @note Substitute movement input while retaining habitat safety and acceleration limits.
    public void OverrideMovement(int slot, Vector3 desiredVelocity)
    {
        AiCrowdAgent agent = m_crowd.GetAgent(slot);
        agent.Intent = AiCrowdIntent.EXTERNAL; agent.ExternalVelocity = desiredVelocity;
        agent.Flags |= AiCrowdFlags.OVERRIDE_STEERING; m_crowd.SetAgent(slot, agent);
    }

    /// @note Invoke from a live Play callback, then commit positions with Objects.TrySetWorldPositions and consume Events before the next Step.
    public bool Step(float deltaSeconds, Vector3 observer)
    {
        AiCrowdSettings settings = m_crowd.Settings; settings.Observer = observer; m_crowd.Settings = settings;
        return m_crowd.Step(deltaSeconds) && m_crowd.CopyPositions(m_positions);
    }

    public ReadOnlySpan<Vector3> Positions => m_positions;
    public ReadOnlySpan<AiCrowdAgent> Snapshot => m_crowd.Snapshot;
}
