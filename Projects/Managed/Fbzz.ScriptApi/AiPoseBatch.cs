/// @file    AiPoseBatch.cs
/// @brief   Commit caller-owned AI world positions and rotations with one native dispatch.
/// @author  Hasegawa Jin
/// @date    2026-10-11
using System;

namespace Fbzz.AI;

public static class AiPoseBatch
{
    /// @note At most 4096 slots; scale is preserved. Finite nonzero rotations are normalized by native.
    /// @note Null and expired slots are skipped; duplicate targets use the last pose. Ancestor/descendant target pairs fail.
    /// @return Failure leaves all targets and statuses unchanged; success only changes the first objects.Length statuses.
    [ApiContract(Call = "live", RunModes = new[] { "Play" }, ParameterUnits = new[] { "positions=m", "rotations=quaternion_xyzw" },
        ParameterCoordinates = new[] { "positions=world", "rotations=world" },
        Failure = "BatchResult_failure_leaves_all_targets_and_statuses_unchanged; invalid_scope_throws; ancestor_descendant_targets_rejected",
        Summary = "Atomically commits position and rotation for up to 4096 AI objects in one dispatch; local scale and status tail are preserved.")]
    public static BatchResult TryCommitWorldPoses(ReadOnlySpan<GameObject> objects, ReadOnlySpan<Vector3> positions,
        ReadOnlySpan<Quaternion> rotations, Span<ObjectStatus> statuses)
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        if (objects.Length > AiPoseWire.MAX_AGENTS || positions.Length < objects.Length || rotations.Length < objects.Length)
            return new(BatchStatus.INVALID_ARGUMENT, objects.Length, 0, 0);
        if (statuses.Length < objects.Length) return new(BatchStatus.BUFFER_TOO_SMALL, objects.Length, 0, 0);
        foreach (GameObject item in objects)
            if (item is not null && item.SceneLifetime != scope.SceneLifetime)
                return new(BatchStatus.SCENE_MISMATCH, objects.Length, 0, 0);
        if (scope.Bridge is not IScriptAiPoseNativeBridge bridge)
            throw new NotSupportedException("AI pose batch services are unavailable.");
        return bridge.CommitWorldPoses(scope, objects, positions[..objects.Length], rotations[..objects.Length], statuses[..objects.Length]);
    }
}

internal interface IScriptAiPoseNativeBridge
{
    BatchResult CommitWorldPoses(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects,
        ReadOnlySpan<Vector3> positions, ReadOnlySpan<Quaternion> rotations, Span<ObjectStatus> statuses);
}

internal static class AiPoseWire
{
    internal const int MAX_AGENTS = 4096, HEADER_BYTES = 4, ROW_BYTES = 44, STATUS_BYTES = 4;
}
