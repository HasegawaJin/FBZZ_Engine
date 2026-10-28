/// @file    AiCrowdQueries.cs
/// @brief   Small caller-owned motion cue queries over the cached managed crowd snapshot.
/// @author  Hasegawa Jin
/// @date    2026-10-11
using System;

namespace Fbzz.AI;

public struct AiCrowdMotionCue
{
    public int Index;
    public Vector3 Position, Velocity;
    public AiCrowdMode Mode;
    public AiCrowdEvents Events;
    public float Fear;
    public double DistanceSquared;
}

public static class AiCrowdQueries
{
    /// @note O(snapshot length times output length), output is limited to 16 cues. Run at the cue cadence rather than once for every fish.
    /// @note Pure managed snapshot query; no allocation, Scene getter, native call, or retained borrowed span.
    /// @return Valid prefix length ordered by distance then slot. Invalid arguments return zero; unused output tail is unchanged.
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Coordinate = "world", ParameterUnits = new[] { "maximumDistance=m", "minimumSpeed=m_per_s" })]
    public static int CopyNearestMoving(ReadOnlySpan<AiCrowdAgent> snapshot, Vector3 observer,
        float maximumDistance, Span<AiCrowdMotionCue> output, float minimumSpeed = 0.02f)
    {
        if (!observer.IsFinite || !float.IsFinite(maximumDistance) || maximumDistance < 0 ||
            !float.IsFinite(minimumSpeed) || minimumSpeed < 0 || output.Length > 16 || output.IsEmpty) return 0;
        double limit = (double)maximumDistance * maximumDistance, minimum = (double)minimumSpeed * minimumSpeed;
        int count = 0;
        for (int index = 0; index < snapshot.Length; ++index) {
            ref readonly AiCrowdAgent agent = ref snapshot[index];
            if ((agent.Flags & AiCrowdFlags.ACTIVE) == 0 || !agent.Position.IsFinite || !agent.Velocity.IsFinite) continue;
            double speed = (double)agent.Velocity.X * agent.Velocity.X + (double)agent.Velocity.Y * agent.Velocity.Y + (double)agent.Velocity.Z * agent.Velocity.Z;
            if (speed < minimum || speed == 0) continue;
            double dx = (double)agent.Position.X - observer.X, dy = (double)agent.Position.Y - observer.Y, dz = (double)agent.Position.Z - observer.Z;
            double distance = dx * dx + dy * dy + dz * dz;
            if (distance > limit) continue;
            int slot = count;
            while (slot > 0 && distance < output[slot - 1].DistanceSquared) --slot;
            if (slot >= output.Length) continue;
            int end = Math.Min(count, output.Length - 1);
            for (int moved = end; moved > slot; --moved) output[moved] = output[moved - 1];
            output[slot] = new() { Index = index, Position = agent.Position, Velocity = agent.Velocity, Mode = agent.Mode,
                Events = agent.Events, Fear = agent.Fear, DistanceSquared = distance };
            count = Math.Min(count + 1, output.Length);
        }
        return count;
    }
}
