/// @file    SettingsPageAndUploadTests.cs
/// @brief   Bounded live pages and checksum/replay checked uploads publish only complete normalized settings.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class SettingsPageAndUploadTests
{
    internal static (string Name, Action Test)[] Cases => [
        ("SettingsLivePageReadsOnlyRequestedRangeWithoutSnapshotValidation", LivePages),
        ("SettingsChunkUploadIsAtomicReplayCheckedAndDisposable", Upload),
        ("SettingsStructuralIntentMovesExactDuplicateReferences", Structural),
        ("SettingsFailedSpilledUploadReleasesStorageAndExhaustedHandlesNeverWrap", FailedStorage),
        ("SettingsStagedMultiTargetBatchIsAtomicAndPreservesUneditedLeaves", MultiTarget),
        ("SettingsAdoptionCaptureIsImmutableSpooledAndExportsSelectedValuesWithoutCallbacks", AdoptionCapture),
        ("SettingsAdoptionSurvivesSourceReleaseAndOwnsIndependentCheckedUndo", AdoptionImport),
        ("SettingsReconnectPathsHaveNoTotalArrayCutoff", ReconnectPaths)
    ];
    [Serializable] sealed class Node { public Node? next = null; public int value = 7; }
    sealed class Actor : Script
    {
        public int[] values = Enumerable.Range(0, 40001).ToArray();
        public int[] compactValues = Enumerable.Range(0, 128).ToArray();
        public string text = new('x', 1024 * 1024 + 23);
        public string boundaryText = new string('a', 63) + "😀" + new string('b', 130);
        public Node? broken = null;
        public GameObject[] targets = [];
        public int validations = 0;
        protected override void OnValidate() { validations++; }
    }
    sealed class World : IDisposable
    {
        internal readonly ManagedScriptRuntime Runtime = new(new Program.RecordingBridge());
        internal readonly Actor Value = new();
        internal readonly ManagedScriptRuntime.ScriptInstance Record;
        internal World(string savedScriptId = "")
        {
            ScriptFieldDescriptor[] fields = typeof(Actor).GetFields(BindingFlags.Instance | BindingFlags.Public).Select(field =>
                new ScriptFieldDescriptor(field, field.Name, ScriptCatalogBuilder.GetKind(field.FieldType), true, false, [])).ToArray();
            ScriptCallbackDescriptor callback = new(ScriptCallback.ON_VALIDATE, typeof(Actor).GetMethod("OnValidate", BindingFlags.NonPublic | BindingFlags.Instance)!);
            ScriptTypeDescriptor type = new(typeof(Actor), typeof(Actor).GetConstructor(Type.EmptyTypes)!, fields, [callback]);
            ScriptBinding binding = new(1, 1, 101, savedScriptId) { GameObject = new GameObject(101, 9, "owner") };
            Record = new(1, 101, type, Value, binding, []);
            var instances = (Dictionary<ulong, ManagedScriptRuntime.ScriptInstance>)typeof(ManagedScriptRuntime).GetField("m_instances", BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(Runtime)!;
            instances.Add(1, Record);
        }
        internal string Envelope(string revision, params object[] patches) => JsonSerializer.Serialize(new {
            targets = new[] { new { instanceId = "1", expectedRevision = revision, fields = new Dictionary<string, object?>(), patches } }
        });
        public void Dispose() { Runtime.ReleaseInstance(1, 1); Runtime.Dispose(); }
    }
    static void Success(ManagedRuntimeResult result) => Program.Check(result.Success, result.Message);
    static void LivePages()
    {
        using World world = new(); Node cyclic = new(); cyclic.next = cyclic; world.Value.broken = cyclic;
        world.Value.text += "\uD800";
        using JsonDocument summary = JsonDocument.Parse(SettingsLivePages.Read(world.Record, "", 0, 8, 4096));
        JsonElement values = summary.RootElement.GetProperty("items").EnumerateArray().Single(item => item.GetProperty("name").GetString() == "values").GetProperty("value");
        Program.Check(values.GetProperty("$deferred").GetString() == "/values" && values.GetProperty("total").GetInt32() == 40001
            && summary.RootElement.GetProperty("sample").GetString() == "request", "Collapsed array display returns a bounded descriptor without traversing or validating unrelated cyclic/invalid fields.");
        using JsonDocument page = JsonDocument.Parse(SettingsLivePages.Read(world.Record, "/values", 39997, 64, 4096));
        Program.Check(page.RootElement.GetProperty("returned").GetInt32() == 4 && page.RootElement.GetProperty("items")[3].GetInt32() == 40000,
            "Live page indexing reaches later caller-owned slots without capturing the full parent array.");
        world.Value.values[^1] = -17;
        using JsonDocument next = JsonDocument.Parse(SettingsLivePages.Read(world.Record, "/values", 40000, 1, 4096));
        Program.Check(next.RootElement.GetProperty("items")[0].GetInt32() == -17 && world.Runtime.SettingsEditSessionCount == 0 && world.Runtime.SettingsTransactionCount == 0,
            "Live pages read their request-time values even when direct field writes do not change Host counters, and allocate no edit sessions/history.");
        using JsonDocument text = JsonDocument.Parse(SettingsLivePages.Read(world.Record, "/text", 0, 64, 4096));
        Program.Check(text.RootElement.GetProperty("text").GetString() == new string('x', 64), "Visible text pages never scan or encode invalid characters outside their requested range.");
        int textOffset = 0;
        StringBuilder rebuilt = new();
        while (textOffset < world.Value.boundaryText.Length) {
            using JsonDocument boundary = JsonDocument.Parse(SettingsLivePages.Read(world.Record, "/boundaryText", textOffset, 64, 1024));
            JsonElement packet = boundary.RootElement;
            int start = packet.GetProperty("start").GetInt32();
            int returned = packet.GetProperty("returned").GetInt32();
            JsonElement textContinuation = packet.GetProperty("nextStart");
            int nextOffset = start + returned;
            Program.Check(start == textOffset && returned > 0 &&
                (textContinuation.ValueKind == JsonValueKind.Null ? nextOffset == world.Value.boundaryText.Length : textContinuation.GetInt32() == nextOffset),
                "Each UTF-16 text page reports its exact next offset or marks the final page.");
            string chunk = packet.GetProperty("text").GetString()!;
            Program.Check(chunk.Length == returned && !(char.IsHighSurrogate(chunk[^1]) ||
                (start > 0 && char.IsLowSurrogate(chunk[0]))), "Paged text never ends or begins inside a surrogate pair.");
            if (textOffset == 0) Program.Check(returned == 63, "A page boundary before a supplementary character returns the shorter valid page.");
            rebuilt.Append(chunk); textOffset = nextOffset;
        }
        Program.Check(rebuilt.ToString() == world.Value.boundaryText, "Following actual text-page offsets reconstructs every UTF-16 code unit without skips.");
        int itemOffset = 0;
        List<int> collectedValues = [];
        bool observedShortPage = false;
        while (itemOffset < world.Value.compactValues.Length) {
            using JsonDocument compact = JsonDocument.Parse(SettingsLivePages.Read(world.Record, "/compactValues", itemOffset, 64, 1024));
            JsonElement packet = compact.RootElement;
            int start = packet.GetProperty("start").GetInt32();
            int returned = packet.GetProperty("returned").GetInt32();
            JsonElement itemContinuation = packet.GetProperty("nextStart");
            int nextOffset = start + returned;
            Program.Check(start == itemOffset && returned > 0 &&
                (itemContinuation.ValueKind == JsonValueKind.Null ? nextOffset == world.Value.compactValues.Length : itemContinuation.GetInt32() == nextOffset),
                "Byte-bounded collection pages return an exact next offset or mark the final page.");
            observedShortPage |= returned < 64;
            collectedValues.AddRange(packet.GetProperty("items").EnumerateArray().Select(item => item.GetInt32()));
            itemOffset = nextOffset;
        }
        Program.Check(observedShortPage && collectedValues.SequenceEqual(world.Value.compactValues),
            "Following short byte-budgeted collection pages neither repeats nor skips items.");
    }
    static string Packet(long offset, byte[] bytes, bool final, string? hash = null) => JsonSerializer.Serialize(new {
        offset, data = Convert.ToBase64String(bytes), sha256 = hash ?? Convert.ToHexString(SHA256.HashData(bytes)), final
    });
    static void Upload()
    {
        using World world = new(); Success(world.Runtime.OpenSettingsEdit(1, 1, out ulong session, out string revision));
        string replacement = new('文', 1024 * 1024 / 3 + 23);
        byte[] input = Encoding.UTF8.GetBytes(world.Envelope(revision, new { path = "/text", value = replacement }));
        byte[] first = input[..1000]; string firstPacket = Packet(0, first, false);
        Program.Check(!world.Runtime.StageSettingsEdit(1, session, Packet(0, first, false, new string('0', 64))).Success && world.Runtime.SettingsEditStorageBytes == 0,
            "Bad checksums must not retain staged bytes or change live settings.");
        Success(world.Runtime.StageSettingsEdit(1, session, firstPacket)); Success(world.Runtime.StageSettingsEdit(1, session, firstPacket));
        byte[] changed = (byte[])first.Clone(); changed[0] ^= 1;
        Program.Check(!world.Runtime.StageSettingsEdit(1, session, Packet(0, changed, false)).Success && world.Runtime.SettingsEditStorageBytes == first.Length,
            "A changed replay rejects without replacing retained bytes.");
        Program.Check(!world.Runtime.StageSettingsEdit(1, session, Packet(1001, first, false)).Success, "Out-of-sequence chunk offsets must reject before staging.");
        Program.Check(!world.Runtime.PrepareSettingsEdit(1, session, out _).Success && world.Value.validations == 0 && world.Value.text[0] == 'x',
            "Incomplete uploads cannot run candidate callbacks or publish partial settings.");
        for (int offset = first.Length; offset < input.Length;) {
            int count = Math.Min(65536, input.Length - offset); byte[] chunk = input.AsSpan(offset, count).ToArray();
            Success(world.Runtime.StageSettingsEdit(1, session, Packet(offset, chunk, offset + count == input.Length))); offset += count;
        }
        Success(world.Runtime.PrepareSettingsEdit(1, session, out ulong prepared)); Success(world.Runtime.PrepareSettingsEdit(1, session, out ulong replay));
        Program.Check(prepared == replay && world.Value.validations == 0 && world.Value.text[0] == 'x', "Prepare replay returns the same token without repeated normalization or touching the live instance.");
        Success(world.Runtime.PublishSettings(1, prepared, false)); Program.Check(world.Value.text == replacement && world.Value.validations == 1, "Complete oversized upload publishes the normalized candidate exactly once.");
        Success(world.Runtime.PublishSettings(1, prepared, true)); Program.Check(world.Value.text[0] == 'x' && world.Value.validations == 0, "Uploaded settings Undo restores the exact prior values.");
        Success(world.Runtime.CloseSettingsEdit(1, session)); Program.Check(world.Runtime.SettingsEditSessionCount == 0 && world.Runtime.SettingsEditStorageBytes == 0,
            "Closing a published edit session releases all staged request storage while its checked Undo token remains separately owned.");
        Success(world.Runtime.DiscardSettings(1, prepared));
        Success(world.Runtime.OpenSettingsEdit(1, 1, out session, out revision));
        input = Encoding.UTF8.GetBytes(world.Envelope(revision, new { path = "/values/40000", value = 123 }));
        Success(world.Runtime.StageSettingsEdit(1, session, Packet(0, input, true))); world.Value.values[0] = -10;
        Program.Check(!world.Runtime.PrepareSettingsEdit(1, session, out _).Success && world.Value.values[^1] == 40000,
            "A complete staged request still conflicts with unreviewed live changes before any publication.");
        Success(world.Runtime.CloseSettingsEdit(1, session));
        Success(world.Runtime.OpenSettingsEdit(1, 1, out session, out revision));
        Success(world.Runtime.StageSettingsEdit(1, session, Packet(0, first, false)));
        Success(world.Runtime.ReleaseInstance(1, 1)); Program.Check(world.Runtime.SettingsEditSessionCount == 0 && world.Runtime.SettingsEditStorageBytes == 0,
            "Deleting an exact target disposes every owned incomplete upload.");
    }
    static void Structural()
    {
        using World world = new(); world.Value.targets = [new GameObject(202, 9, "same"), new GameObject(303, 8, "same")];
        Success(world.Runtime.OpenSettingsEdit(1, 1, out ulong session, out string revision));
        byte[] bytes = Encoding.UTF8.GetBytes(world.Envelope(revision, new { path = "/targets", action = "move", from = 0, to = 1 }));
        Success(world.Runtime.StageSettingsEdit(1, session, Packet(0, bytes, true))); Success(world.Runtime.PrepareSettingsEdit(1, session, out ulong prepared));
        Success(world.Runtime.PublishSettings(1, prepared, false)); Program.Check(world.Value.targets[0].ObjectId == 303 && world.Value.targets[0].SceneLifetime == 8,
            "Index-based structural intent moves exact stale tokens without decoding GUIDs or implicitly rebinding them.");
        world.Value.targets[0] = new GameObject(404, 9, "same");
        Program.Check(!world.Runtime.PublishSettings(1, prepared, true).Success && world.Value.targets[1].ObjectId == 202,
            "Structural Undo compares the complete collection and all exact tokens before replacing any values.");
    }
    static void FailedStorage()
    {
        using World world = new(); Success(world.Runtime.OpenSettingsEdit(1, 1, out ulong session, out string revision));
        string large = new('y', 1024 * 1024 + 23);
        byte[] input = Encoding.UTF8.GetBytes(world.Envelope(revision, new { path = "/text", value = large }));
        for (int offset = 0; offset < input.Length;) {
            int size = Math.Min(65536, input.Length - offset); Success(world.Runtime.StageSettingsEdit(1, session, Packet(offset, input.AsSpan(offset, size).ToArray(), offset + size == input.Length))); offset += size;
        }
        Program.Check(world.Runtime.SettingsEditStorageBytes > 1024 * 1024, "A complete oversized request must own its staged spool before preparation.");
        world.Value.values[0] = -1;
        Program.Check(!world.Runtime.PrepareSettingsEdit(1, session, out _).Success && world.Runtime.SettingsEditStorageBytes == 0,
            "Preparation failure after spilling must dispose its retained input immediately, not hide it from the storage metric.");
        Success(world.Runtime.CloseSettingsEdit(1, session));
        typeof(ManagedScriptRuntime).GetField("m_nextSettingsEdit", BindingFlags.Instance | BindingFlags.NonPublic)!.SetValue(world.Runtime, ulong.MaxValue);
        Success(world.Runtime.OpenSettingsEdit(1, 1, out session, out _)); Program.Check(session == ulong.MaxValue, "The final nonzero edit-session identity is usable once.");
        Success(world.Runtime.CloseSettingsEdit(1, session));
        Program.Check(!world.Runtime.OpenSettingsEdit(1, 1, out session, out _).Success && session == 0,
            "Exhausted edit handles are latched; closing the final session must not allow zero/wrapped handle reuse.");
    }
    static void ReconnectPaths()
    {
        using World world = new();
        string json = JsonSerializer.Serialize(new { targets = Enumerable.Repeat("target-guid", 32769).ToArray() });
        string[] paths = Enumerable.Range(0, 32769).Select(index => "/targets/" + index).ToArray();
        ManagedRuntimeResult result = world.Runtime.ConfigureFields(1, 1, json, validateOnly: true, SettingsValidationPurpose.AUTHORING, paths);
        Program.Check(result.Success && world.Value.targets.Length == 0, result.Message.Length == 0
            ? "Explicit reconnect path capacity must follow the caller-sized array and remain nonmutating during validation." : result.Message);
    }
    static void MultiTarget()
    {
        using World world = new(); Actor second = new();
        ScriptBinding secondBinding = new(1, 2, 202) { GameObject = new GameObject(202, 9, "second-owner") };
        ManagedScriptRuntime.ScriptInstance secondRecord = new(1, 202, world.Record.Type, second, secondBinding, []);
        var instances = (Dictionary<ulong, ManagedScriptRuntime.ScriptInstance>)typeof(ManagedScriptRuntime).GetField("m_instances", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(world.Runtime)!;
        instances.Add(2, secondRecord);
        Success(world.Runtime.OpenSettingsEdit(1, 1, out ulong primary, out string firstRevision));
        Success(world.Runtime.OpenSettingsEdit(1, 2, out ulong secondary, out string secondRevision));
        Success(world.Runtime.CloseSettingsEdit(1, secondary));
        byte[] Batch() => Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new { targets = new object[] {
            new { instanceId = "1", expectedRevision = firstRevision, editSession = primary.ToString(), fields = new { }, patches = new[] { new { path = "/values/40000", value = -17 } } },
            new { instanceId = "2", expectedRevision = secondRevision, fields = new { }, patches = new[] { new { path = "/values/40000", value = -29 } } }
        } }));
        Success(world.Runtime.StageSettingsEdit(1, primary, Packet(0, Batch(), true)));
        Success(world.Runtime.PrepareSettingsEdit(1, primary, out ulong conflict));
        second.values[0] = 777;
        Program.Check(!world.Runtime.PublishSettings(1, conflict, false).Success && world.Value.values[^1] == 40000 && second.values[^1] == 40000 &&
            world.Value.validations == 0 && second.validations == 0, "A late conflict in another target must leave every target and callback result unpublished.");
        Success(world.Runtime.CloseSettingsEdit(1, primary));
        Success(world.Runtime.OpenSettingsEdit(1, 1, out primary, out firstRevision));
        Success(world.Runtime.OpenSettingsEdit(1, 2, out secondary, out secondRevision)); Success(world.Runtime.CloseSettingsEdit(1, secondary));
        Success(world.Runtime.StageSettingsEdit(1, primary, Packet(0, Batch(), true)));
        Success(world.Runtime.PrepareSettingsEdit(1, primary, out ulong prepared));
        Success(world.Runtime.PublishSettings(1, prepared, false));
        Program.Check(world.Value.values[^1] == -17 && second.values[^1] == -29 && world.Value.validations == 1 && second.validations == 1,
            "One staged packet batch commits distinct later-array leaf edits and normalizes each detached target once.");
        world.Value.values[0] = 999;
        Success(world.Runtime.PublishSettings(1, prepared, true));
        Program.Check(world.Value.values[^1] == 40000 && second.values[^1] == 40000 && world.Value.values[0] == 999 && second.values[0] == 777,
            "Atomic multi-target Undo restores only normalized changed leaves and preserves each target's unrelated array slots.");
        Success(world.Runtime.PublishSettings(1, prepared, false));
        Program.Check(world.Value.values[^1] == -17 && second.values[^1] == -29 && world.Value.values[0] == 999 && second.values[0] == 777,
            "Multi-target Redo must restore the stored normalized values without replacing unrelated leaves or repeating callbacks.");
        Success(world.Runtime.CloseSettingsEdit(1, primary, retainPrepared: true)); Success(world.Runtime.DiscardSettings(1, prepared));
        Success(world.Runtime.ReleaseInstance(1, 2));
        Program.Check(world.Runtime.SettingsEditSessionCount == 0 && world.Runtime.SettingsTransactionCount == 0, "Multi-target upload and retained history handles must be explicitly reclaimable.");
    }
    static void AdoptionCapture()
    {
        using World world = new();
        using SettingsAdoptionCapture capture = SettingsAdoptionCapture.Capture(world.Record.Type, world.Value,
            ["/values/40000", "/text"], new(ResidentBytes: 65536));
        Program.Check(capture.ResidentBytes <= 65536 && capture.StorageBytes > 1024 * 1024 && capture.Leaves.Single(leaf => leaf.Path == "/text").Value.IsSpooled,
            "Selected adoption values have an aggregate resident budget and spill large data without rejecting its total size.");
        long residentBytes = capture.ResidentBytes;
        string revision = capture.Revision;
        world.Value.values[^1] = -7; world.Value.text = "changed-after-capture";
        Dictionary<string, object?> tail = capture.ReadPage(0, "", 0, 1, 4096);
        Dictionary<string, object?> text = capture.ReadPage(1, "", 0, 64, 4096);
        Program.Check((int)tail["value"]! == 40000 && (string)text["text"]! == new string('x', 64) && capture.Revision == revision && world.Value.validations == 0,
            "Explicit capture pages keep the selected point-in-time values after live writes and execute no normalization callback.");
        capture.ValidateDestination(world.Record.Type, world.Value);
        using MemoryStream output = new(); Dictionary<string, object?> references = new(StringComparer.Ordinal);
        using (Utf8JsonWriter writer = new(output)) capture.WritePatches(writer, references);
        using JsonDocument patches = JsonDocument.Parse(output.ToArray());
        Program.Check(patches.RootElement[0].GetProperty("value").GetInt32() == 40000 && patches.RootElement[1].GetProperty("value").GetString()!.Length == 1024 * 1024 + 23 && references.Count == 0,
            "Canonical export streams the complete selected data for destination validation without restoring Source-ALC Types.");
        _ = capture.ReadPage(1, "", 524288, 64, 4096); _ = capture.ReadPage(1, "", 0, 64, 4096);
        Program.Check(capture.ResidentBytes == residentBytes && capture.ResidentBytes <= 65536,
            "Repeated random reads keep the aggregate adoption cache at its capture-time resident charge.");
        bool overlapping = false;
        try { using var rejected = SettingsAdoptionCapture.Capture(world.Record.Type, world.Value, ["/values", "/values/0"], new(ResidentBytes: 65536)); }
        catch (InvalidDataException) { overlapping = true; }
        Program.Check(overlapping && world.Value.values[^1] == -7 && world.Value.text == "changed-after-capture" && world.Value.validations == 0,
            "Overlapping adoption selections reject the whole capture and leave every live value and callback state unchanged.");
    }
    static void AdoptionImport()
    {
        const string saved = "11111111-1111-1111-1111-111111111111", originalBirth = "22222222-2222-2222-2222-222222222222";
        using World world = new(saved); Actor source = new(); source.values[^1] = -17; source.text = new('p', 1024 * 1024 + 23);
        world.Record.Binding.ScriptBirth = originalBirth;
        ScriptBinding sourceBinding = new(2, 2, 202, saved) { GameObject = new GameObject(202, 88, "owner"), ScriptBirth = "33333333-3333-3333-3333-333333333333" };
        var instances = (Dictionary<ulong, ManagedScriptRuntime.ScriptInstance>)typeof(ManagedScriptRuntime).GetField("m_instances", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(world.Runtime)!;
        instances.Add(2, new(2, 202, world.Record.Type, source, sourceBinding, []));
        Success(world.Runtime.CaptureSettingsAdoption(2, 2, "{\"paths\":[\"/values/40000\",\"/text\"]}", out ulong capture, out _));
        Success(world.Runtime.ReleaseInstance(2, 2));
        Success(world.Runtime.ReadSettingsAdoption(capture, "{\"leafIndex\":0,\"count\":1}", out string frozen));
        using (JsonDocument page = JsonDocument.Parse(frozen)) Program.Check(page.RootElement.GetProperty("value").GetInt32() == -17, "Source release must leave the owned selected capture readable without a live Source record.");
        Success(world.Runtime.OpenSettingsEdit(1, 1, out ulong session, out string revision)); Success(world.Runtime.CloseSettingsEdit(1, session));
        string Request(string birth) => JsonSerializer.Serialize(new { targets = new[] { new { instanceId = "1", expectedRevision = revision, scriptBirth = birth, captureId = capture.ToString() } } });
        Program.Check(!world.Runtime.PrepareSettingsAdoption(1, Request("different-birth"), out _).Success && world.Value.values[^1] == 40000,
            "An original Script replacement rejects adoption without publishing or invalidating the capture.");
        Success(world.Runtime.ReadSettingsAdoption(capture, "{\"leafIndex\":0,\"count\":1}", out _));
        Success(world.Runtime.PrepareSettingsAdoption(1, Request(originalBirth), out ulong prepared));
        Program.Check(world.Value.values[^1] == 40000 && world.Value.validations == 0, "Destination preparation remains detached and runs no live callback state.");
        Success(world.Runtime.PublishSettings(1, prepared, false));
        Program.Check(world.Value.values[^1] == -17 && world.Value.text[0] == 'p' && world.Value.validations == 1, "One normalized destination transaction adopts complete large selected values.");
        Success(world.Runtime.CloseSettingsAdoption(capture));
        Success(world.Runtime.PublishSettings(1, prepared, true)); Success(world.Runtime.PublishSettings(1, prepared, false));
        Program.Check(world.Value.values[^1] == -17 && world.Value.text.Length == 1024 * 1024 + 23 && world.Value.validations == 1,
            "Adoption Undo/Redo owns its normalized snapshots independently of the discarded Source capture and invokes no extra normalization.");
        Success(world.Runtime.DiscardSettings(1, prepared));
    }
}
