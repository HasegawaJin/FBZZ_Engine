/// @file    ScriptObjects.cs
/// @brief   保持した GameObject 群への一括ワールド位置操作。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;

namespace Fbzz;

/// @note 成功時も未設定・失効・指定による除外をスキップできる。失敗時の適用数とスキップ数は零。
public readonly struct BatchResult
{
    internal BatchResult(BatchStatus status, int input, int applied, int skipped)
    {
        Status = status; InputCount = input;
        AppliedCount = status == BatchStatus.OK ? applied : 0;
        SkippedCount = status == BatchStatus.OK ? skipped : 0;
    }

    public bool Succeeded => Status == BatchStatus.OK;
    public BatchStatus Status { get; }
    public int InputCount { get; }
    public int AppliedCount { get; }
    public int SkippedCount { get; }
}

/// @note 所有 Script の世代と Scene を毎回検証し、現在の同期 callback の権限で一括処理する。
public sealed class ScriptObjects
{
    internal ScriptObjects(ScriptBinding owner) { m_owner = owner; }
    readonly ScriptBinding m_owner;

    /// @note displacement はワールド軸のメートル単位。入力順・重複・即時親子同期は通常の反復操作と同じ。
    /// @return 要求全体を検証してから適用する。失敗時は対象を変更しない。
    [ApiContract(ParameterUnits = new[] { "displacement=m" }, ParameterCoordinates = new[] { "displacement=world_axes" }, Failure = "BatchResult_failure_leaves_all_targets_unchanged; missing_slots_skipped; ordered_duplicates_preserved; invalid_scope_throws")]
    public BatchResult TranslateWorld(ReadOnlySpan<GameObject> objects, Vector3 displacement, bool activeOnly = false)
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        if (!displacement.IsFinite) return new BatchResult(BatchStatus.INVALID_ARGUMENT, objects.Length, 0, 0);
        if (!MatchesScene(objects, scope.SceneLifetime)) return new BatchResult(BatchStatus.SCENE_MISMATCH, objects.Length, 0, 0);
        return scope.Bridge.TranslateWorld(scope, objects, displacement, activeOnly);
    }

    /// @note 有効な添字だけ positions を更新し、欠損位置と入力数を超えた出力の余りは変更しない。
    /// @return 要求全体を処理できた場合は true。失敗時は全出力を変更しない。
    [ApiContract(ParameterUnits = new[] { "positions=m" }, ParameterCoordinates = new[] { "positions=world_cached_render_pose" }, Failure = "false_leaves_all_outputs_unchanged; missing_positions_and_output_tails_unchanged; invalid_scope_throws")]
    public bool TryReadWorldPositions(ReadOnlySpan<GameObject> objects, Span<Vector3> positions, Span<ObjectStatus> statuses)
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        if (positions.Length < objects.Length || statuses.Length < objects.Length) return false;
        if (!MatchesScene(objects, scope.SceneLifetime)) return false;
        return scope.Bridge.ReadWorldPositions(scope, objects, positions[..objects.Length], statuses[..objects.Length]).Succeeded;
    }

    /// @note positions はワールド軸のメートル単位。有効な対象に使う位置だけ native で有限性を検証する。
    /// @return 要求全体を処理できた場合は true。失敗時は全対象と statuses を変更しない。
    [ApiContract(ParameterUnits = new[] { "positions=m" }, ParameterCoordinates = new[] { "positions=world" }, Failure = "false_leaves_targets_and_statuses_unchanged; ordered_duplicates_preserved; invalid_scope_throws")]
    public bool TrySetWorldPositions(ReadOnlySpan<GameObject> objects, ReadOnlySpan<Vector3> positions, Span<ObjectStatus> statuses)
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        if (positions.Length < objects.Length || statuses.Length < objects.Length) return false;
        if (!MatchesScene(objects, scope.SceneLifetime)) return false;
        return scope.Bridge.SetWorldPositions(scope, objects, positions[..objects.Length], statuses[..objects.Length]).Succeeded;
    }

    static bool MatchesScene(ReadOnlySpan<GameObject> objects, ulong sceneLifetime)
    {
        foreach (GameObject target in objects)
            if (target is not null && target.SceneLifetime != sceneLifetime) return false;
        return true;
    }
}
