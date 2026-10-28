/// @file    EditorFeatureTests.cs
/// @brief   設定候補 action と draw scope の隔離・容量・複製費用を実 Host で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Buffers.Binary;
using System.Linq;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class EditorFeatureTests
{
    internal static (string Name, Action Test)[] Cases => [
        ("EditorActionsUseNormalizedCandidateTransactionAndSnapshotHistory", Actions),
        ("GizmoPreviewIsolatesSettingsAndReportsLinearCloneCost", Gizmos),
        ("CallbackSuccessRemainsImmutableAcrossOwnedCleanupAndFailures", CallbackResults)
    ];
    static void Actions()
    {
        using World world = new("ManagedFixtures.CompletionComponent");
        Program.Check(world.Record.Type.Actions.Single().Key == "double-value", "Passive catalog must expose a stable action ID without invoking it.");
        using JsonDocument before = JsonDocument.Parse(ManagedSettingsSnapshot.Read(world.Record, 1, world.Id));
        string request = JsonSerializer.Serialize(new { targets = new[] { new {
            instanceId = world.Id.ToString(), expectedRevision = before.RootElement.GetProperty("revision").GetString(),
            fields = new { }, action = "double-value"
        } } });
        ManagedRuntimeResult prepared = world.Runtime.PrepareSettings(1, request, out ulong token);
        Program.Check(prepared.Success, prepared.Message);
        int Read(string key) => (int)world.Record.Type.Fields.Single(field => field.Name == key).Read(world.Record.Instance)!;
        Program.Check(Read("actionCalls") == 0 && Read("actionValue") == 2, "Action preview must not alter the live instance.");
        Program.Check(world.Runtime.PublishSettings(1, token, false).Success && Read("actionCalls") == 1 && Read("actionValue") == 4 && Read("actionCoupled") == 5,
            "Publishing must atomically apply action output and all coupled fields.");
        Program.Check(world.Runtime.PublishSettings(1, token, true).Success && Read("actionCalls") == 0 && Read("actionValue") == 2,
            "Undo must restore pre-action values.");
        Program.Check(world.Runtime.PublishSettings(1, token, false).Success && Read("actionCalls") == 1 && Read("actionValue") == 4,
            "Redo must restore the committed snapshot without re-running the action against the after value.");
        world.Runtime.DiscardSettings(1, token);
    }
    static void Gizmos()
    {
        using World world = new("ManagedFixtures.GizmoProbeComponent");
        ManagedRuntimeResult result = world.Runtime.DrawGizmos(1, world.Id, 101, 1, ScriptCallback.DRAW_GIZMOS, 3, out byte[] commands);
        Program.Check(result.Success && commands.Length == 160 && BinaryPrimitives.ReadUInt32LittleEndian(commands) == 3,
            "Line/Sphere/Box must produce a bounded fixed-size command packet through a real draw candidate.");
        Program.Check((float)world.Record.Type.Fields.Single(field => field.Name == "radius").Read(world.Record.Instance)! == 1f,
            "Draw candidate field assignments must never update live settings.");
        Program.Check(!world.Runtime.TryInvoke(1, world.Id, 101, 2, ScriptCallback.DRAW_GIZMOS).Success,
            "A draw callback must not be dispatched through the ordinary live-instance Invoke path.");
        Program.Check(!world.Runtime.DrawGizmos(1, world.Id, 101, 3, ScriptCallback.DRAW_GIZMOS, 2, out byte[] rejected).Success && rejected.Length == 0,
            "Command capacity failure must discard the entire candidate output.");
        int[] values = Enumerable.Range(1, 70000).ToArray();
        world.Record.Type.Fields.Single(field => field.Name == "samples").ReflectionField.SetValue(world.Record.Instance, values);
        long allocated = GC.GetAllocatedBytesForCurrentThread();
        Program.Check(world.Runtime.DrawGizmos(1, world.Id, 101, 4, ScriptCallback.DRAW_GIZMOS, 3, out byte[] first).Success,
            "Large settings remain supported in an isolated draw candidate.");
        long bytes = GC.GetAllocatedBytesForCurrentThread() - allocated;
        Console.WriteLine($"[MEASURE] GizmoPreview settings_items={values.Length} allocated_bytes={bytes}");
        Program.Check(values[0] == 1 && BitConverter.ToSingle(first, 20) == 1f,
            "Draw candidate array mutations must remain isolated while the geometry observes the current setting values.");
        values[0] = 7;
        Program.Check(world.Runtime.DrawGizmos(1, world.Id, 101, 5, ScriptCallback.DRAW_GIZMOS, 3, out byte[] next).Success
            && BitConverter.ToSingle(next, 20) == 7f && values[0] == 7,
            "Direct callback field changes must affect the next draw without a stale settings revision cache.");
        using JsonDocument catalog = JsonDocument.Parse(SettingsDocument.CatalogEntry(world.Record.Type));
        Program.Check(catalog.RootElement.GetProperty("gizmoPreview").GetProperty("cost").GetString() == "linear_in_total_settings_size",
            "Catalog consumers must see the cost and absence of private runtime caches.");
    }
    static void CallbackResults()
    {
        using World world = new("ManagedFixtures.HealthComponent");
        ManagedRuntimeResult first = world.Runtime.TryInvoke(1, world.Id, 101, 1, ScriptCallback.UPDATE);
        Program.Check(first.Success && first.Message.Length == 0, "An absent Update callback must retain its successful result.");
        ManagedRuntimeResult edited = first with { Status = BridgeStatus.INVALID_ARGUMENT, Message = "caller-local diagnostic" };
        Program.Check(!edited.Success && first.Success && first.Message.Length == 0, "A caller's record copy must not mutate the shared successful result.");
        Program.Check(world.Runtime.TryInvoke(1, world.Id, 101, 2, ScriptCallback.ON_DISABLE).Success,
            "Absent Disable callbacks must still perform owned cleanup and succeed.");
        ManagedRuntimeResult failed = world.Runtime.TryInvoke(1, world.Id, 999, 3, ScriptCallback.LATE_UPDATE);
        Program.Check(!failed.Success, "A different owner must not acquire a cached success.");
        ManagedRuntimeResult next = world.Runtime.TryInvoke(1, world.Id, 101, 4, ScriptCallback.LATE_UPDATE);
        Program.Check(next.Success && next.Message.Length == 0 && first.Success && first.Message.Length == 0,
            "Later success and failure must preserve the earlier immutable successful result.");
    }

    sealed class World : IDisposable
    {
        internal World(string type)
        {
            Runtime = new(new Program.RecordingBridge());
            Program.Check(Runtime.LoadCandidate(Program.FixturePath("V1"), 1, true).Success, "Editor feature fixtures must load.");
            Program.Check(Runtime.CreateInstance(1, type, 101, out ulong id).Success, "Editor feature fixture must construct.");
            Id = id; Program.BindFixture(Runtime, 1, Id);
            Program.Check(Runtime.TryInstance(1, Id, out ManagedScriptRuntime.ScriptInstance? record), "Editor feature fixture must bind.");
            Record = record!;
        }
        internal ManagedScriptRuntime Runtime { get; }
        internal ulong Id { get; }
        internal ManagedScriptRuntime.ScriptInstance Record { get; }
        public void Dispose() => Runtime.Dispose();
    }
}
