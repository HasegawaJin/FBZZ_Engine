/// @file    SettingsTransactionTests.cs
/// @brief   Candidate isolation, normalized atomic publication, reference moves and callback-free Undo.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class SettingsTransactionTests
{
    internal static (string Name, Action Test)[] Cases => [
        ("SettingsCandidateNormalizesWithoutConstructors", Normalization),
        ("SettingsBatchFailureAndConflictAreAtomic", Atomicity),
        ("SettingsReferenceMovesKeepDuplicateGuidLifetimes", ReferenceMoves),
        ("SettingsAddRedoConstructsFreshWithoutNormalization", AddRedo),
        ("SettingsUndoRetainsUnrelatedEditsAndImmutableSnapshots", UndoIsolation),
        ("SettingsLeafPatchesPreserveNestedAndMathSiblings", LeafPatches),
        ("SettingsExplicitValueCreateHonorsInitializersOnce", ExplicitCreate),
        ("SettingsLedgerCompactsAndDiscardReclaimsBudget", LedgerBudget),
        ("PreparedReviewBudgetsAllTargetsAndKeepsImmutableLargeDetails", BoundedReview)
    ];
    [Serializable] sealed class Nested
    {
        public int value = 2;
        public int sibling = 3;
        public Nested() { ConstructorCalls++; }
    }
    static int ConstructorCalls, ValidateCalls, ResetCalls;
    sealed class Candidate : Script
    {
        public Candidate() { ConstructorCalls++; }
        [ValueRange(0, 10)] public int amount = 2;
        public int coupled = 4;
        public int probe = 0;
        public int unrelated = 0;
        public bool fail = false;
        public bool touchEngine = false;
        public List<Nested> nodes = [new()];
        public Nested? optional = null;
        public string large = "";
        public Vector3 position = new(1, 2, 3);
        [ValueRange(1, 10)] public List<uint> counts = [];
        public GameObject[] targets = [];
        int m_cache = 99;
        protected override void Reset() { ResetCalls++; amount = 3; }
        protected override void OnValidate()
        {
            ValidateCalls++;
            amount = Math.Clamp(amount, 0, 10); coupled = amount * 2; probe = m_cache;
            if (touchEngine) _ = Transform.WorldPosition;
            if (fail) { nodes[0].value = 999; throw new InvalidOperationException("candidate failure"); }
        }
    }
    sealed class World : IDisposable
    {
        internal World()
        {
            Runtime = new(new Program.RecordingBridge());
            ScriptFieldDescriptor[] fields = typeof(Candidate).GetFields(BindingFlags.Instance | BindingFlags.Public).Select(field =>
                new ScriptFieldDescriptor(field, field.Name, ScriptCatalogBuilder.GetKind(field.FieldType), true, false, [])).ToArray();
            ScriptCallbackDescriptor[] callbacks = [
                new(ScriptCallback.RESET, typeof(Candidate).GetMethod("Reset", BindingFlags.Instance | BindingFlags.NonPublic)!),
                new(ScriptCallback.ON_VALIDATE, typeof(Candidate).GetMethod("OnValidate", BindingFlags.Instance | BindingFlags.NonPublic)!)
            ];
            Type = new(typeof(Candidate), typeof(Candidate).GetConstructor(System.Type.EmptyTypes)!, fields, callbacks);
            Records = (Dictionary<ulong, ManagedScriptRuntime.ScriptInstance>)typeof(ManagedScriptRuntime).GetField("m_instances", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(Runtime)!;
            Add(1); Add(2);
            ConstructorCalls = ValidateCalls = ResetCalls = 0;
        }
        internal ManagedScriptRuntime Runtime { get; }
        internal ScriptTypeDescriptor Type { get; }
        internal Dictionary<ulong, ManagedScriptRuntime.ScriptInstance> Records { get; }
        internal Candidate Value(ulong id) => (Candidate)Records[id].Instance;
        internal void Add(ulong id, ulong ownerId = 101)
        {
            Candidate value = new(); ScriptBinding binding = new(1, id, ownerId) { GameObject = new GameObject(101, 9, "owner") };
            Records.Add(id, new(1, ownerId, Type, value, binding, []));
        }
        internal string Revision(ulong id)
        {
            using JsonDocument snapshot = JsonDocument.Parse(ManagedSettingsSnapshot.Read(Records[id], 1, id));
            return snapshot.RootElement.GetProperty("revision").GetString()!;
        }
        internal object Request(ulong id, string fields, bool initial = false, object[]? moves = null)
            => new { instanceId = id.ToString(), expectedRevision = Revision(id), fields = JsonSerializer.Deserialize<JsonElement>(fields), initial, rebind = Array.Empty<string>(), moves = moves ?? [] };
        internal ManagedRuntimeResult Prepare(object[] requests, out ulong token) => Runtime.PrepareSettings(1, JsonSerializer.Serialize(new { targets = requests }), out token);
        internal ulong Prepare(ulong id, string fields, bool initial = false, object[]? moves = null)
        { ManagedRuntimeResult result = Prepare([Request(id, fields, initial, moves)], out ulong token); Program.Check(result.Success, result.Message); return token; }
        public void Dispose() { foreach (ulong id in Records.Keys.ToArray()) Runtime.ReleaseInstance(1, id); Runtime.Dispose(); }
    }
    static void Success(ManagedRuntimeResult result) => Program.Check(result.Success, result.Message);
    static void Normalization()
    {
        using World world = new();
        ulong token = world.Prepare(1, "{\"amount\":25}");
        Program.Check(world.Value(1).amount == 2 && world.Value(1).coupled == 4 && ConstructorCalls == 0 && ValidateCalls == 1 && ResetCalls == 0,
            "Preparation must normalize a detached constructor-free candidate exactly once without touching the live instance.");
        Success(world.Runtime.PublishSettings(1, token, false));
        Program.Check(world.Value(1).amount == 10 && world.Value(1).coupled == 20 && world.Value(1).probe == 0,
            "Normalization includes fields outside the input, and private runtime caches have CLR defaults.");
        Success(world.Runtime.PublishSettings(1, token, false));
        Success(world.Runtime.PublishSettings(1, token, true));
        Program.Check(world.Value(1).amount == 2 && world.Value(1).coupled == 4 && world.Value(1).probe == 0, "Undo must restore all normalized changes.");
        Success(world.Runtime.PublishSettings(1, token, false));
        Program.Check(ValidateCalls == 1 && ResetCalls == 0 && ConstructorCalls == 0, "Commit retries, Undo and Redo must not rerun authoring callbacks or constructors.");
        Success(world.Runtime.DiscardSettings(1, token));
        Program.Check(!world.Runtime.PublishSettings(1, token, true).Success, "Discarded previews must not remain usable.");
    }
    static void Atomicity()
    {
        using World world = new();
        ManagedRuntimeResult rejected = world.Prepare([world.Request(1, "{\"amount\":4}"), world.Request(2, "{\"fail\":true}")], out ulong none);
        Program.Check(!rejected.Success && none == 0 && world.Value(1).amount == 2 && world.Value(2).nodes[0].value == 2,
            "A later callback failure must leave every target and shared nested graph unchanged.");
        rejected = world.Prepare([world.Request(1, "{\"touchEngine\":true}")], out none);
        Program.Check(!rejected.Success && !world.Value(1).touchEngine && ScriptApiRuntime.Current is null, "Candidate callbacks must have no usable Engine scope.");
        ScriptExecutionScope scope = new(new Program.RecordingBridge(), world.Runtime, 1, 1, 101, 1, 9, ScriptCallback.UPDATE);
        ScriptApiRuntime.Current = scope;
        try {
            rejected = world.Prepare([world.Request(1, "{\"amount\":4}")], out none);
            Program.Check(rejected.Status == BridgeStatus.INVALID_SCOPE && ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, scope),
                "Preparation cannot borrow an existing Engine callback scope and must leave that scope intact.");
        } finally { ScriptApiRuntime.Current = null; }
        Success(world.Prepare([world.Request(1, "{\"amount\":4}"), world.Request(2, "{\"amount\":5}")], out ulong token));
        world.Value(2).unrelated = 9;
        Program.Check(!world.Runtime.PublishSettings(1, token, false).Success && world.Value(1).amount == 2 && world.Value(2).amount == 2,
            "The full snapshot comparison must catch direct public field writes even without a revision counter update, before any target is written.");
    }
    static void ReferenceMoves()
    {
        using World world = new();
        world.Value(1).targets = [new GameObject(202, 9, "same-guid"), new GameObject(303, 8, "same-guid")];
        object[] moves = [new { fromPath = "/targets/0", toPath = "/targets/1" }, new { fromPath = "/targets/1", toPath = "/targets/0" }];
        ulong token = world.Prepare(1, "{\"targets\":[{\"$go\":\"same-guid\"},{\"$go\":\"same-guid\"}]}", moves: moves);
        Success(world.Runtime.PublishSettings(1, token, false));
        Program.Check(world.Value(1).targets[0].ObjectId == 303 && world.Value(1).targets[0].SceneLifetime == 8
            && world.Value(1).targets[1].ObjectId == 202, "Reordering identical GUIDs must move exact tokens, including expired lifetimes.");
        Success(world.Runtime.PublishSettings(1, token, true));
        Program.Check(world.Value(1).targets[0].ObjectId == 202 && world.Value(1).targets[1].SceneLifetime == 8, "Undo must never re-resolve an expired reference GUID.");
        Success(world.Runtime.PublishSettings(1, token, false));
        Program.Check(world.Value(1).targets[0].ObjectId == 303, "Redo must retain the same exact token movement.");
    }
    static void AddRedo()
    {
        using World world = new();
        ulong token = world.Prepare(1, "{\"amount\":5}", initial: true);
        Program.Check(ResetCalls == 1 && ValidateCalls == 1 && ConstructorCalls == 0, "Initial add runs Reset before explicit input and validation once.");
        Success(world.Runtime.PublishSettings(1, token, false)); Success(world.Runtime.PublishSettings(1, token, true));
        Success(world.Runtime.ReleaseInstance(1, 1)); world.Add(3, 303);
        string remap = JsonSerializer.Serialize(new { replacements = new[] { new { previousId = "1", instanceId = "3", expectedRevision = world.Revision(3) } } });
        Success(world.Runtime.PublishSettings(1, token, false, remap));
        Program.Check(world.Value(3).amount == 5 && world.Value(3).coupled == 10 && ConstructorCalls == 2 && ResetCalls == 1 && ValidateCalls == 1,
            "Redo add creates a fresh real instance but restores the normalized snapshot without Reset or OnValidate.");
        Success(world.Prepare([world.Request(3, "{\"amount\":7}", true), world.Request(2, "{\"amount\":8}", true)], out ulong batch));
        Success(world.Runtime.PublishSettings(1, batch, false)); Success(world.Runtime.PublishSettings(1, batch, true));
        Success(world.Runtime.ReleaseInstance(1, 3)); Success(world.Runtime.ReleaseInstance(1, 2)); world.Add(4);
        string duplicate = JsonSerializer.Serialize(new { replacements = new[] {
            new { previousId = "3", instanceId = "4", expectedRevision = world.Revision(4) },
            new { previousId = "2", instanceId = "4", expectedRevision = world.Revision(4) }
        } });
        Program.Check(!world.Runtime.PublishSettings(1, batch, false, duplicate).Success && world.Value(4).amount == 2,
            "Fresh add replacement identities must remain distinct and a duplicate destination must reject the whole batch before any write.");
    }
    static void UndoIsolation()
    {
        using World world = new();
        ulong token = world.Prepare(1, "{\"amount\":7,\"nodes\":[{\"value\":8}]}"); Success(world.Runtime.PublishSettings(1, token, false));
        world.Value(1).unrelated = 42; world.Value(1).nodes[0].value = 9;
        Program.Check(!world.Runtime.PublishSettings(1, token, true).Success && world.Value(1).amount == 7,
            "Live mutable graphs must not alias the immutable token; a newer edited value conflicts instead of overwriting it.");
        world.Value(1).nodes[0].value = 8; Success(world.Runtime.PublishSettings(1, token, true));
        Program.Check(world.Value(1).amount == 2 && world.Value(1).nodes[0].value == 2 && world.Value(1).unrelated == 42,
            "Undo restores changed fields and exact snapshots while preserving unrelated later edits.");
    }
    static ulong PreparePatches(World world, ulong id, params object[] patches)
    {
        ManagedRuntimeResult result = world.Prepare([new {
            instanceId = id.ToString(), expectedRevision = world.Revision(id), fields = new Dictionary<string, object?>(), patches
        }], out ulong token);
        Success(result); return token;
    }
    static void LeafPatches()
    {
        using World world = new();
        ulong token = PreparePatches(world, 1, new { path = "/nodes/0/value", value = 8 }, new { path = "/position/0", value = 7 });
        Success(world.Runtime.PublishSettings(1, token, false));
        world.Value(1).nodes[0].sibling = 42; world.Value(1).position = new(7, 99, 3);
        Success(world.Runtime.PublishSettings(1, token, true));
        Program.Check(world.Value(1).nodes[0].value == 2 && world.Value(1).nodes[0].sibling == 42 && world.Value(1).position.Equals(new Vector3(1, 99, 3)),
            "Undo must preserve unrelated nested leaves and math axes in the same root field.");
        Success(world.Runtime.PublishSettings(1, token, false));
        Program.Check(world.Value(1).nodes[0].value == 8 && world.Value(1).nodes[0].sibling == 42 && world.Value(1).position.Y == 99,
            "Redo reapplies only normalized changed leaves, preserving each target's current sibling values.");
        world.Value(1).nodes[0].value = 9;
        Program.Check(!world.Runtime.PublishSettings(1, token, true).Success && world.Value(1).position.X == 7,
            "A later leaf conflict must be detected before writing any other leaf.");
        object[] patches = [new { path = "/nodes/0/value", value = 4 }];
        Success(world.Prepare([new { instanceId = "1", expectedRevision = world.Revision(1), fields = new Dictionary<string, object?>(), patches },
            new { instanceId = "2", expectedRevision = world.Revision(2), fields = new Dictionary<string, object?>(), patches }], out token));
        world.Value(2).nodes[0].sibling = 88;
        Program.Check(!world.Runtime.PublishSettings(1, token, false).Success && world.Value(1).nodes[0].value == 9,
            "First publication still compares every target's full current snapshot rather than accepting unreviewed concurrent changes.");
    }
    static void ExplicitCreate()
    {
        using World world = new();
        ulong token = PreparePatches(world, 1, new { path = "/optional", action = "create" });
        Program.Check(ConstructorCalls == 1 && world.Value(1).optional is null, "Explicit helper creation evaluates its constructor exactly once on the detached candidate.");
        Success(world.Runtime.PublishSettings(1, token, false));
        Program.Check(world.Value(1).optional?.value == 2 && world.Value(1).optional?.sibling == 3, "Explicit creation honors declared initializer values, distinct from schema metadata defaults.");
        Success(world.Runtime.PublishSettings(1, token, true)); Success(world.Runtime.PublishSettings(1, token, false));
        Program.Check(ConstructorCalls == 1, "Undo/Redo must not repeat a helper constructor.");
        token = PreparePatches(world, 1, new { path = "/nodes/-", action = "create" }, new { path = "/counts/-", action = "create" });
        Success(world.Runtime.PublishSettings(1, token, false));
        Program.Check(world.Value(1).nodes.Count == 2 && world.Value(1).nodes[1].value == 2 && world.Value(1).counts.SequenceEqual(new uint[] { 1 }),
            "List-item create honors helper initializers and uses range-valid scalar defaults.");
    }
    static void BoundedReview()
    {
        using World world = new();
        world.Value(1).large = world.Value(2).large = new string('x', 2 * 1024 * 1024);
        object[] requests = [world.Request(1, "{\"amount\":7}", initial: true), world.Request(2, "{\"amount\":7}", initial: true)];
        Success(world.Prepare(requests, out ulong token));
        Success(world.Runtime.ReadPreparedReview(1, token, "{\"mode\":\"targets\",\"targetCount\":2,\"changeCount\":20,\"wireBytes\":4096}", out string summary));
        Program.Check(System.Text.Encoding.UTF8.GetByteCount(summary) <= 4096, "One response budget must include all target, change and exact-reference metadata.");
        using JsonDocument page = JsonDocument.Parse(summary);
        Program.Check(page.RootElement.GetProperty("totalTargets").GetInt32() == 2, "A bounded target page must report the full target count.");
        Success(world.Runtime.ReadPreparedReview(1, token, "{\"mode\":\"plan\",\"targetOffset\":0,\"targetCount\":1,\"wireBytes\":2048}", out string planPage));
        using JsonDocument planMetadata = JsonDocument.Parse(planPage);
        Program.Check(planMetadata.RootElement.GetProperty("returnedTargets").GetInt32() == 1
            && planMetadata.RootElement.GetProperty("nextTargetOffset").GetInt32() == 1
            && planMetadata.RootElement.GetProperty("targets")[0].GetProperty("instanceId").GetString() == "1",
            "Compact native plan pages must expose exact target identity and forward progress.");
        Success(world.Runtime.ReadPreparedReview(1, token, "{\"mode\":\"validate\",\"side\":\"before\",\"wireBytes\":2048}", out string initialValidation));
        using JsonDocument initialValidationDocument = JsonDocument.Parse(initialValidation);
        Program.Check(initialValidationDocument.RootElement.GetProperty("current").GetBoolean(), "Read-only full preflight must accept unchanged settings.");
        string revision = world.Revision(1);
        int large = 0;
        for (int offset = 0; ; ) {
            string changes = JsonSerializer.Serialize(new { mode = "changes", targetIndex = 0, changeOffset = offset, changeCount = 16, wireBytes = 4096 });
            Success(world.Runtime.ReadPreparedReview(1, token, changes, out string changePage));
            using JsonDocument changePageDocument = JsonDocument.Parse(changePage);
            JsonElement items = changePageDocument.RootElement.GetProperty("target").GetProperty("changes");
            int found = -1;
            for (int index = 0; index < items.GetArrayLength(); index++) if (items[index].GetProperty("path").GetString() == "/large") found = offset + index;
            if (found >= 0) { large = found; break; }
            JsonElement next = changePageDocument.RootElement.GetProperty("target").GetProperty("nextChangeOffset");
            Program.Check(next.ValueKind == JsonValueKind.Number && next.GetInt32() > offset, "Change pages must make forward progress while locating a requested field.");
            offset = next.GetInt32();
        }
        string detail = JsonSerializer.Serialize(new { mode = "value", targetIndex = 0, changeIndex = large, side = "after", path = "/large", start = 1000000, count = 32, wireBytes = 4096 });
        Success(world.Runtime.ReadPreparedReview(1, token, detail, out string beforeMutation));
        world.Value(1).large = "live mutated after preview";
        Success(world.Runtime.ReadPreparedReview(1, token, "{\"mode\":\"validate\",\"side\":\"before\",\"wireBytes\":2048}", out string staleValidation));
        using JsonDocument staleValidationDocument = JsonDocument.Parse(staleValidation);
        Program.Check(!staleValidationDocument.RootElement.GetProperty("current").GetBoolean(), "Read-only first-apply validation must detect a full-revision conflict.");
        Success(world.Runtime.ReadPreparedReview(1, token, detail, out string afterMutation));
        using JsonDocument preserved = JsonDocument.Parse(afterMutation);
        Program.Check(preserved.RootElement.GetProperty("page").GetProperty("text").GetString() == new string('x', 32)
            && preserved.RootElement.GetProperty("page").GetProperty("total").GetInt64() == 2L * 1024 * 1024,
            "Lazy detail must read the immutable normalized candidate rather than live or clipped settings.");
        Program.Check(beforeMutation == afterMutation, "Review of the same retained candidate must stay identical across unrelated live changes.");
        world.Value(1).large = new string('x', 2 * 1024 * 1024);
        Program.Check(world.Revision(1) == revision, "Review must not change live settings or transaction conflict hashes.");
        Success(world.Runtime.PublishSettings(1, token, false));
        Program.Check(world.Value(1).large.Length == 2 * 1024 * 1024 && world.Value(1).amount == 7, "Bounded review must not limit committed setting values.");
        Success(world.Runtime.ReadPreparedReview(1, token, "{\"mode\":\"validate\",\"side\":\"after\",\"deltaOnly\":true,\"wireBytes\":2048}", out string appliedValidation));
        using JsonDocument appliedValidationDocument = JsonDocument.Parse(appliedValidation);
        Program.Check(appliedValidationDocument.RootElement.GetProperty("current").GetBoolean(), "Read-only receipt validation must accept exact changed leaves after publication.");
        Success(world.Runtime.PublishSettings(1, token, true));
        Program.Check(world.Value(1).large.Length == 2 * 1024 * 1024 && world.Value(1).amount == 2, "Undo must retain complete values independent of review pages.");
        Success(world.Runtime.DiscardSettings(1, token));
        Program.Check(!world.Runtime.ReadPreparedReview(1, token, detail, out _).Success, "Discard must invalidate review access and reclaim retained snapshots.");
    }

    static void LedgerBudget()
    {
        using World world = new(); world.Value(1).large = new string('x', 120000);
        for (int index = 0; index < 20; index++) {
            ulong token = world.Prepare(1, "{\"amount\":7}");
            Success(world.Runtime.ReadPreparedSettings(1, token, out string preview));
            using JsonDocument review = JsonDocument.Parse(preview);
            JsonElement target = review.RootElement.GetProperty("targets")[0];
            long reviewBytes = System.Text.Encoding.UTF8.GetByteCount(preview);
            Program.Check(target.GetProperty("changedPathCount").GetInt32() > 0
                && reviewBytes < 4096
                && world.Runtime.SettingsTransactionStorageBytes > 0
                && world.Runtime.SettingsTransactionBytes > 0
                && world.Runtime.SettingsTransactionBytes < 2L * world.Value(1).large.Length,
                "An uncommitted preview retains compact target metadata and immutable change snapshots without duplicating full before/after JSON.");
            Success(world.Runtime.DiscardSettings(1, token));
            Program.Check(world.Runtime.SettingsTransactionBytes == 0 && world.Runtime.SettingsTransactionStorageBytes == 0 && world.Runtime.SettingsTransactionCount == 0,
                "Explicit preview discard must reclaim all retained resident and spool capacity.");
        }
        List<ulong> tokens = new();
        for (int index = 0; index < 40; index++) {
            ulong token = world.Prepare(1, index % 2 == 0 ? "{\"amount\":7}" : "{\"amount\":2}");
            Success(world.Runtime.PublishSettings(1, token, false)); tokens.Add(token);
            Program.Check(world.Runtime.SettingsTransactionBytes < 200000, "Committed history stores changed leaves rather than pinning unchanged large settings snapshots.");
        }
        foreach (ulong token in tokens) Success(world.Runtime.DiscardSettings(1, token));
        Program.Check(world.Runtime.SettingsTransactionBytes == 0, "Releasing retained history receipts must make the ledger capacity reusable.");
    }
}
