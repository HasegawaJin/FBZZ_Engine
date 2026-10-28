/// @file    ObservationTests.cs
/// @brief   巨大値の選択 capture・範囲読み取り・UTF8 continuation と観測の寿命を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class ObservationTests
{
    internal static (string Name, Action Test)[] Cases => [
        ("ObservationPagesKeepLargeSelectedListImmutable", LargeList),
        ("ObservationTextChunksKeepUtf8Boundaries", LargeText),
        ("ObservationHandlesHaveExactLifetimeAndResourceOwnership", Lifetime)
    ];
    static void LargeList()
    {
        using World world = new();
        object settings = world.Instance.GetType().GetField("settings")!.GetValue(world.Instance)!;
        List<float> values = Enumerable.Range(0, 70000).Select(index => (float)index).ToList();
        settings.GetType().GetField("weights")!.SetValue(settings, values);
        world.Runtime.ConfigureObservations(new(ResidentBytes: 128));
        Program.Check(world.Runtime.OpenObservation(1, world.Id, "/settings/weights", out ulong handle).Success, "A list beyond 32768 nodes must capture without whole-list JSON encoding.");
        values[66000] = -1;
        using JsonDocument page = world.Page(handle, "/settings/weights", 66000, 3);
        JsonElement root = page.RootElement;
        Program.Check(root.GetProperty("items")[0].GetSingle() == 66000 && root.GetProperty("total").GetInt64() == 70000
            && root.GetProperty("nextStart").GetInt64() == 66003 && root.GetProperty("spooled").GetBoolean()
            && root.GetProperty("sample").GetString() == "snapshot", "Paging must use immutable values and explicit total/range/continuation after live mutations.");
        ScriptFieldDescriptor field = world.Runtime.GetCatalog(1)!.Find("ManagedFixtures.CompletionComponent")!.Fields.Single(value => value.Name == "settings");
        using JsonDocument summary = JsonDocument.Parse(ManagedScriptRuntime.ObservationSummary(world.Record, field));
        Program.Check(summary.RootElement.GetProperty("value").GetProperty("weights").GetProperty("$deferred").GetString() == "/settings/weights"
            && world.Runtime.ObservationCount == 1, "Default summary must defer large values without creating additional snapshot handles.");
        Program.Check(world.Runtime.CloseObservation(1, world.Id, handle).Success && world.Runtime.ObservationCount == 0, "Explicit close must dispose snapshot storage.");
        world.Instance.GetType().GetField("observationArray")!.SetValue(world.Instance, Enumerable.Range(0, 70000).ToArray());
        Program.Check(world.Runtime.OpenObservation(1, world.Id, "/array~1~0values", out ulong array).Success,
            "Classic arrays must use the same arbitrary practical capacity and escaped pointer contract as Lists.");
        using JsonDocument arrayPage = world.Page(array, "/array~1~0values", 69000, 2);
        Program.Check(arrayPage.RootElement.GetProperty("items")[1].GetInt32() == 69001, "Array pages must retain offsets beyond former total-item cutoffs.");
        settings.GetType().GetField("target")!.SetValue(settings, new GameObject(202, 1, "target-guid"));
        Program.Check(world.Runtime.OpenObservation(1, world.Id, "/settings/target", out ulong target).Success, "Exact references may be selected as scalar observations.");
        using JsonDocument reference = world.Page(target, "/settings/target", 0, 1);
        JsonElement token = reference.RootElement.GetProperty("references").GetProperty("/settings/target");
        Program.Check(token.GetProperty("objectId").GetString() == "202" && token.GetProperty("sceneLifetime").GetString() == "1",
            "Returned exact reference paths must include the selected root and retained runtime lifetime.");
    }
    static void LargeText()
    {
        using World world = new();
        object settings = world.Instance.GetType().GetField("settings")!.GetValue(world.Instance)!;
        settings.GetType().GetField("label")!.SetValue(settings, new string('あ', 700000));
        world.Runtime.ConfigureObservations(new(ResidentBytes: 128));
        Program.Check(world.Runtime.OpenObservation(1, world.Id, "/settings/label", out ulong handle).Success, "A string larger than 1 MiB must capture in spool storage.");
        settings.GetType().GetField("label")!.SetValue(settings, "mutated");
        using JsonDocument first = world.Page(handle, "/settings/label", 0, 17);
        Program.Check(first.RootElement.GetProperty("text").GetString() == new string('あ', 5)
            && first.RootElement.GetProperty("unit").GetString() == "utf8_bytes" && first.RootElement.GetProperty("nextStart").GetInt64() == 15,
            "Text paging must trim complete UTF8 characters and preserve the selected snapshot.");
        using JsonDocument next = world.Page(handle, "/settings/label", 15, 17);
        Program.Check(next.RootElement.GetProperty("text").GetString() == new string('あ', 5), "Text continuation must start at the previous returned byte offset.");
        Program.Check(!world.Runtime.ReadObservation(1, world.Id, handle, "/settings/label", 1, 10, 4096, out _).Success,
            "UTF8 continuation must reject a mid-character offset without altering retained data.");
        Program.Check(world.Runtime.OpenObservation(1, world.Id, "/rotation/3", out ulong scalar).Success, "Direct math component paths must match immutable snapshot array coordinates.");
        using JsonDocument component = world.Page(scalar, "/rotation/3", 0, 1);
        Program.Check(component.RootElement.GetProperty("value").GetSingle() == 1f, "Known SDK Quaternion components must be read without user getters.");
    }
    static void Lifetime()
    {
        using World world = new();
        long now = 10;
        world.Runtime.ObservationClock = () => now;
        world.Runtime.ConfigureObservations(new(HandleCapacity: 1, ResidentBytes: 128, LifetimeMilliseconds: 5));
        Program.Check(world.Runtime.OpenObservation(1, world.Id, "/settings/weights", out ulong first).Success, "A bounded resource policy must permit its first snapshot.");
        Program.Check(world.Runtime.ReadObservation(1, world.Id, first, "/settings/weights", 0, 1, 1024, out _).Status == BridgeStatus.INVALID_ARGUMENT,
            "Wire capacity below the shared packet minimum must reject consistently.");
        Program.Check(world.Runtime.ReadObservation(1, world.Id, first, "/settings/weights", 0, 1, 2048, out _).Success,
            "The declared minimum wire capacity must hold a normal observation packet.");
        Program.Check(world.Runtime.ReadObservation(2, world.Id, first, "/settings/weights", 0, 1, 4096, out _).Status == BridgeStatus.STALE_GENERATION
            && world.Runtime.ObservationCount == 1, "Wrong caller generation must reject without destroying another caller's retained snapshot.");
        Program.Check(world.Runtime.OpenObservation(1, world.Id, "/settings/weights", out _).Status == BridgeStatus.PENDING_LIMIT,
            "Handle exhaustion must report a resource condition, never silently discard collection data.");
        now = 14;
        Program.Check(world.Runtime.ReadObservation(1, world.Id, first, "/settings/weights", 0, 1, 4096, out _).Success,
            "Successful paging must extend idle expiry.");
        now = 18;
        Program.Check(world.Runtime.ReadObservation(1, world.Id, first, "/settings/weights", 0, 1, 4096, out _).Success,
            "An active page sequence must remain available beyond its original absolute timeout.");
        now = 23;
        Program.Check(world.Runtime.ReadObservation(1, world.Id, first, "/settings/weights", 0, 1, 4096, out _).Status == BridgeStatus.NOT_FOUND
            && world.Runtime.ObservationCount == 0, "Expiry must dispose owned storage without a wall-clock test wait.");
        Program.Check(world.Runtime.OpenObservation(1, world.Id, "/settings/weights", out ulong rebound).Success, "Expired capacity must become reusable.");
        Program.BindFixture(world.Runtime, 1, world.Id, objectId: 202, guid: "target-guid");
        Program.Check(world.Runtime.ReadObservation(1, world.Id, rebound, "/settings/weights", 0, 1, 4096, out _).Status == BridgeStatus.STALE_GENERATION,
            "Changing an owner runtime identity must expire its previous observation even in the same code generation.");
        Program.Check(world.Runtime.OpenObservation(1, world.Id, "/settings/weights", out _).Success, "A rebound exact owner may open its own new snapshot.");
        Program.Check(world.Runtime.ReleaseInstance(1, world.Id).Success && world.Runtime.ObservationCount == 0, "Script release must dispose its retained observation handles.");
    }
    sealed class World : IDisposable
    {
        internal World()
        {
            Runtime = new(new Program.RecordingBridge());
            Program.Check(Runtime.LoadCandidate(Program.FixturePath("V1"), 1, true).Success, "Observation fixture must load.");
            Program.Check(Runtime.CreateInstance(1, "ManagedFixtures.CompletionComponent", 101, out ulong id).Success, "Observation fixture must construct.");
            Id = id; Program.BindFixture(Runtime, 1, Id);
            Program.Check(Runtime.TryInstance(1, Id, out ManagedScriptRuntime.ScriptInstance? record), "Observation fixture must bind.");
            Record = record!;
        }
        internal ManagedScriptRuntime Runtime { get; }
        internal ulong Id { get; }
        internal ManagedScriptRuntime.ScriptInstance Record { get; }
        internal Script Instance => Record.Instance;
        internal JsonDocument Page(ulong handle, string path, long start, int count)
        {
            ManagedRuntimeResult result = Runtime.ReadObservation(1, Id, handle, path, start, count, 4096, out string json);
            Program.Check(result.Success, result.Message);
            return JsonDocument.Parse(json);
        }
        public void Dispose() => Runtime.Dispose();
    }
}
