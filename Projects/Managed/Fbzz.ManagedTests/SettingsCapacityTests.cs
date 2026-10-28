/// @file    SettingsCapacityTests.cs
/// @brief   Caller-sized large settings, incremental snapshots, exact references and spooled Undo values.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class SettingsCapacityTests
{
    internal static (string Name, Action Test)[] Cases => [
        ("SettingsCapacityRoundtripsBeyondFormerElementAndByteLimits", Roundtrip),
        ("SettingsCapacityPagesAndTextUseImmutableOwnedSpool", Pages),
        ("SettingsCapacityLaterLeafAndSpilledHistoryUndoWithoutLoss", Transactions),
        ("SettingsSnapshotStorageCleanupAndCollectibleTypes", StorageAndTypes)
    ];
    [Serializable] sealed class Large
    {
        public int[] values = Enumerable.Range(0, 40001).ToArray();
        public List<int> list = Enumerable.Range(0, 40001).ToList();
        public string text = new('文', 1024 * 1024 / 3 + 23);
        public GameObject[] references = [];
    }
    static void Roundtrip()
    {
        Large source = new();
        string json = StructuredFieldCodec.Encode(source, typeof(Large));
        Program.Check(System.Text.Encoding.UTF8.GetByteCount(json) > 1024 * 1024, "The regression must exceed the former total byte boundary.");
        Large restored = (Large)StructuredFieldCodec.Decode(json, typeof(Large), _ => throw new InvalidDataException())!;
        Program.Check(restored.values.Length == 40001 && restored.values[4096] == 4096 && restored.values[32768] == 32768 && restored.values[^1] == 40000
            && restored.list.Count == 40001 && restored.list[^1] == 40000 && restored.text == source.text,
            "Saved arrays/lists/text retain every value beyond former display and transfer limits.");
    }
    static void Pages()
    {
        Large source = new(); source.references = [new GameObject(202, 9, "same"), new GameObject(303, 8, "same")];
        using ImmutableValueSnapshot small = ImmutableValueSnapshot.Capture(17, typeof(int));
        Program.Check(!small.IsSpooled, "Small values should remain lightweight in resident storage.");
        using ImmutableValueSnapshot directSpool = ImmutableValueSnapshot.Capture(new int[10000], typeof(int[]), new(ResidentBytes: 1));
        Program.Check(directSpool.IsSpooled && directSpool.ResidentBytes == 0,
            "A resident budget smaller than one charged cache slot uses the direct spool path without exceeding its budget.");
        using ImmutableValueSnapshot snapshot = ImmutableValueSnapshot.Capture(source, typeof(Large), new(ResidentBytes: 32768));
        Program.Check(snapshot.IsSpooled && snapshot.StorageBytes > 1024 * 1024 && snapshot.ResidentBytes is > 0 and <= 32768,
            "Large immutable values spill while their bounded page cache stays within the explicit resident budget.");
        using ImmutableValueSnapshot memorySnapshot = ImmutableValueSnapshot.Capture(source, typeof(Large), new(ResidentBytes: 4 * 1024 * 1024));
        Program.Check(!memorySnapshot.IsSpooled && memorySnapshot.Revision == snapshot.Revision,
            "Paged spill storage must preserve the exact binary revision produced by in-memory capture.");
        source.values[40000] = -1; source.list.Clear(); source.text = "changed";
        using JsonDocument page = JsonDocument.Parse(JsonSerializer.Serialize(snapshot.ReadPage("/values", 39990, 32, 4096)));
        Program.Check(page.RootElement.GetProperty("total").GetInt64() == 40001 && page.RootElement.GetProperty("returned").GetInt32() == 11
            && page.RootElement.GetProperty("items")[10].GetInt32() == 40000 && page.RootElement.GetProperty("nextStart").ValueKind == JsonValueKind.Null,
            "Later pages seek captured offsets directly and never mix later live mutations into the immutable observation.");
        using JsonDocument listPage = JsonDocument.Parse(JsonSerializer.Serialize(snapshot.ReadPage("/list", 39999, 2, 4096)));
        Program.Check(listPage.RootElement.GetProperty("returned").GetInt32() == 2
            && listPage.RootElement.GetProperty("items")[0].GetInt32() == 39999 && listPage.RootElement.GetProperty("items")[1].GetInt32() == 40000,
            "Typed List<int> capture retains the tail values without boxing them through IList.");
        using JsonDocument refs = JsonDocument.Parse(JsonSerializer.Serialize(snapshot.ReadPage("/references", 0, 2, 4096)));
        Program.Check(refs.RootElement.GetProperty("references").GetProperty("/references/1").GetProperty("objectId").GetString() == "303"
            && refs.RootElement.GetProperty("references").GetProperty("/references/1").GetProperty("sceneLifetime").GetString() == "8",
            "Each emitted reference page carries the exact captured token, including stale same-GUID references.");
        Large restored = (Large)memorySnapshot.RestoreValue(typeof(Large))!;
        Program.Check(restored.values.SequenceEqual(Enumerable.Range(0, 40001)) && restored.list.SequenceEqual(Enumerable.Range(0, 40001))
            && restored.references.Length == 2 && restored.references[1].ObjectId == 303 && restored.references[1].SceneLifetime == 8,
            "Lazy path materialization preserves canonical array/List values and exact nested reference identities.");
        bool pathReported = false;
        try { using ImmutableValueSnapshot invalid = ImmutableValueSnapshot.CaptureFields([("a/b~", (object?)new[] { float.NaN }, typeof(float[]))]); }
        catch (InvalidDataException exception) { pathReported = exception.Message.Contains("/a~1b~0/0", StringComparison.Ordinal); }
        Program.Check(pathReported, "Error paths remain RFC 6901 escaped and materialize only when validation fails.");
        long offset = 0, bytes = 0;
        while (true) {
            using JsonDocument chunk = JsonDocument.Parse(JsonSerializer.Serialize(snapshot.ReadPage("/text", offset, 777, 4096)));
            string text = chunk.RootElement.GetProperty("text").GetString()!;
            Program.Check(text.All(character => character == '文'), "Text chunks must retain whole UTF-8 characters.");
            bytes += chunk.RootElement.GetProperty("returned").GetInt32();
            JsonElement next = chunk.RootElement.GetProperty("nextStart"); if (next.ValueKind == JsonValueKind.Null) break; offset = next.GetInt64();
        }
        Program.Check(bytes > 1024 * 1024, "Text chunk continuation must cover the entire oversized string.");
        Program.Throws<IOException>(() => ImmutableValueSnapshot.Capture(new int[40001], typeof(int[]), new(ResidentBytes: 0, SpoolBytes: 128)));
        GameObject?[] sparse = new GameObject?[1500]; sparse[0] = new GameObject(404, 8, "first"); sparse[^1] = new GameObject(505, 8, "last");
        using ImmutableValueSnapshot sparseSnapshot = ImmutableValueSnapshot.Capture(sparse, typeof(GameObject[]), new(ResidentBytes: 8192));
        using JsonDocument sparsePage = JsonDocument.Parse(JsonSerializer.Serialize(sparseSnapshot.ReadPage("", 1497, 3, 4096)));
        Program.Check(sparseSnapshot.IsSpooled && sparseSnapshot.ResidentBytes is > 0 and <= 8192 && sparsePage.RootElement.GetProperty("items")[0].ValueKind == JsonValueKind.Null
            && sparsePage.RootElement.GetProperty("items")[2].GetProperty("$go").GetString() == "last",
            "Paged snapshots preserve null slots between populated values across spool index pages.");
        using ImmutableValueSnapshot escaped = ImmutableValueSnapshot.CaptureFields([("a/b~", (object?)new[] { 4, 5 }, typeof(int[]))]);
        using JsonDocument nested = JsonDocument.Parse(JsonSerializer.Serialize(escaped.ReadPage("/a~1b~0", 1, int.MaxValue, 4096)));
        Program.Check(nested.RootElement.GetProperty("items")[0].GetInt32() == 5, "Snapshot paths must decode escaped slash and tilde keys with an oversized requested count safely.");
        Program.Throws<InvalidDataException>(() => escaped.ReadPage("/a~1b~0", long.MaxValue, 1, 4096));
        using ImmutableValueSnapshot emoji = ImmutableValueSnapshot.Capture("A😀文B😀", typeof(string));
        long cursor = 0; string joined = "";
        while (true) {
            using JsonDocument chunk = JsonDocument.Parse(JsonSerializer.Serialize(emoji.ReadPage("", cursor, 5, 4096)));
            joined += chunk.RootElement.GetProperty("text").GetString();
            JsonElement next = chunk.RootElement.GetProperty("nextStart"); if (next.ValueKind == JsonValueKind.Null) break; cursor = next.GetInt64();
        }
        Program.Check(joined == "A😀文B😀", "UTF-8 byte continuation must retain surrogate-pair and multibyte characters across nonaligned page boundaries.");
        Program.Throws<InvalidDataException>(() => emoji.ReadPage("", 2, 5, 4096));
    }
    sealed class Actor : Script
    {
        public Large settings = new();
    }
    static void Transactions()
    {
        using ManagedScriptRuntime runtime = new(new Program.RecordingBridge());
        FieldInfo field = typeof(Actor).GetField("settings")!;
        ScriptFieldDescriptor setting = new(field, "settings", ScriptFieldKind.STRUCTURED, true, false, []);
        ScriptTypeDescriptor descriptor = new(typeof(Actor), typeof(Actor).GetConstructor(Type.EmptyTypes)!, [setting], []);
        Actor actor = new(); ScriptBinding binding = new(1, 1, 101) { GameObject = new GameObject(101, 9, "owner") };
        ManagedScriptRuntime.ScriptInstance record = new(1, 101, descriptor, actor, binding, []);
        var records = (Dictionary<ulong, ManagedScriptRuntime.ScriptInstance>)typeof(ManagedScriptRuntime).GetField("m_instances", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(runtime)!;
        records.Add(1, record);
        string Revision() { using JsonDocument document = JsonDocument.Parse(ManagedSettingsSnapshot.Read(record, 1, 1)); return document.RootElement.GetProperty("revision").GetString()!; }
        string request = JsonSerializer.Serialize(new { targets = new[] { new { instanceId = "1", expectedRevision = Revision(), fields = new Dictionary<string, object?>(), patches = new[] { new { path = "/settings/values/40000", value = -123 } } } } });
        ManagedRuntimeResult prepared = runtime.PrepareSettings(1, request, out ulong token); Program.Check(prepared.Success, prepared.Message);
        Program.Check(runtime.PublishSettings(1, token, false).Success && actor.settings.values[^1] == -123, "A leaf beyond former cutoffs must publish atomically.");
        actor.settings.list[^1] = 888;
        Program.Check(runtime.PublishSettings(1, token, true).Success && actor.settings.values[^1] == 40000 && actor.settings.list[^1] == 888, "Large document Undo restores the selected leaf while retaining unrelated later values.");
        Program.Check(runtime.PublishSettings(1, token, false).Success && actor.settings.values[^1] == -123 && actor.settings.text.Length > 300000, "Redo must preserve all omitted oversized settings.");
        runtime.DiscardSettings(1, token);
        string changedText = new('x', 1024 * 1024 + 23);
        request = JsonSerializer.Serialize(new { targets = new[] { new { instanceId = "1", expectedRevision = Revision(), fields = new Dictionary<string, object?>(), patches = new[] { new { path = "/settings/text", value = changedText } } } } });
        prepared = runtime.PrepareSettings(1, request, out token); Program.Check(prepared.Success, prepared.Message);
        Program.Check(runtime.PublishSettings(1, token, false).Success && actor.settings.text == changedText && runtime.SettingsTransactionBytes < 200000,
            "Oversized changed values use owned spill storage; resident history does not grow with the full string length.");
        Program.Check(runtime.PublishSettings(1, token, true).Success && actor.settings.text.All(character => character == '文'), "Spilled Undo restores the complete prior oversized value.");
        Program.Check(runtime.PublishSettings(1, token, false).Success && actor.settings.text == changedText, "Spilled Redo restores the complete normalized value without callbacks.");
        runtime.DiscardSettings(1, token); runtime.ReleaseInstance(1, 1);
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    static (ImmutableValueSnapshot Snapshot, WeakReference Type) CaptureCollectible()
    {
        AssemblyBuilder assembly = AssemblyBuilder.DefineDynamicAssembly(new AssemblyName("SnapshotCollectibleProbe"), AssemblyBuilderAccess.RunAndCollect);
        TypeBuilder builder = assembly.DefineDynamicModule("values").DefineType("CapturedValue", TypeAttributes.Public);
        builder.DefineDefaultConstructor(MethodAttributes.Public);
        builder.SetCustomAttribute(new CustomAttributeBuilder(typeof(SerializableAttribute).GetConstructor(Type.EmptyTypes)!, []));
        builder.DefineField("value", typeof(int), FieldAttributes.Public);
        builder.DefineField("values", typeof(int[]), FieldAttributes.Public); Type type = builder.CreateType()!;
        object value = Activator.CreateInstance(type)!; type.GetField("value")!.SetValue(value, 41);
        type.GetField("values")!.SetValue(value, Enumerable.Range(0, 40001).ToArray());
        return (ImmutableValueSnapshot.Capture(value, type, new(ResidentBytes: 32768)), new WeakReference(type));
    }
    static void StorageAndTypes()
    {
        byte[] expected = Enumerable.Range(0, 65537).Select(index => (byte)(index * 31 + 7)).ToArray();
        using (ImmutableValueSnapshot.SnapshotStream stream = new(new(ResidentBytes: 8192))) {
            stream.Write(expected);
            byte[] patch = [19, 23, 29, 31, 37, 41, 43]; patch.CopyTo(expected, 4093);
            stream.Position = 4093; stream.Write(patch);
            stream.Position = 0; byte[] actual = new byte[expected.Length]; stream.ReadExactly(actual);
            Program.Check(stream.IsSpooled && stream.ResidentBytes <= 8192 && actual.AsSpan().SequenceEqual(expected)
                && SHA256.HashData(actual).AsSpan().SequenceEqual(SHA256.HashData(expected)),
                "A write larger than the page cache preserves every byte across dirty eviction and a later backpatch read.");
            stream.Dispose(); stream.Dispose();
            Program.Check(stream.ResidentBytes == 0, "Repeated disposal releases a cached spool and reports no resident pages.");
        }
        using (ImmutableValueSnapshot.SnapshotStream stream = new(new(ResidentBytes: 8192))) {
            stream.SetLength(16384); byte[] prefix = [3, 5, 7, 11, 13, 17, 19, 23]; stream.Write(prefix);
            stream.SetLength(4100); stream.SetLength(8192); stream.Position = 0;
            byte[] actual = new byte[8192]; stream.ReadExactly(actual);
            Program.Check(stream.IsSpooled && actual.AsSpan(0, prefix.Length).SequenceEqual(prefix) && actual.Skip(prefix.Length).All(value => value == 0),
                "Spool shrink and extension retain the prefix, clear removed bytes and expose zero-filled extension through EOF.");
        }
        using (ImmutableValueSnapshot.SnapshotStream stream = new(new(ResidentBytes: 0))) {
            stream.Write([1, 2, 3]); stream.Dispose(); stream.Dispose();
            Program.Check(stream.IsSpooled && stream.ResidentBytes == 0, "Repeated disposal is safe for an uncached direct spool.");
        }
        var capture = CaptureCollectible();
        using (capture.Snapshot) {
            Program.Check(capture.Snapshot.IsSpooled && capture.Snapshot.ResidentBytes is > 0 and <= 32768,
                "Spool page caching must remain budgeted while retaining no collectible user metadata.");
            for (int index = 0; index < 4 && capture.Type.IsAlive; index++) { GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect(); }
            Program.Check(!capture.Type.IsAlive, "An immutable retained snapshot must not pin its collectible game Type or FieldInfo.");
            using JsonDocument scalar = JsonDocument.Parse(JsonSerializer.Serialize(capture.Snapshot.ReadValue("/value", 4096)));
            using JsonDocument page = JsonDocument.Parse(JsonSerializer.Serialize(capture.Snapshot.ReadPage("/values", 40000, 1, 4096)));
            Program.Check(scalar.RootElement.GetProperty("value").GetInt32() == 41 && page.RootElement.GetProperty("items")[0].GetInt32() == 40000,
                "Cached spooled primitive data remains readable after its declaring game type is collected.");
        }
        string directory = Path.Combine(Path.GetTempPath(), "fbzz-snapshot-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory); string unrelated = Path.Combine(directory, "unrelated.txt"); File.WriteAllText(unrelated, "keep");
        try {
            using (ImmutableValueSnapshot snapshot = ImmutableValueSnapshot.Capture(new int[32769], typeof(int[]), new(ResidentBytes: 8192, Directory: directory)))
                Program.Check(snapshot.IsSpooled && snapshot.ResidentBytes is > 0 and <= 8192 && Directory.GetFiles(directory).Length == 2,
                    "A cached snapshot stays within its declared resident budget and owns a unique spool alongside unrelated files.");
            Program.Check(Directory.GetFiles(directory).SequenceEqual(new[] { unrelated }), "Dispose deletes only its owned spool.");
            Program.Throws<IOException>(() => ImmutableValueSnapshot.Capture(new int[32769], typeof(int[]), new(ResidentBytes: 0, SpoolBytes: 128, Directory: directory)));
            Program.Check(Directory.GetFiles(directory).SequenceEqual(new[] { unrelated }), "Failed capture cleans its own temporary storage and preserves unrelated files.");
            Program.Throws<IOException>(() => ImmutableValueSnapshot.Capture(new string('x', 200000), typeof(string),
                new(ResidentBytes: 32768, SpoolBytes: 100000, Directory: directory)));
            Program.Check(Directory.GetFiles(directory).SequenceEqual(new[] { unrelated }), "A failed cached spill flush deletes only its own spool.");
        } finally { File.Delete(unrelated); Directory.Delete(directory); }
    }
}
