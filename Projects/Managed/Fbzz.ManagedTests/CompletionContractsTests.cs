/// @file    CompletionContractsTests.cs
/// @brief   本物 Host による複合保存・候補 rollback・receiver 所有・イベント・待機の検証。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class CompletionContractsTests
{
    internal static void Run()
    {
        StructuredAtomicity();
        PlayStateAndCacheReconnect();
        ListCachesUseExactSavedIdentity();
        TypedRequirementsUseExactPhysicalPresenceAndInheritedMetadata();
        ReceiverOwnershipAndConnections();
        SelfCancellationDoesNotReregister();
        AssetCopiesAndTypeSafety();
    }
    static void StructuredAtomicity()
    {
        using World world = new();
        ulong id = world.Create("ManagedFixtures.CompletionComponent", 101);
        ScriptTypeDescriptor type = world.Runtime.GetCatalog(1)!.Find("ManagedFixtures.CompletionComponent")!;
        ScriptFieldDescriptor field = type.Fields.Single((ScriptFieldDescriptor field) => field.Name == "settings");
        Program.Check(field.Kind == ScriptFieldKind.STRUCTURED && type.Fields.Single((ScriptFieldDescriptor field) => field.Name == "rotation").Kind == ScriptFieldKind.QUATERNION,
            "Serializable classes, List and Quaternion must use real supported codecs.");
        Program.Check(world.Runtime.ConfigureFields(1, id, "{\"settings\":{\"label\":\"changed\",\"weights\":[3,4],\"target\":\"target-guid\"},\"rotation\":[0,0,0,1]}", false).Success,
            "Nested values must configure atomically through ordinary JSON.");
        Script instance = world.Instance(1, id);
        string before = StructuredFieldCodec.Encode(field.Read(instance), field.ValueType);
        Program.Check(!world.Runtime.ConfigureFields(1, id, "{\"settings\":{\"label\":\"bad\",\"weights\":[1e100]}}", false).Success,
            "A nonfinite nested value must reject the entire write.");
        Program.Check(before == StructuredFieldCodec.Encode(field.Read(instance), field.ValueType), "Failed nested configure must not modify the previous value.");
        object? copy = StructuredFieldCodec.Decode(before, field.ValueType, (string guid) => new GameObject(0, 1, guid));
        Program.Check(StructuredFieldCodec.Encode(copy, field.ValueType) == before, "Versioned nested values must round trip without evaluating properties.");
        Program.Throws<System.IO.InvalidDataException>(() => StructuredFieldCodec.Decode(before.Replace("\"$version\":1", "\"$version\":2", StringComparison.Ordinal), field.ValueType,
            (string guid) => new GameObject(0, 1, guid)));
        Node node = new(); node.next = node;
        Program.Check(!StructuredFieldCodec.Validate(node, typeof(Node)), "Actual object cycles must reject before a write.");
        Program.Check(!StructuredFieldCodec.Validate(new List<float> { float.NaN }, typeof(List<float>)), "List codecs must reject nonfinite values.");
        foreach (Type unsupported in new Type[] { typeof(object), typeof(Dictionary<string, int>), typeof(DateTime), typeof(Guid), typeof(TimeSpan), typeof(Uri) })
            Program.Check(!StructuredFieldCodec.IsSupported(unsupported), "Framework values must not become lossy empty schemas: " + unsupported.FullName);
        foreach (Type unsupported in new Type[] { typeof(DictionaryValue), typeof(ListValue), typeof(FrameworkBasedValue), typeof(EnumerableValue) })
            Program.Check(!StructuredFieldCodec.IsSupported(unsupported), "Framework inheritance and custom collections must not erase their state: " + unsupported.FullName);
        SavedDerived inherited = new();
        Program.Check(StructuredFieldCodec.IsSupported(typeof(SavedDerived)) && StructuredFieldCodec.IsSupported(typeof(SavedValue)),
            "Game class inheritance and user structs must retain their field codecs.");
        string inheritedJson = StructuredFieldCodec.Encode(inherited, typeof(SavedDerived));
        using (JsonDocument document = JsonDocument.Parse(inheritedJson))
        {
            JsonElement value = document.RootElement.GetProperty("value");
            Program.Check(value.GetProperty("baseSetting").GetSingle() == 2f && value.GetProperty("child").GetInt32() == 3,
                "Private Setting fields from game base classes must be saved alongside derived fields.");
        }
        object? inheritedCopy = StructuredFieldCodec.Decode(inheritedJson.Replace("\"baseSetting\":2", "\"baseSetting\":6", StringComparison.Ordinal),
            typeof(SavedDerived), static (string guid) => new GameObject(0, 1, guid));
        Program.Check(StructuredFieldCodec.Encode(inheritedCopy, typeof(SavedDerived)).Contains("\"baseSetting\":6", StringComparison.Ordinal),
            "Private Setting fields from game base classes must restore through FieldInfo.");
        Program.Check(!StructuredFieldCodec.Validate(new ShadowedValue(), typeof(ShadowedValue)),
            "A hidden inherited field with the same persistent key must reject its ambiguous schema.");
        ReservedTokenValue reserved = new();
        Program.Throws<System.IO.InvalidDataException>(() => StructuredFieldCodec.Encode(reserved, typeof(ReservedTokenValue)));
        Program.Check(!StructuredFieldCodec.Validate(reserved, typeof(ReservedTokenValue)) && reserved.text == "source-object",
            "A plain GUID string must not be emitted as a fake GameObject token or modified during rejection.");
        Program.Throws<System.IO.InvalidDataException>(() => StructuredFieldCodec.IsSupported(typeof(ReservedPreviousTokenValue)));
        ModuleBuilder module = AssemblyBuilder.DefineDynamicAssembly(new AssemblyName("ReservedNestedKeyFixture"),
            AssemblyBuilderAccess.RunAndCollect).DefineDynamicModule("main");
        TypeBuilder actor = module.DefineType("ReservedKeyComponent", TypeAttributes.Public, typeof(Script));
        actor.DefineDefaultConstructor(MethodAttributes.Public);
        actor.DefineField("options", typeof(ReservedTokenValue), FieldAttributes.Public);
        CatalogBuildResult rejected = ScriptCatalogBuilder.Build(actor.CreateType()!.Assembly);
        Program.Check(!rejected.Success && rejected.Diagnostics.Any(static (ScriptDiagnostic diagnostic) =>
            diagnostic.Code == "CS_STRUCTURED_SCHEMA" && diagnostic.MemberName == "options" && diagnostic.Message.Contains("'$go'", StringComparison.Ordinal)),
            "Reserved nested field keys must reject discovery with an explicit schema diagnostic before any Scene or Prefab write.");
    }
    static void PlayStateAndCacheReconnect()
    {
        using World world = new();
        ulong target = world.Create("ManagedFixtures.HealthComponent", 202);
        world.Bridge.Components[202] = [target];
        ulong id = world.Create("ManagedFixtures.CompletionComponent", 101);
        Program.Check(world.Runtime.TryInvoke(1, id, 101, 1, ScriptCallback.AWAKE).Success, "Initial cache discovery must run.");
        Program.Check(world.Runtime.TryInvoke(1, id, 101, 2, ScriptCallback.START).Success, "Start must run once.");
        Program.Check(world.Runtime.TryInvoke(1, id, 101, 3, ScriptCallback.UPDATE).Success, "The state must advance before reload.");
        Program.Check(world.Runtime.CaptureReload(1, id, out string state).Success, "Play state capture must use safe fields.");
        Program.Check(world.Runtime.LoadCandidate(Program.FixturePath("V2"), 2, true).Success, "The candidate must prepare while the old generation remains registered.");
        ulong nextTarget = world.Create("ManagedFixtures.HealthComponent", 202, generation: 2);
        ulong next = world.Create("ManagedFixtures.CompletionComponent", 101, generation: 2);
        Program.Check(world.Runtime.RestoreReload(2, next, state).Success, "All candidates must be built before reference caches reconnect.");
        Script instance = world.Instance(2, next);
        ScriptTypeDescriptor descriptor = world.Runtime.GetCatalog(2)!.Find("ManagedFixtures.CompletionComponent")!;
        object? Value(string name) => descriptor.Fields.Single((ScriptFieldDescriptor field) => field.Name == name).Read(instance);
        Program.Check(Equals(Value("awakeCount"), 1) && Equals(Value("startCount"), 1) && Equals(Value("m_elapsed"), 0.25f) && Equals(Value("m_transient"), 0f),
            "Saved and KeepOnReload values must survive while unmarked runtime state resets without repeated Awake/Start.");
        Program.Check(ReferenceEquals(Value("m_health"), world.Instance(2, nextTarget)) && !ReferenceEquals(Value("m_health"), world.Instance(1, target)),
            "Direct Script caches must point at the exact new owner entry.");
        string bad = state.Replace("\"kind\":14", "\"kind\":13", StringComparison.Ordinal);
        Program.Check(!world.Runtime.RestoreReload(2, next, bad).Success && Equals(Value("m_elapsed"), 0.25f),
            "Failed candidate state restoration must not partially change values.");
        Program.Check(world.Runtime.TryInvoke(1, id, 101, 4, ScriptCallback.UPDATE).Success, "Rejected preparation must leave old code executable.");
        Program.Check(world.Runtime.RetireGeneration(1).Success, "Old instances must retire after commit.");
    }
    static void ListCachesUseExactSavedIdentity()
    {
        using World world = new();
        const string firstId = "11111111-1111-4111-8111-111111111111";
        const string secondId = "22222222-2222-4222-8222-222222222222";
        ulong first = world.Create("ManagedFixtures.HealthComponent", 202, savedScriptId: firstId);
        ulong second = world.Create("ManagedFixtures.HealthComponent", 202, savedScriptId: secondId);
        ulong source = world.Create("ManagedFixtures.CompletionComponent", 101);
        Script original = world.Instance(1, source);
        Type originalType = original.GetType();
        GameObject held = new(202, 1, "target-guid");
        originalType.BaseType!.GetField("shadowedCache", BindingFlags.Instance | BindingFlags.NonPublic)!.SetValue(original, held);
        void SetList(string name, params object?[] entries)
        {
            FieldInfo field = originalType.GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!;
            IList list = (IList)Activator.CreateInstance(field.FieldType)!;
            foreach (object? value in entries) list.Add(value);
            field.SetValue(original, list);
        }
        SetList("m_objectCaches", held, null, held);
        SetList("m_healthCaches", world.Instance(1, second), null, world.Instance(1, first), world.Instance(1, second));
        Camera camera = (Camera)NativeComponentFactory.Create(typeof(Camera), held, 100)!;
        SetList("m_componentCaches", world.Instance(1, second), held.Transform, null, world.Instance(1, first), camera, camera);
        Program.Check(world.Runtime.CaptureReload(1, source, out string snapshot).Success, "Direct typed Lists must capture null, duplicate and ordered caches.");
        Program.Check(world.Runtime.LoadCandidate(Program.FixturePath("V2"), 2, true).Success, "The list candidate must prepare.");
        ulong nextSecond = world.Create("ManagedFixtures.HealthComponent", 202, 2, secondId);
        ulong nextFirst = world.Create("ManagedFixtures.HealthComponent", 202, 2, firstId);
        ulong candidate = world.Create("ManagedFixtures.CompletionComponent", 101, 2);
        Program.Check(world.Runtime.RestoreReload(2, candidate, snapshot).Success, "Direct Lists must restore after all exact saved identities bind.");
        Script restored = world.Instance(2, candidate);
        IList Cache(string name) => (IList)restored.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(restored)!;
        IList health = Cache("m_healthCaches"), objects = Cache("m_objectCaches"), components = Cache("m_componentCaches");
        Program.Check(health.Count == 4 && ReferenceEquals(health[0], world.Instance(2, nextSecond)) && health[1] is null
            && ReferenceEquals(health[2], world.Instance(2, nextFirst)) && ReferenceEquals(health[3], health[0]),
            "Duplicate same-type Script entries must reconnect by saved identity, preserving list positions and duplicate references.");
        Program.Check(objects.Count == 3 && objects[1] is null && ((GameObject)objects[0]!).ObjectId == 202
            && ((GameObject)objects[2]!).ObjectId == 202 && components[1] is Transform && components[2] is null,
            "GameObject and heterogeneous Component Lists must retain null slots and native facade types.");
        Program.Check(restored.GetType().BaseType!.GetField("shadowedCache", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(restored)
            is GameObject inherited && inherited.ObjectId == 202,
            "A derived saved field must not exclude a hidden base private cache with the same field name.");
        world.Bridge.DuringTranslate = () => {
            world.Bridge.DuringTranslate = null;
            Camera retained = (Camera)components[4]!;
            Camera fresh = retained.GameObject.GetComponent<Camera>()!;
            Program.Check(retained == (Component)components[5]! && retained.Equals(fresh) && retained.GetHashCode() == fresh.GetHashCode(),
                "Duplicate reconstructed native cache entries and fresh lookup must share immutable exact identity and hash.");
            world.Bridge.NativeLifetime = 101;
            Camera replacement = retained.GameObject.GetComponent<Camera>()!;
            Program.Check(retained != replacement && !retained.Equals(replacement),
                "A newly added native row must not compare equal to the restored expired epoch.");
        };
        Program.Check(world.Runtime.TryInvoke(2, candidate, 101, 1, ScriptCallback.UPDATE).Success,
            "Reload alias identity checks must run in a real Host callback scope.");
        Program.Check(!world.Runtime.CreateInstance(2, "ManagedFixtures.HealthComponent", 202, firstId, out _).Success,
            "A duplicate explicit saved Script identity must reject before another instance is constructed.");
        Program.Check(world.Runtime.BindOwner(2, nextFirst, 202, 203, 1, "target-guid").Success,
            "The fixture replacement may retain its GUID while changing its runtime object identity.");
        Program.Check(world.Runtime.RestoreReload(2, candidate, snapshot).Success && Cache("m_healthCaches")[2] is null,
            "A saved Script ID and GUID must not revive a cache on a replacement GameObject.");
    }
    static void TypedRequirementsUseExactPhysicalPresenceAndInheritedMetadata()
    {
        using World world = new();
        world.Bridge.CameraPresent = false;
        ulong dependent = world.Create("ManagedFixtures.RequiredAuthoringComponent", 101);
        ScriptTypeDescriptor type = world.Runtime.GetCatalog(1)!.Find("ManagedFixtures.RequiredAuthoringComponent")!;
        Program.Check(type.ExecutionOrder == -20 && type.RequiredComponentTypes.Any(value => value == typeof(Camera))
            && type.RequiredComponentTypes.Any(value => value.Name == "RequiredPeerBase"),
            "Requirements must combine base declarations and inherited execution order without evaluating game constructors.");
        Program.Check(!world.Runtime.ValidateReferences(1, dependent).Success, "A missing required native row must reject preparation.");
        world.Bridge.CameraPresent = true;
        world.Create("ManagedFixtures.RequiredPeerComponent", 202);
        Program.Check(!world.Runtime.ValidateReferences(1, dependent).Success, "A matching Script on another GameObject must not satisfy a requirement.");
        ulong peer = world.Create("ManagedFixtures.RequiredPeerComponent", 101);
        Program.Check(world.Runtime.ValidateReferences(1, dependent).Success, "An assignable concrete Script on the same owner must satisfy its abstract requirement.");
        Program.Check(world.Runtime.TryInvoke(1, dependent, 101, 1, ScriptCallback.UPDATE).Success, "Satisfied requirements must permit an ordinary update.");
        world.Runtime.TryInstance(1, peer, out ManagedScriptRuntime.ScriptInstance? record);
        world.Bridge.PendingScripts.Add(record!.Binding.SavedScriptId);
        ManagedRuntimeResult removed = world.Runtime.TryInvoke(1, dependent, 101, 2, ScriptCallback.UPDATE);
        Program.Check(!removed.Success && removed.Message.Contains("RequiredPeerBase", StringComparison.Ordinal),
            "Cleanup-deferred physical removal must reject the next dependent callback while the CLR record is still held.");
        world.Bridge.PendingScripts.Clear(); world.Bridge.CameraPresent = false;
        Program.Check(!world.Runtime.TryInvoke(1, dependent, 101, 3, ScriptCallback.UPDATE).Success,
            "Removing a native row must reject the dependent callback before user code runs.");
        AssemblyBuilder invalid = AssemblyBuilder.DefineDynamicAssembly(new AssemblyName("InvalidRequirement"), AssemblyBuilderAccess.RunAndCollect);
        TypeBuilder actor = invalid.DefineDynamicModule("main").DefineType("InvalidRequirementComponent", TypeAttributes.Public, typeof(Script));
        actor.DefineDefaultConstructor(MethodAttributes.Public);
        actor.SetCustomAttribute(new CustomAttributeBuilder(typeof(RequireComponentAttribute).GetConstructor([typeof(Type[])])!, [new Type[] { typeof(string) }]));
        CatalogBuildResult rejected = ScriptCatalogBuilder.Build(actor.CreateType()!.Assembly);
        Program.Check(!rejected.Success && rejected.Diagnostics.Any(value => value.Code == "CS_COMPONENT_REQUIREMENT"),
            "Host discovery must reject an unsupported required type even when analyzer checks are bypassed.");
    }
    static void ReceiverOwnershipAndConnections()
    {
        using World world = new();
        ulong callerId = world.Create("ManagedFixtures.CompletionComponent", 101);
        ulong receiverId = world.Create("ManagedFixtures.CompletionComponent", 202);
        Script receiver = world.Instance(1, receiverId);
        int called = 0;
        JobHandle handle = default;
        world.Bridge.DuringTranslate = () =>
        {
            world.Bridge.DuringTranslate = null;
            handle = receiver.Jobs.Wait(0f, () => { called++; Program.Check(ScriptApiRuntime.Current!.Value.InstanceId == receiverId, "Owned callback must use B scope."); });
            Program.Check(world.Bridge.LastCaller == callerId && world.Bridge.LastReceiver == receiverId && handle.Instance == receiverId,
                "A-to-B scheduling must keep caller auth separate from B ownership.");
        };
        Program.Check(world.Runtime.TryInvoke(1, callerId, 101, 1, ScriptCallback.UPDATE).Success, "Caller must register the receiver job.");
        Program.Check(world.Runtime.InvokeOwned(1, receiverId, 202, 2, handle.Token, false, "", 0, true).Success && called == 1,
            "One-shot job delivery must invoke exactly once.");
        Program.Check(world.Runtime.InvokeOwned(1, receiverId, 202, 3, handle.Token, false, "", 0, true).Status == BridgeStatus.NOT_FOUND && called == 1,
            "Consumed tokens must not redeliver.");
        Program.Check(world.Runtime.TryInvoke(1, receiverId, 202, 4, ScriptCallback.CONNECT).Success && world.Bridge.Subscriptions.Count == 1,
            "A successful Connect must publish its registration batch.");
        ulong eventToken = world.Bridge.Subscriptions.Single();
        Program.Check(world.Runtime.InvokeOwned(1, receiverId, 202, 5, eventToken, true, "{\"$version\":1,\"value\":7}", 0, false).Success,
            "Typed synchronous events must execute in the receiver scope.");
        Program.Check(Equals(world.Field(receiverId, "eventSum"), 7), "Event payload must reach the real game handler.");
        Program.Check(world.Runtime.ConfigureFields(1, callerId, "{\"failConnect\":true}", false).Success, "Failure fixture must configure.");
        Program.Check(!world.Runtime.TryInvoke(1, callerId, 101, 6, ScriptCallback.CONNECT).Success && world.Bridge.Subscriptions.Count == 1,
            "Failed Connect must not publish pending subscriptions or delete another receiver's subscriptions.");
        Program.Check(world.Runtime.CancelOwnedJobs(1, receiverId).Success
            && world.Runtime.InvokeOwned(1, receiverId, 202, 7, eventToken, true, "{\"$version\":1,\"value\":1}", 0, false).Status == BridgeStatus.NOT_FOUND,
            "Owner cleanup must remove every managed delegate token.");
    }
    static void SelfCancellationDoesNotReregister()
    {
        using World world = new();
        ulong id = world.Create("ManagedFixtures.CompletionComponent", 101);
        Script instance = world.Instance(1, id);
        foreach (bool cancelOnCurrent in new[] { false, true })
        {
            int before = world.Bridge.Registrations;
            JobHandle handle = default;
            SelfCancellingRoutine routine = new(() => instance.Jobs.CancelAll(), cancelOnCurrent);
            world.Bridge.DuringTranslate = () =>
            {
                world.Bridge.DuringTranslate = null;
                handle = instance.Jobs.Run(routine);
                Program.Check(handle.IsValid && routine.Disposals == 0, "Run must reserve its first owned delivery without advancing the routine in the caller scope.");
            };
            Program.Check(world.Runtime.TryInvoke(1, id, 101, 10, ScriptCallback.UPDATE).Success && world.Bridge.Registrations == before + 1,
                "Run must initially reserve exactly one native delivery.");
            Program.Check(world.Runtime.InvokeOwned(1, id, 101, 11, handle.Token, false, "", 0, true).Success,
                "MoveNext / Current self-cancellation must remain a successful owned callback.");
            Program.Check(routine.Disposals == 1 && world.Bridge.Registrations == before + 1 && world.Bridge.NativeReservations == 0,
                "Self-cancellation must dispose once and not register another native delivery.");
            Program.Check(world.Runtime.InvokeOwned(1, id, 101, 12, handle.Token, false, "", 0, true).Status == BridgeStatus.NOT_FOUND,
                "The self-cancelled routine must stay consumed.");
        }
        JobHandle conditional = default;
        IEnumerable<Wait> Routine()
        {
            yield return Wait.Until(() => { instance.Jobs.CancelAll(); return false; });
            throw new InvalidOperationException("A cancelled condition must not advance the routine.");
        }
        int beforeCondition = world.Bridge.Registrations;
        world.Bridge.DuringTranslate = () =>
        {
            world.Bridge.DuringTranslate = null;
            conditional = instance.Jobs.Run(Routine().GetEnumerator());
        };
        Program.Check(world.Runtime.TryInvoke(1, id, 101, 13, ScriptCallback.UPDATE).Success && conditional.IsValid && world.Bridge.Registrations == beforeCondition + 1,
            "The routine must reserve its initial owned delivery.");
        Program.Check(world.Runtime.InvokeOwned(1, id, 101, 14, conditional.Token, false, "", 0, true).Success && world.Bridge.Registrations == beforeCondition + 2,
            "The first owned MoveNext must establish the wait condition.");
        Program.Check(world.Runtime.InvokeOwned(1, id, 101, 15, conditional.Token, false, "", 0, true).Success
            && world.Bridge.Registrations == beforeCondition + 2 && world.Bridge.NativeReservations == 0,
            "A condition which cancels its owner must not register another delivery.");
        Program.Check(world.Runtime.InvokeOwned(1, id, 101, 16, conditional.Token, false, "", 0, true).Status == BridgeStatus.NOT_FOUND,
            "The cancelled condition token must stay consumed.");
    }

    sealed class SelfCancellingRoutine(Action cancel, bool cancelOnCurrent) : IEnumerator<Wait>
    {
        internal int Disposals;
        public Wait Current { get { if (cancelOnCurrent) cancel(); return Wait.ForFrames(1); } }
        object System.Collections.IEnumerator.Current => Current;
        public bool MoveNext() { if (!cancelOnCurrent) cancel(); return true; }
        public void Reset() => throw new NotSupportedException();
        public void Dispose() { Disposals++; }
    }

    static void AssetCopiesAndTypeSafety()
    {
        using World world = new();
        ulong id = world.Create("ManagedFixtures.CompletionComponent", 101);
        Type assetType = world.Runtime.GetCatalog(1)!.DataAssets.Single((DataAssetTypeDescriptor type) => type.TypeName == "ManagedFixtures.WeaponData").Type;
        Program.Check(world.Runtime.GetCatalog(1)!.Find("ManagedFixtures.WeaponData") is null, "DataAsset must not become an attachable Script.");
        world.Bridge.AssetJson = "{\"type\":\"ManagedFixtures.WeaponData\",\"damage\":12,\"rotation\":[0,0,0,1],\"weights\":[4,5]}";
        world.Bridge.DuringTranslate = () =>
        {
            world.Bridge.DuringTranslate = null;
            ScriptExecutionScope scope = ScriptApiRuntime.Current!.Value;
            DataAsset first = ((IScriptDataAssetRuntime)world.Runtime).LoadDataAsset(scope, assetType, "guid:fixture")!;
            DataAsset second = ((IScriptDataAssetRuntime)world.Runtime).LoadDataAsset(scope, assetType, "guid:fixture")!;
            Program.Check(!ReferenceEquals(first, second) && Equals(assetType.GetField("damage")!.GetValue(first), 12), "Each asset read must produce an independent validated value copy.");
            world.Bridge.AssetJson = "{\"type\":\"WrongType\",\"damage\":99}";
            Program.Throws<System.IO.InvalidDataException>(() => ((IScriptDataAssetRuntime)world.Runtime).LoadDataAsset(scope, assetType, "guid:fixture"));
            Program.Check(ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, scope), "Asset preparation failure must restore caller scope.");
        };
        Program.Check(world.Runtime.TryInvoke(1, id, 101, 1, ScriptCallback.UPDATE).Success, "Asset access must run through the real current scope.");
    }
    [Serializable]
    sealed class Node { public Node? next = null; }

    [Serializable]
    sealed class DictionaryValue : Dictionary<string, int> { }
    [Serializable]
    sealed class ListValue : List<float> { }
    [Serializable]
    sealed class FrameworkBasedValue : EventArgs { public int value = 1; }
    [Serializable]
    sealed class EnumerableValue : IEnumerable<int>
    {
        public IEnumerator<int> GetEnumerator() => throw new InvalidOperationException("Type validation must not enumerate user values.");
        IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
    }
    [Serializable]
    class SavedBase
    {
        [Setting, FieldKey("baseSetting")] float m_baseSetting = 2f;
        public int value = 7;
        public float ReadBaseSetting() => m_baseSetting;
    }
    [Serializable]
    sealed class SavedDerived : SavedBase { public int child = 3; }
    [Serializable]
    sealed class ShadowedValue : SavedBase { public new int value = 8; }
    [Serializable]
    struct SavedValue { public float amount = 1f; public SavedValue() { } }
    [Serializable]
    sealed class ReservedTokenValue { [FieldKey("$go")] public string text = "source-object"; }
    [Serializable]
    sealed class ReservedPreviousTokenValue { [PreviousFieldKey("$go")] public string text = "source-object"; }

    sealed class World : IDisposable
    {
        internal World()
        { Runtime = new(Bridge); ManagedRuntimeResult result = Runtime.LoadCandidate(Program.FixturePath("V1"), 1, true); Program.Check(result.Success, result.Message); }
        internal CompletionBridge Bridge { get; } = new();
        internal ManagedScriptRuntime Runtime { get; }
        readonly Dictionary<(string Type, ulong Owner), string> m_savedIds = new();
        internal ulong Create(string type, ulong owner, ulong generation = 1, string? savedScriptId = null)
        {
            if (savedScriptId is null) {
                if (!m_savedIds.TryGetValue((type, owner), out savedScriptId)) {
                    savedScriptId = Guid.NewGuid().ToString("D");
                    m_savedIds.Add((type, owner), savedScriptId);
                }
            }
            Program.Check(Runtime.CreateInstance(generation, type, owner, savedScriptId, out ulong id).Success, "Completion fixture must construct.");
            Program.BindFixture(Runtime, generation, id, owner, owner, owner == 101 ? "owner-101" : "target-guid");
            return id;
        }
        internal Script Instance(ulong generation, ulong id)
        { Program.Check(Runtime.TryInstance(generation, id, out ManagedScriptRuntime.ScriptInstance? record), "The fixture must remain registered."); return record!.Instance; }
        internal object? Field(ulong id, string name)
        { ScriptTypeDescriptor type = Runtime.GetCatalog(1)!.Find("ManagedFixtures.CompletionComponent")!; return type.Fields.Single((ScriptFieldDescriptor field) => field.Name == name).Read(Instance(1, id)); }
        public void Dispose() { Runtime.Dispose(); }
    }
    sealed class CompletionBridge : Program.RecordingBridge, IScriptJobNativeBridge, IScriptDataAssetNativeBridge, IScriptNativeBridge
    {
        internal bool CameraPresent = true;
        internal ulong NativeLifetime = 100;
        internal readonly HashSet<string> PendingScripts = new(StringComparer.Ordinal);
        public bool ReadComponent(ScriptExecutionScope scope, ulong objectId, uint kind) => kind == 0 || (kind == 8 && CameraPresent);
        public ulong ReadComponentLifetime(ScriptExecutionScope scope, ulong objectId, uint kind) => kind == 0 ? objectId : kind == 8 && CameraPresent ? NativeLifetime : 0;
        public bool HasScriptEntry(ScriptExecutionScope scope, ulong objectId, string savedScriptId) => !PendingScripts.Contains(savedScriptId);
        internal ulong LastCaller;
        internal ulong LastReceiver;
        internal int Registrations;
        internal readonly HashSet<ulong> Subscriptions = new();
        readonly Dictionary<ulong, ulong> m_owners = new();
        internal int NativeReservations => m_owners.Count;
        internal string? AssetJson;
        public void RegisterJob(ScriptExecutionScope scope, ulong receiver, ulong token, float delay, float interval, uint kind, bool unscaled, JobEase ease, float from, float to)
        { Registrations++; LastCaller = scope.InstanceId; LastReceiver = receiver; m_owners[token] = receiver; }
        public void CancelJob(ScriptExecutionScope scope, ulong receiver, ulong token) { m_owners.Remove(token); Subscriptions.Remove(token); }
        public void CancelOwner(ScriptExecutionScope scope, ulong receiver)
        { foreach (ulong token in m_owners.Where((KeyValuePair<ulong, ulong> value) => value.Value == receiver).Select((KeyValuePair<ulong, ulong> value) => value.Key).ToArray()) { m_owners.Remove(token); Subscriptions.Remove(token); } }
        public void SubscribeEvent(ScriptExecutionScope scope, ulong receiver, ulong token, string channel)
        { m_owners[token] = receiver; Subscriptions.Add(token); }
        public void PublishEvent(ScriptExecutionScope scope, string channel, string json) { }
        public GameObject ResolveEventReference(ScriptExecutionScope scope, string guid) => new(202, scope.SceneLifetime, guid);
        public string? ReadDataAsset(ScriptExecutionScope scope, string reference) => AssetJson;
    }
}
