/// @file    Stage2Tests.cs
/// @brief   参照寿命・配列・一括設定・型 ID と名前検索の実 Host 検証。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class Stage2Tests
{
    internal static (string Name, Action Test)[] Cases =>
    [
        ("PureMathContracts", MathTests.Run),
        ("RandomStreamContracts", RandomStreamTests.Run),
        ("MaterialStoreApiContracts", ServicesApiTests.AuthoringServices),
        ("PortableStoreContracts", PortableStoreTests.Run),
        ("QuaternionMathContracts", QuaternionTests.Run),
        ("QuaternionBatchContracts", QuaternionBatchTests.Run),
        ("ManagedDiagnosticsContracts", ManagedDiagnosticsTests.Run),
        ("CompletionContracts", CompletionContractsTests.Run),
        .. RequiredReferenceTests.Cases,
        ("ServicesApiContracts", ServicesApiTests.Run),
        ("TransformPoseApiContracts", TransformPoseApiTests.Run),
        ("ObjectBatchApiContracts", ObjectBatchApiTests.Run),
        ("ObjectBatchNativeBridgeContracts", ObjectBatchBridgeTests.Run),
        ("ArraySchemaAndBinaryRoundtrip", ArraySchemaAndBinaryRoundtrip),
        ("AtomicConfigureAndRequiredReferences", AtomicConfigureAndRequiredReferences),
        ("NativeReferenceQueryUsesOutputCapacity", NativeReferenceQueryUsesOutputCapacity),
        ("LiveLookupBindsOnlyCurrentSceneMetadata", LiveLookupBindsOnlyCurrentSceneMetadata),
        ("RetainedLookupAndCallerScope", RetainedLookupAndCallerScope),
        ("ManagedComponentPagingContracts", ManagedComponentPagingContracts),
        ("ManagedComponentMalformedCopyPagePreservesCaller", ManagedComponentMalformedCopyPagePreservesCaller),
        ("ScriptLifetimeIsDistinctFromObjectLifetime", ScriptLifetimeIsDistinctFromObjectLifetime),
        ("NamedLookupAndMovementHelpers", NamedLookupAndMovementHelpers),
        ("PlainHelperUsesCallerScope", PlainHelperUsesCallerScope),
        ("UtilityLookupRetainsGenerationIdentity", UtilityLookupRetainsGenerationIdentity),
        ("GeneratorContracts", GeneratorContracts),
        ("SourceDiscoveryAndStableManifest", SourceDiscoveryAndStableManifest)
    ];

    static void ArraySchemaAndBinaryRoundtrip()
    {
        using World world = new();
        ulong id = world.Create("ManagedFixtures.ReferenceComponent");
        ScriptTypeDescriptor type = world.Type("ManagedFixtures.ReferenceComponent");
        Script instance = world.Instance(id);
        foreach (ScriptFieldDescriptor field in type.Fields.Where((ScriptFieldDescriptor value) => value.IsArray))
        {
            Array original = (Array)field.Read(instance)!;
            byte[] encoded = ScriptFieldCodec.EncodeArray(original, field.ElementKind);
            Array restored = ScriptFieldCodec.DecodeArray(encoded, field, (string guid) => new GameObject(0, 1, guid));
            Program.Check(original.Cast<object?>().SequenceEqual(restored.Cast<object?>()), "Every supported one-dimensional array must retain its declared values and element type.");
            Program.Check(!field.TryWrite(instance, null), "A null array must remain distinct from an empty array and be rejected.");
        }
        ScriptFieldDescriptor weights = Program.Field(type, "weights");
        Program.Check(!weights.TryWrite(instance, new float[] { float.NaN }) && !weights.TryWrite(instance, new float[] { 11f }), "Array constraints must validate every numeric element.");
        Program.Check(!Program.Field(type, "labels").TryWrite(instance, new string[] { null! }), "String arrays must reject null elements.");
        ScriptFieldDescriptor targets = Program.Field(type, "targets");
        GameObject[] references = [new GameObject(202, 1, "target-guid"), null!, new GameObject(0, 1, "missing-guid"), new GameObject(202, 1, "target-guid")];
        byte[] payload = ScriptFieldCodec.EncodeArray(references, targets.ElementKind);
        GameObject[] decoded = (GameObject[])ScriptFieldCodec.DecodeArray(payload, targets, (string guid) => new GameObject(guid == "target-guid" ? 202UL : 0, 1, guid));
        Program.Check(decoded.Length == 4 && decoded[1] is null && decoded[2].Guid == "missing-guid"
            && decoded[0].Guid == decoded[3].Guid, "Reference arrays must preserve order, duplicates, null and missing GUID slots.");
        byte[] malformed = [.. payload, 0];
        Program.Throws<InvalidDataException>(() => ScriptFieldCodec.DecodeArray(malformed, targets, (string guid) => new GameObject(0, 1, guid)));
    }

    static void AtomicConfigureAndRequiredReferences()
    {
        using World world = new();
        ulong id = world.Create("ManagedFixtures.ReferenceComponent");
        ScriptTypeDescriptor type = world.Type("ManagedFixtures.ReferenceComponent");
        Script instance = world.Instance(id);
        const string valid = "{\"target\":\"target-guid\",\"targets\":[\"target-guid\",null,\"missing-guid\",\"target-guid\"],\"counts\":[3,-2],\"weights\":[1,2],\"toggles\":[false,true],\"labels\":[\"日本語\",\"second\"],\"offsets\":[[1,2,3]],\"modes\":[0,1]}";
        Program.Check(world.Runtime.ConfigureFields(1, id, valid, validateOnly: true).Success, "A preview must validate every field without requiring a user getter.");
        Program.Check(Program.Field(type, "target").Read(instance) is null, "Preview must leave the live instance unchanged.");
        Program.Check(world.Runtime.ConfigureFields(1, id, valid, validateOnly: false).Success, "A validated batch must apply directly to the existing instance.");
        GameObject[] targets = (GameObject[])Program.Field(type, "targets").Read(instance)!;
        Program.Check(targets[1] is null && targets[2].ObjectId == 0 && targets[2].Guid == "missing-guid", "Missing references must not become CLR null.");
        Program.Check(!world.Runtime.ConfigureFields(1, id, "{\"counts\":[99],\"weights\":[50]}", false).Success, "One invalid field must reject the entire batch.");
        Program.Check(((int[])Program.Field(type, "counts").Read(instance)!).SequenceEqual([3, -2]), "Invalid batches must not leave earlier writes applied.");
        Program.Check(!world.Runtime.ConfigureFields(1, id, "{\"counts\":[99],\"counts\":[88]}", false).Success, "Duplicate JSON keys must not silently select a value.");

        ulong requiredId = world.Create("ManagedFixtures.RequiredComponent");
        Program.Check(!world.Runtime.ValidateReferences(1, requiredId).Success, "Default missing RequiredReference must block restoration, without rejecting the type catalog.");
        Program.Check(!world.Runtime.ConfigureFields(1, requiredId, "{\"target\":\"missing-guid\"}", false).Success, "Unresolved required GUIDs must be rejected.");
        Program.Check(world.Runtime.ConfigureFields(1, requiredId, "{\"target\":\"target-guid\"}", false).Success, "A live required reference must configure.");
        Script required = world.Instance(requiredId);
        ScriptFieldDescriptor targetField = Program.Field(world.Type("ManagedFixtures.RequiredComponent"), "target");
        GameObject old = (GameObject)targetField.Read(required)!;
        world.Bridge.Alive.Remove(202);
        world.Bridge.Alive.Add(303);
        world.Bridge.Guids["target-guid"] = 303;
        int resolves = world.Bridge.ResolveCount;
        Program.Check(!world.Runtime.ValidateReferences(1, requiredId).Success && world.Bridge.ResolveCount == resolves,
            "Required validation must inspect the old token without resolving its GUID again.");
        Program.Check(old.ObjectId == 202 && ((GameObject)targetField.Read(required)!).ObjectId == 202, "Same-GUID recreation must not revive retained wrappers.");
        Program.Check(world.Runtime.ConfigureFields(1, requiredId, "{\"target\":\"target-guid\"}", false).Success
            && ((GameObject)targetField.Read(required)!).ObjectId == 303, "Only an explicit new setting may resolve a recreated GUID.");
        Program.Check(world.Runtime.ConfigureFields(1, requiredId, "{\"target\":null}", false, purpose: SettingsValidationPurpose.SNAPSHOT).Success
            && !world.Runtime.ValidateReferences(1, requiredId).Success, "Internal Undo restoration must restore an earlier missing required field without permitting normal publication.");
        Program.Check(!world.Runtime.ConfigureFields(1, id, "{\"weights\":[50]}", false, purpose: SettingsValidationPurpose.SNAPSHOT).Success,
            "Snapshot restoration must retain numeric and structural validation.");
        ulong arrayId = world.Create("ManagedFixtures.RequiredArrayComponent");
        Program.Check(!world.Runtime.ConfigureFields(1, arrayId, "{\"targets\":[]}", false).Success
            && !world.Runtime.ConfigureFields(1, arrayId, "{\"targets\":[null]}", false).Success, "Required arrays must be nonempty with every element live.");
        Program.Check(world.Runtime.ConfigureFields(1, arrayId, "{\"targets\":[\"owner-101\",\"target-guid\"]}", false).Success, "Required reference arrays must accept an ordered live set.");
    }

    static int s_referenceQueryCalls;

    static unsafe void NativeReferenceQueryUsesOutputCapacity()
    {
        s_referenceQueryCalls = 0;
        BridgeRequest initialize = new()
        {
            AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.INITIALIZE, Generation = BridgeAbi.SCHEMA_HASH,
            Index = (uint)sizeof(BridgeRequest), Flags = (uint)sizeof(BridgeReply),
            Integer = (long)(nint)(delegate* unmanaged[Cdecl]<BridgeRequest*, BridgeReply*, int>)&ReferenceNativeDispatch
        };
        Program.Check(Dispatch(initialize).Status == BridgeStatus.OK, "The reference bridge regression must initialize the real Host entrypoint.");
        try
        {
            BridgeRequest load = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.LOAD_CANDIDATE, Generation = 700, Flags = BridgeAbi.INTERNAL_MODE };
            Program.Check(Dispatch(load, Program.FixturePath("V1")).Status == BridgeStatus.OK, "The real Host must load its normal reference fixture.");
            BridgeRequest create = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.CREATE, Generation = 700, OwnerId = 101 };
            BridgeReply created = Dispatch(create, "ManagedFixtures.ReferenceComponent\n11111111-1111-4111-8111-111111111111");
            Program.Check(created.Status == BridgeStatus.OK, "The reference fixture must construct.");
            BridgeRequest bind = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.BIND_OWNER, Generation = 700, InstanceId = created.InstanceId, OwnerId = 101, Integer = 101, CallId = 1 };
            Program.Check(Dispatch(bind, "owner-101\n22222222-2222-4222-8222-222222222222").Status == BridgeStatus.OK, "The reference fixture must bind before preparation.");
            string guid = new('g', 300);
            BridgeRequest write = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.WRITE_FIELDS, Generation = 700, InstanceId = created.InstanceId, OwnerId = 101 };
            Program.Check(Dispatch(write, JsonSerializer.Serialize(new { target = guid })).Status == BridgeStatus.OK && s_referenceQueryCalls == 2,
                "Read-only GUID queries must allocate output capacity and retry a larger native payload exactly once.");
            Program.Check(Dispatch(write, "{\"counts\":[9]}").Status == BridgeStatus.OK && s_referenceQueryCalls == 2,
                "Unrelated field deltas must preserve retained reference identity without resolving GUIDs again.");
            BridgeRequest read = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.READ_FIELD, Generation = 700, InstanceId = created.InstanceId };
            byte[] output = new byte[300];
            BridgeReply restored = Dispatch(read, "target", output);
            Program.Check(restored.Status == BridgeStatus.OK && restored.Integer == 202 && Encoding.UTF8.GetString(output) == guid,
                "The saved GUID and opaque resolved identity must survive the real managed ABI.");
        }
        finally
        {
            BridgeRequest shutdown = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.SHUTDOWN };
            Program.Check(Dispatch(shutdown).Status == BridgeStatus.OK, "The regression Host session must release after execution.");
        }
    }

    static unsafe BridgeReply Dispatch(BridgeRequest request, string input = "", byte[]? output = null)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(input);
        fixed (byte* inputPointer = bytes)
        fixed (byte* outputPointer = output)
        {
            request.Input = inputPointer; request.InputBytes = (uint)bytes.Length;
            request.Output = outputPointer; request.OutputCapacity = (uint)(output?.Length ?? 0);
            BridgeReply reply = default;
            int status = EntryPoint.DispatchManaged(ref request, ref reply);
            Program.Check(status == (int)reply.Status, "The real ABI must keep status and return code consistent.");
            return reply;
        }
    }

    /// @note 実 native と同じ GUID 出力契約を使い、capacity 不足では書き込まず同じ token を返す。
    [UnmanagedCallersOnly(CallConvs = new Type[] { typeof(CallConvCdecl) })]
    static unsafe int ReferenceNativeDispatch(BridgeRequest* request, BridgeReply* reply)
    {
        if (request is null || reply is null) return (int)BridgeStatus.INVALID_ARGUMENT;
        *reply = default;
        if (request->Op == BridgeOp.RESOLVE_GUID)
        {
            s_referenceQueryCalls++;
            reply->ValueKind = ScriptFieldKind.GAME_OBJECT; reply->Integer = 202; reply->CallId = 1;
            reply->OutputBytes = request->InputBytes;
            if (request->OutputCapacity < request->InputBytes || request->Output is null) reply->Status = BridgeStatus.BUFFER_TOO_SMALL;
            else new ReadOnlySpan<byte>(request->Input, (int)request->InputBytes).CopyTo(new Span<byte>(request->Output, (int)request->InputBytes));
        }
        else if (request->Op == BridgeOp.OBJECT_IS_ALIVE) reply->Integer = request->Integer == 202 ? 1 : 0;
        else reply->Status = BridgeStatus.UNSUPPORTED;
        return (int)reply->Status;
    }

    static void LiveLookupBindsOnlyCurrentSceneMetadata()
    {
        using World world = new();
        ulong caller = world.Create("ManagedFixtures.LifecycleComponent");
        Program.Check(world.Runtime.CreateInstance(1, "ManagedFixtures.HealthComponent", 202, out ulong targetId).Success, "The target must start as a constructed but unbound Script.");
        Program.Check(world.Runtime.LoadCandidate(Program.FixturePath("V2"), 2, internalMode: true).Success, "The fixture must include a separate live candidate generation.");
        Program.Check(world.Runtime.CreateInstance(2, "ManagedFixtures.HealthComponent", 202, out ulong candidateId).Success, "The candidate metadata must remain unbound.");
        world.Bridge.Components[202] = [targetId];
        world.Bridge.DuringTranslate = () =>
        {
            Program.Check(world.Runtime.BindOwner(2, candidateId, 202, 202, 1, "target-guid").Status == BridgeStatus.INVALID_SCOPE
                && world.Runtime.BindOwner(1, targetId, 202, 202, 2, "target-guid").Status == BridgeStatus.INVALID_SCOPE
                && world.Runtime.BindOwner(1, targetId, 202, 0, 0, "").Status == BridgeStatus.INVALID_SCOPE,
                "Live metadata binding must reject other generations, scenes and detached targets.");
            Program.Check(world.Runtime.ConfigureFields(1, targetId, "{\"health\":5}", false).Status == BridgeStatus.INVALID_SCOPE,
                "The metadata exception must not authorize setting writes during a callback.");
            Program.Check(world.Runtime.BindOwner(1, targetId, 202, 202, 1, "target-guid").Success,
                "Native lookup must be able to bind an unstarted target inside the current caller scope.");
            GameObject target = new(202, 1, "target-guid");
            Script? found = target.GetComponent<Script>();
            Program.Check(found is not null && ReferenceEquals(found, world.Instance(targetId)) && found.IsAlive,
                "Typed lookup must return the bound Script without requiring its own callback to have started.");
            Program.Check(ScriptApiRuntime.Current?.OwnerId == 101, "Metadata binding must preserve the caller scope.");
        };
        world.Invoke(caller, ScriptCallback.UPDATE);
        world.Bridge.DuringTranslate = null;
    }

    static void ManagedComponentPagingContracts()
    {
        using World world = new();
        ulong caller = world.Create("ManagedFixtures.ReferenceComponent");
        Program.Check(world.Runtime.ConfigureFields(1, caller, "{\"target\":\"target-guid\"}", false).Success, "The paging caller must connect its translate target.");
        const int total = 4103;
        ulong[] ids = new ulong[total];
        for (int index = 0; index < total; index++) ids[index] = world.Create("ManagedFixtures.HealthComponent", 202, 202, "target-guid");
        world.Bridge.Components[202] = ids;
        GameObject target = new(202, 1, "target-guid");
        Script sentinel = world.Instance(caller);
        Script?[] shortOutput = new Script?[total - 1]; Array.Fill(shortOutput, sentinel);
        Script?[] exact = new Script?[total + 1]; exact[total] = sentinel;
        world.Bridge.DuringTranslate = () => {
            CollectionReadResult count = target.GetComponents<Script>(Span<Script?>.Empty);
            Program.Check(count.Required == total && count.Written == 0, "An empty caller buffer must report the full collection beyond4096.");
            CollectionReadResult insufficient = target.GetComponents<Script>(shortOutput);
            Program.Check(insufficient.Required == total && insufficient.Written == 0 && shortOutput.All(value => ReferenceEquals(value, sentinel)),
                "Insufficient caller capacity must leave every sentinel unchanged.");
            CollectionReadResult copied = target.GetComponents<Script>(exact);
            Program.Check(copied.Required == total && copied.Written == total && copied.Complete && ReferenceEquals(exact[total], sentinel),
                "Adequate caller capacity must copy every match while preserving the unused tail.");
            for (int index = 0; index < total; index++) Program.Check(ReferenceEquals(exact[index], world.Instance(ids[index])), "Paged lookup must preserve ScriptEntry order.");
            for (int index = 0; index < 10; index++) target.GetComponents<Script>(exact);
            long before = GC.GetAllocatedBytesForCurrentThread();
            for (int index = 0; index < 100; index++) target.GetComponents<Script>(exact);
            long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
            Program.Check(allocated == 0, "Warm caller-buffer component lookup must not allocate temporary collections.");
            Program.Check(target.GetComponents<Script>().Length == total && ReferenceEquals(target.GetComponent<Script>(), exact[0]),
                "Ordinary collection lookup returns all matches and singleton lookup returns the first.");
        };
        world.Invoke(caller, ScriptCallback.UPDATE);
        world.Bridge.DuringTranslate = null;
    }

    static void ManagedComponentMalformedCopyPagePreservesCaller()
    {
        MalformedComponentBridge bridge = new();
        using World world = new(bridge);
        ulong caller = world.Create("ManagedFixtures.ReferenceComponent");
        Program.Check(world.Runtime.ConfigureFields(1, caller, "{\"target\":\"target-guid\"}", false).Success, "The caller must enter a real synchronous scope.");
        ulong[] ids = new ulong[65];
        for (int index = 0; index < ids.Length; index++) ids[index] = world.Create("ManagedFixtures.HealthComponent", 202, 202, "target-guid");
        bridge.Components[202] = ids;
        Script sentinel = world.Instance(caller);
        Script?[] output = new Script?[66]; Array.Fill(output, sentinel);
        bridge.DuringTranslate = () => {
            GameObject target = new(202, 1, "target-guid");
            bridge.PageCalls = 0; bridge.FailPage = 4;
            Program.Throws<InvalidDataException>(() => target.GetComponents<Script>(output));
            Program.Check(bridge.PageCalls == 4 && output.All(value => ReferenceEquals(value, sentinel)),
                "A malformed second copy page after a valid full count and first copy page must leave the entire caller unchanged.");
            bridge.PageCalls = 0; bridge.FailPage = 0;
            Program.Check(target.GetComponents<Script>(output).Written == ids.Length && ReferenceEquals(output[^1], sentinel),
                "The pooled staging buffer must remain usable after failure and preserve the caller tail.");
        };
        world.Invoke(caller, ScriptCallback.UPDATE);
        bridge.DuringTranslate = null;
    }

    sealed class MalformedComponentBridge : Program.RecordingBridge, IScriptNativeBridge
    {
        internal int PageCalls, FailPage;
        public ManagedComponentPage ReadManagedComponents(ScriptExecutionScope scope, ulong objectId, uint offset, Span<ulong> output)
        {
            PageCalls++;
            ulong[] ids = Components[objectId];
            int written = Math.Min(output.Length, ids.Length - checked((int)offset));
            ids.AsSpan((int)offset, written).CopyTo(output);
            if (PageCalls == FailPage) return new(written, offset, false);
            uint next = offset + (uint)written;
            return new(written, next, next == ids.Length);
        }
    }

    static void RetainedLookupAndCallerScope()
    {
        using World world = new();
        ulong caller = world.Create("ManagedFixtures.ReferenceComponent");
        ulong first = world.Create("ManagedFixtures.HealthComponent", 202, 202, "target-guid");
        ulong second = world.Create("ManagedFixtures.HealthComponent", 202, 202, "target-guid");
        world.Bridge.Components[202] = [second, first];
        world.Runtime.TryInstance(1, second, out ManagedScriptRuntime.ScriptInstance? secondRecord);
        secondRecord!.Faulted = true;
        Program.Check(world.Runtime.ConfigureFields(1, caller, "{\"target\":\"target-guid\"}", false).Success, "The caller must connect to another GameObject.");
        world.Invoke(caller, ScriptCallback.AWAKE);
        Script instance = world.Instance(caller);
        ScriptTypeDescriptor type = world.Type("ManagedFixtures.ReferenceComponent");
        Component cached = (Component)Program.Field(type, "m_cachedHealth").Read(instance)!;
        Program.Check(ReferenceEquals(cached, world.Instance(second)), "Typed lookup must preserve ScriptEntry order and include stopped entries.");
        world.Bridge.DuringTranslate = () =>
        {
            world.Bridge.DuringTranslate = null;
            cached.GetType().GetMethod("Move")!.Invoke(cached, [new Vector3(0f, 1f, 0f)]);
            Program.Check(ScriptApiRuntime.Current?.OwnerId == 101, "A normal callee method must retain the caller scope.");
            GameObject target = (GameObject)Program.Field(type, "target").Read(instance)!;
            Program.Check(target.GetComponents<Script>().Length == 2, "GetComponents must return every matching Script in order.");
        };
        world.Invoke(caller, ScriptCallback.UPDATE);
        Program.Check(Equals(Program.Field(world.Type("ManagedFixtures.HealthComponent"), "health").Read(world.Instance(second)), 7)
            && Equals(Program.Field(world.Type("ManagedFixtures.HealthComponent"), "health").Read(world.Instance(first)), 10), "The first matching CLR instance must receive the ordinary public call.");
        Program.Check(world.Bridge.LastOwner == 101 && world.Bridge.LastObject == 202, "Callee-owned Transform must use caller authority and a separate target ID.");
        GameObject oldTarget = (GameObject)Program.Field(type, "target").Read(instance)!;
        Program.Throws<InvalidOperationException>(() => { _ = oldTarget.IsAlive; });
        world.Bridge.Alive.Remove(202);
        world.Bridge.Alive.Add(303);
        world.Bridge.Guids["target-guid"] = 303;
        world.Invoke(caller, ScriptCallback.UPDATE);
        Program.Check(Equals(Program.Field(type, "cachedTargetAlive").Read(instance), false)
            && Equals(Program.Field(type, "cachedTransformAlive").Read(instance), false)
            && Equals(Program.Field(type, "cachedHealthAlive").Read(instance), false), "Cached GO, Transform and Script must all observe native target destruction.");
        Program.Check(Program.Field(type, "moveCached").TryWrite(instance, true), "The failure fixture must enable an explicit stale cache operation.");
        Program.Check(world.Runtime.TryInvoke(1, caller, 101, 1, ScriptCallback.UPDATE).Status == BridgeStatus.CALLBACK_FAILED, "Stale native access must stop the caller entry instead of changing a replacement entity.");
    }

    static void ScriptLifetimeIsDistinctFromObjectLifetime()
    {
        using World world = new();
        ulong oldId = world.Create("ManagedFixtures.HealthComponent");
        Script old = world.Instance(oldId);
        GameObject owner = old.Binding!.GameObject;
        Transform retainedTransform = owner.Transform;
        Program.Check(world.Runtime.LoadCandidate(Program.FixturePath("V2"), 2, internalMode: true).Success, "The fixture must keep both code generations live before retirement.");
        Program.Check(world.Runtime.CreateInstance(2, "ManagedFixtures.LifecycleComponent", 101, out ulong currentId).Success, "The new caller must construct.");
        Program.BindFixture(world.Runtime, 2, currentId);
        Program.Check(world.Runtime.CreateInstance(2, "ManagedFixtures.HealthComponent", 202, out ulong foreignId).Success, "The foreign scene receiver must construct.");
        Program.BindFixture(world.Runtime, 2, foreignId, 202, 202, "target-guid", scene: 2);
        world.Runtime.TryInstance(2, currentId, out ManagedScriptRuntime.ScriptInstance? current);
        world.Runtime.TryInstance(2, foreignId, out ManagedScriptRuntime.ScriptInstance? foreign);
        world.Bridge.DuringTranslate = () =>
        {
            Program.Check(!old.IsAlive && !foreign!.Instance.IsAlive && current!.Instance.IsAlive,
                "A Script must match the current code generation and caller scene, even before its old registration retires.");
            Program.Check(owner.IsAlive && retainedTransform.IsAlive, "GameObject and Transform lifetime must remain independent of code generations.");
            Program.Throws<InvalidOperationException>(() => { _ = old.Transform; });
            retainedTransform.TranslateWorld(new Vector3(0f, 1f, 0f));
        };
        ManagedRuntimeResult invoked = world.Runtime.TryInvoke(2, currentId, 101, 1, ScriptCallback.UPDATE);
        Program.Check(invoked.Success, invoked.Message);
        world.Bridge.DuringTranslate = null;
    }

    static void NamedLookupAndMovementHelpers()
    {
        using World world = new();
        ulong id = world.Create("ManagedFixtures.LifecycleComponent");
        Script instance = world.Instance(id);
        Transform? retained = null;
        world.Bridge.DuringTranslate = () =>
        {
            world.Bridge.DuringTranslate = null;
            Program.Check(instance.Scene.TryFind("Target", out GameObject? target) && target.Name == "Target", "Authoring lookup must expose readable names without requiring GUID syntax.");
            Program.Check(!instance.Scene.TryFind("Missing", out GameObject? missing) && missing is null, "Missing named lookups must clear their output.");
            Program.Throws<InvalidOperationException>(() => instance.Scene.TryFind("Ambiguous", out _));
            GameObject found = target ?? throw new InvalidOperationException("The named lookup fixture is missing.");
            Program.Check(found.TryFindChild(["Visual", "Weapon"], out GameObject? child) && child.Name == "Weapon", "Child lookup must consume explicit name segments.");
            retained = found.Transform;
            retained.MoveTowardsWorld(new Vector3(10f, 0f, 0f), 3f, 2f);
            retained.TranslateWorld(new Vector3(0f, 2f, 0f));
            Program.Check(retained.WorldPosition.Equals(new Vector3(3f, 2f, 0f)), "Movement helpers must preserve world position semantics and explicit distance units.");
            Program.Throws<ArgumentOutOfRangeException>(() => retained.MoveTowardsWorld(Vector3.ZERO, float.NaN));
        };
        world.Invoke(id, ScriptCallback.UPDATE);
        Program.Throws<InvalidOperationException>(() => { _ = retained!.WorldPosition; });
    }

    static void PlainHelperUsesCallerScope()
    {
        using World world = new();
        ScriptCatalog catalog = world.Runtime.GetCatalog(1)!;
        Program.Check(catalog.Find("ManagedFixtures.PlainMovementHelper") is null,
            "An ordinary static helper must not become an attachable Script catalog entry.");
        ulong id = world.Create("ManagedFixtures.PlainHelperComponent");
        Program.Check(world.Runtime.ConfigureFields(1, id, "{\"target\":\"target-guid\"}", false).Success,
            "The caller must configure a target distinct from its own GameObject.");

        world.Invoke(id, ScriptCallback.UPDATE);
        Vector3 moved = new(1f, 0f, 0f);
        Program.Check(world.Bridge.Positions.GetValueOrDefault(202UL).Equals(moved) && world.Bridge.Position.Equals(Vector3.ZERO),
            "A normal helper called by Update must move the passed target, not the caller owner.");
        Program.Check(world.Bridge.LastGeneration == 1 && world.Bridge.LastOwner == 101 && world.Bridge.LastObject == 202,
            "The helper must use the existing caller generation and owner with a separate target identity.");
        Program.Check(ScriptApiRuntime.Current is null, "A normal helper must not retain the completed callback scope.");

        ScriptTypeDescriptor type = world.Type("ManagedFixtures.PlainHelperComponent");
        Type helper = type.Type.Assembly.GetType("ManagedFixtures.PlainMovementHelper", throwOnError: true)!;
        Action<GameObject, Vector3> move = helper.GetMethod("Move", BindingFlags.Public | BindingFlags.Static)!
            .CreateDelegate<Action<GameObject, Vector3>>();
        GameObject target = (GameObject)Program.Field(type, "target").Read(world.Instance(id))!;
        Program.Throws<InvalidOperationException>(() => move(target, new Vector3(5f, 0f, 0f)));
        Program.Check(world.Bridge.Positions.GetValueOrDefault(202UL).Equals(moved),
            "Calling the same helper after the callback must reject Engine access before changing the target.");
    }

    static void UtilityLookupRetainsGenerationIdentity()
    {
        string path = Path.Combine(AppContext.BaseDirectory, "Fixtures", "Dependency", "Fbzz.ManagedDependencyFixtures.dll");
        Program.RecordingBridge bridge = new();
        using ManagedScriptRuntime runtime = new(bridge);
        Type? oldBase = null;
        for (ulong generation = 1; generation <= 2; generation++)
        {
            Program.Check(runtime.LoadCandidate(path, generation, internalMode: true).Success, "A game dependency must load into the candidate collectible context.");
            ScriptTypeDescriptor health = runtime.GetCatalog(generation)!.Find("ManagedDependencyFixtures.DependencyHealthComponent")!;
            Type baseType = health.Type.BaseType!;
            Program.Check(baseType.Assembly != health.Type.Assembly, "The fixture must keep the requested base type in a separate utility DLL.");
            Program.Check(runtime.CreateInstance(generation, health.TypeName, 202, out ulong healthId).Success, "Derived fixture must construct.");
            Program.BindFixture(runtime, generation, healthId, 202, 202, "target-guid");
            Program.Check(runtime.CreateInstance(generation, "ManagedDependencyFixtures.DependencyCallerComponent", 101, out ulong callerId).Success, "The caller must construct.");
            Program.BindFixture(runtime, generation, callerId);
            bridge.Components[202] = [healthId];
            bridge.DuringTranslate = () =>
            {
                GameObject target = new(202, 1, "target-guid");
                MethodInfo lookup = typeof(GameObject).GetMethods().Single(method => method.Name == nameof(GameObject.GetComponents)
                    && method.IsGenericMethodDefinition && method.GetParameters().Length == 0).MakeGenericMethod(baseType);
                Array result = (Array)lookup.Invoke(target, null)!;
                Program.Check(result.Length == 1 && baseType.IsInstanceOfType(result.GetValue(0)), "An abstract Script from the same collectible dependency context must match its derived instance.");
                if (oldBase is not null)
                {
                    MethodInfo stale = typeof(GameObject).GetMethods().Single(method => method.Name == nameof(GameObject.GetComponents)
                        && method.IsGenericMethodDefinition && method.GetParameters().Length == 0).MakeGenericMethod(oldBase);
                    try { stale.Invoke(target, null); }
                    catch (TargetInvocationException exception) when (exception.InnerException is InvalidOperationException) { return; }
                    throw new InvalidOperationException("A same-name type retained from the old dependency context must be rejected.");
                }
            };
            ManagedRuntimeResult invoked = runtime.TryInvoke(generation, callerId, 101, 1, ScriptCallback.UPDATE);
            Program.Check(invoked.Success, invoked.Message);
            bridge.DuringTranslate = null;
            oldBase = baseType;
            Program.Check(runtime.RetireGeneration(generation).Success, "The fixture must retire its registered instances.");
        }
        GC.KeepAlive(oldBase);
    }

    static void GeneratorContracts()
        => Program.Check(Fbzz.ScriptGeneratorTests.GeneratorTests.RunAll() == 0, "Source declaration generation and the limited Inspector-field warning suppressor must pass their real Roslyn tests.");

    static void SourceDiscoveryAndStableManifest()
    {
        IReadOnlyList<ScriptSourceEntry> sources = ScriptManifest.ReadSources(Program.FixturePath("V1"));
        Program.Check(sources.Count != 0 && sources.All((ScriptSourceEntry entry) => entry.Sources.Count != 0), "Source discovery must read generated declarations without loading a game context.");
        Program.Check(sources.Any((ScriptSourceEntry entry) => entry.ClrName == "ManagedFixtures.UppercaseSourceComponent"
            && entry.Sources.Any((ScriptSourceInfo source) => source.Path.EndsWith(".CS", StringComparison.Ordinal))),
            "A compiler-generated source attribute with uppercase .CS must pass the same Host validation as Windows source import.");
        string directory = Directory.CreateTempSubdirectory("fbzz-manifest-test-").FullName;
        Directory.CreateDirectory(directory);
        string path = Path.Combine(directory, "Fbzz.ManagedFixtures.dll");
        File.Copy(Program.FixturePath("V1"), path, overwrite: true);
        string pdb = Path.ChangeExtension(Program.FixturePath("V1"), ".pdb");
        if (File.Exists(pdb)) File.Copy(pdb, Path.ChangeExtension(path, ".pdb"), overwrite: true);
        object[] types = sources.Select((ScriptSourceEntry entry, int index) => (object)new
        {
            id = "cs:" + (index + 1).ToString("x32", CultureInfo.InvariantCulture),
            clrName = entry.ClrName,
            primarySource = entry.Sources[0].Path,
            sources = entry.Sources.Select((ScriptSourceInfo source) => new { path = source.Path, line = source.Line, contentHash = source.ContentHash })
        }).ToArray();
        string manifest = JsonSerializer.Serialize(new { schemaVersion = 1, assemblyName = "Fbzz.ManagedFixtures", types });
        File.WriteAllText(path + ".scripts.json", manifest);
        using ManagedScriptRuntime runtime = new(new Program.RecordingBridge());
        Program.Check(runtime.LoadCandidate(path, 1).Success, "An exact source/type manifest must prepare a published candidate.");
        ScriptTypeDescriptor health = runtime.GetCatalog(1)!.Find("ManagedFixtures.HealthComponent")!;
        Program.Check(health.PersistentId.StartsWith("cs:", StringComparison.Ordinal) && runtime.CreateInstance(1, health.PersistentId, 101, out _).Success,
            "Persistent type IDs must resolve independently of CLR lookup names.");
        string invalid = manifest.Replace(sources[0].Sources[0].ContentHash, new string('0', 64), StringComparison.Ordinal);
        File.WriteAllText(path + ".scripts.json", invalid);
        Program.Check(runtime.LoadCandidate(path, 2).Status == BridgeStatus.LOAD_FAILED && runtime.GetCatalog(1) is not null,
            "Source mismatch must reject the entire candidate while retaining the old catalog.");
        Program.Check(runtime.LoadCandidate(Program.FixturePath("V2"), 3).Status == BridgeStatus.LOAD_FAILED,
            "A normal game load must not silently downgrade an absent manifest to internal type IDs.");
    }

    sealed class World : IDisposable
    {
        internal World(Program.RecordingBridge? bridge = null)
        {
            Bridge = bridge ?? new();
            Runtime = new ManagedScriptRuntime(Bridge);
            ManagedRuntimeResult loaded = Runtime.LoadCandidate(Program.FixturePath("V1"), 1, internalMode: true);
            Program.Check(loaded.Success, loaded.Message);
        }
        internal Program.RecordingBridge Bridge { get; }
        internal ManagedScriptRuntime Runtime { get; }
        internal ScriptTypeDescriptor Type(string name) => Runtime.GetCatalog(1)!.Find(name)!;
        internal ulong Create(string name, ulong owner = 101, ulong objectId = 101, string guid = "owner-101")
        {
            Program.Check(Runtime.CreateInstance(1, name, owner, out ulong id).Success, "Fixture instance must construct.");
            Program.BindFixture(Runtime, 1, id, owner, objectId, guid);
            return id;
        }
        internal Script Instance(ulong id)
        {
            Program.Check(Runtime.TryInstance(1, id, out ManagedScriptRuntime.ScriptInstance? instance), "Fixture instance must remain registered.");
            return instance!.Instance;
        }
        internal void Invoke(ulong id, ScriptCallback callback)
        {
            ManagedRuntimeResult result = Runtime.TryInvoke(1, id, 101, 1, callback);
            Program.Check(result.Success, result.Message);
        }
        public void Dispose() { Runtime.Dispose(); }
    }
}
