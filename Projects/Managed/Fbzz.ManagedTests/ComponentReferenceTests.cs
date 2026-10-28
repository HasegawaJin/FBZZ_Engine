/// @file    ComponentReferenceTests.cs
/// @brief   保存住所、native/Script 個体寿命と明示 reload migration の境界を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class ComponentReferenceTests
{
    const string FIRST = "11111111-1111-4111-8111-111111111111";
    const string SECOND = "22222222-2222-4222-8222-222222222222";
    const string FIRST_BIRTH = "33333333-3333-4333-8333-333333333333";
    const string SECOND_BIRTH = "44444444-4444-4444-8444-444444444444";
    const string REPLACEMENT_BIRTH = "55555555-5555-4555-8555-555555555555";
    const string TYPE = "ManagedFixtures.TypedReferenceComponent";
    internal static (string Name, Action Test)[] Cases => [
        ("TypedReferenceNativeEpochAndExplicitReconnect", NativeEpoch),
        ("TypedReferenceScriptReloadPreservesFieldsButNotHeldReferences", ScriptReload),
        ("TypedReferenceCodecSpoolAndAssetKinds", Codec),
        ("WorldFamilyReferencesKeepExactEpochAndPortableAddressesBeyondKind32", WorldFamilyReferences)
        , ("RenderingReferencesReloadSpoolAndReaddPreserveExactBirths", RenderingReferences)
    ];
    static void NativeEpoch()
    {
        Bridge bridge = new(); using ManagedScriptRuntime runtime = new(bridge);
        ScriptApiRuntime.Current = Scope(runtime, bridge, 1, 1);
        ComponentRef<RigidBody> held = new((RigidBody)NativeComponentFactory.Create(typeof(RigidBody), new GameObject(202, 1, "target-guid"), 41)!);
        Program.Check(held.TryGet(out RigidBody? body) && body is not null && !held.IsEmpty, "A typed native reference retains its exact physical row.");
        bridge.Epoch = 0;
        Program.Check(!held.TryGet(out _) && held.IsMissing, "Pending/absent rows must expire without GUID lookup.");
        bridge.Epoch = 42;
        Program.Check(!held.TryGet(out _) && held.Resolve().TryGet(out _), "A replacement row is reachable only through an explicit Resolve producing a new reference.");
        Program.Check(bridge.Resolves == 1, "TryGet must never resolve saved addresses.");
        ScriptApiRuntime.Current = null;
        Program.Check(default(ComponentRef<Component>).IsEmpty, "Empty checks require no Engine scope.");
        Program.Throws<InvalidOperationException>(() => held.TryGet(out _));
    }
    static void WorldFamilyReferences()
    {
        Bridge bridge = new(); using ManagedScriptRuntime runtime = new(bridge);
        ScriptApiRuntime.Current = Scope(runtime, bridge, 1, 1);
        GameObject owner = new(202, 1, "target-guid");
        foreach (Type type in new[] { typeof(VFX), typeof(SequencePlayer), typeof(Terrain), typeof(Water) }) {
            bridge.Epoch = 41;
            Component component = NativeComponentFactory.Create(type, owner, 41)!;
            ComponentRef<Component> held = new(component);
            Program.Check(held.TryGet(out Component? alive) && alive.GetType() == type, "New native kinds map to the actual typed facade.");
            bridge.Epoch = 0;
            Program.Check(!held.TryGet(out _), "Pending removal invalidates every new facade kind immediately.");
            bridge.Epoch = 42;
            Program.Check(!held.TryGet(out _) && held.Resolve().TryGet(out alive) && alive.GetType() == type,
                "Held refs do not revive across row re-add; explicit resolution obtains the correct new row type.");
        }
        ComponentReferenceIdentity identity = new("native", "target-guid", (uint)NativeComponentKind.WATER, "", "", 202, 1, 42);
        ComponentRef<Water> water = new(identity);
        string json = StructuredFieldCodec.EncodeValue(water, typeof(ComponentRef<Water>));
        using JsonDocument document = JsonDocument.Parse(json);
        Program.Check(document.RootElement.GetProperty("$component").GetProperty("componentKind").GetUInt32() == 33
            && !json.Contains("componentLifetime", StringComparison.Ordinal), "Saving Water33 preserves its stable kind and omits runtime epochs.");
        using ImmutableValueSnapshot snapshot = ImmutableValueSnapshot.Capture(water, typeof(ComponentRef<Water>), new(ResidentBytes: 1));
        ComponentRef<Water> restored = (ComponentRef<Water>)snapshot.RestoreValue(typeof(ComponentRef<Water>))!;
        Program.Check(((IComponentReference)restored).Identity == identity && restored.TryGet(out _), "Spool restoration retains the exact birth for new high kinds.");
        Program.Check(NativeComponentFactory.GetType((uint)NativeComponentKind.COUNT) is null,
            "COUNT is an exclusive bound and never a fabricated Component.");
        ScriptApiRuntime.Current = null;
    }
    static void ScriptReload()
    {
        Bridge bridge = new(); using ManagedScriptRuntime runtime = new(bridge);
        Success(runtime.LoadCandidate(Program.FixturePath("V1"), 1, true));
        ulong first = Create(runtime, bridge, 1, FIRST, FIRST_BIRTH), second = Create(runtime, bridge, 1, SECOND, SECOND_BIRTH);
        Program.Check(!runtime.CreateInstance(1, TYPE, 101, FIRST, out _).Success, "Duplicate explicit saved IDs must reject before constructing another Script.");
        runtime.TryInstance(1, first, out ManagedScriptRuntime.ScriptInstance? source);
        runtime.TryInstance(1, second, out ManagedScriptRuntime.ScriptInstance? peer);
        ScriptApiRuntime.Current = Scope(runtime, bridge, 1, first);
        ComponentRef<Component> held = new(peer!.Instance);
        ComponentRef<Component> self = new(source!.Instance);
        Program.Check(held.TryGet(out Component? selected) && ReferenceEquals(selected, peer.Instance)
            && self.TryGet(out selected) && ReferenceEquals(selected, source.Instance), "Duplicate same-type Scripts remain distinct through saved UUID and exact instance.");
        Program.Field(source.Type, "target").AssignPrepared(source.Instance, held);
        ScriptApiRuntime.Current = null;
        Success(runtime.CaptureReload(1, first, out string snapshot));
        Success(runtime.LoadCandidate(Program.FixturePath("V2"), 2, true));
        ulong newFirst = Create(runtime, bridge, 2, FIRST, FIRST_BIRTH), newSecond = Create(runtime, bridge, 2, SECOND, SECOND_BIRTH);
        Success(runtime.RestoreReload(2, newFirst, snapshot));
        runtime.TryInstance(2, newFirst, out ManagedScriptRuntime.ScriptInstance? next);
        ComponentRef<Component> restored = (ComponentRef<Component>)Program.Field(next!.Type, "target").Read(next.Instance)!;
        ScriptApiRuntime.Current = Scope(runtime, bridge, 2, newFirst);
        Program.Check(!held.TryGet(out _) && restored.TryGet(out Component? migrated) && migrated.Binding!.InstanceId == newSecond,
            "Explicit reload restoration rebuilds a field token; a held SDK ComponentRef<Component> must not migrate itself.");
        Program.Check(held.Resolve().TryGet(out migrated) && migrated.Binding!.InstanceId == newSecond, "Explicit Resolve reconnects using the saved address in the current generation.");
        ScriptApiRuntime.Current = null;
        Success(runtime.ReleaseInstance(2, newSecond));
        ulong replacement = Create(runtime, bridge, 2, SECOND, REPLACEMENT_BIRTH);
        Success(runtime.RestoreReload(2, newFirst, snapshot));
        restored = (ComponentRef<Component>)Program.Field(next.Type, "target").Read(next.Instance)!;
        ScriptApiRuntime.Current = Scope(runtime, bridge, 2, newFirst);
        Program.Check(!restored.TryGet(out _) && restored.Resolve().TryGet(out migrated) && migrated.Binding!.InstanceId == replacement,
            "Undo/redo-style recreation with the same saved ID and new birth must not be adopted as reload lineage.");
        ScriptApiRuntime.Current = null;
    }
    static void RenderingReferences()
    {
        Bridge bridge = new(); using ManagedScriptRuntime runtime = new(bridge);
        Success(runtime.LoadCandidate(Program.FixturePath("V1"), 1, true));
        ulong first = Create(runtime, bridge, 1, FIRST, FIRST_BIRTH);
        runtime.TryInstance(1, first, out ManagedScriptRuntime.ScriptInstance? source);
        ScriptApiRuntime.Current = Scope(runtime, bridge, 1, first);
        GameObject owner = new(202, 1, "target-guid");
        ComponentRef<LineRenderer> line = new((LineRenderer)NativeComponentFactory.Create(typeof(LineRenderer), owner, 41)!);
        ComponentRef<MotionWarp> motion = new((MotionWarp)NativeComponentFactory.Create(typeof(MotionWarp), owner, 41)!);
        Program.Field(source!.Type, "line").AssignPrepared(source.Instance, line);
        Program.Field(source.Type, "motionWarp").AssignPrepared(source.Instance, motion);
        Program.Field(source.Type, "model").AssignPrepared(source.Instance, new ModelAssetRef("model-guid"));
        string portable = StructuredFieldCodec.EncodeValue(line, typeof(ComponentRef<LineRenderer>));
        using (JsonDocument address = JsonDocument.Parse(portable)) {
            Program.Check(address.RootElement.GetProperty("$component").GetProperty("componentKind").GetUInt32() == 35
                && !portable.Contains("componentLifetime", StringComparison.Ordinal) && !portable.Contains("objectId", StringComparison.Ordinal),
                "Line settings must store the stable native address and keep runtime births out of saved data.");
        }
        using (ImmutableValueSnapshot spool = ImmutableValueSnapshot.Capture(motion, typeof(ComponentRef<MotionWarp>), new(ResidentBytes: 1))) {
            ComponentRef<MotionWarp> restored = (ComponentRef<MotionWarp>)spool.RestoreValue(typeof(ComponentRef<MotionWarp>))!;
            Program.Check(spool.IsSpooled && ((IComponentReference)restored).Identity == ((IComponentReference)motion).Identity && restored.TryGet(out _),
                "A spooled MotionWarp42 reference retains the exact current row birth.");
        }
        ScriptApiRuntime.Current = null;
        Success(runtime.CaptureReload(1, first, out string snapshot));
        Success(runtime.LoadCandidate(Program.FixturePath("V2"), 2, true));
        ulong next = Create(runtime, bridge, 2, FIRST, FIRST_BIRTH);
        Success(runtime.RestoreReload(2, next, snapshot));
        runtime.TryInstance(2, next, out ManagedScriptRuntime.ScriptInstance? destination);
        ComponentRef<LineRenderer> nextLine = (ComponentRef<LineRenderer>)Program.Field(destination!.Type, "line").Read(destination.Instance)!;
        ComponentRef<MotionWarp> nextMotion = (ComponentRef<MotionWarp>)Program.Field(destination.Type, "motionWarp").Read(destination.Instance)!;
        ModelAssetRef model = (ModelAssetRef)Program.Field(destination.Type, "model").Read(destination.Instance)!;
        ScriptApiRuntime.Current = Scope(runtime, bridge, 2, next);
        Program.Check(line.TryGet(out _) && nextLine.TryGet(out _) && motion.TryGet(out _) && nextMotion.TryGet(out _)
            && model.Reference == "guid:model-guid", "Code reload preserves surviving native rows and a model GUID without resolving their addresses.");
        bridge.Epoch = 42;
        Program.Check(!line.TryGet(out _) && !nextLine.TryGet(out _) && !motion.TryGet(out _) && !nextMotion.TryGet(out _),
            "Native row re-add must expire both retained facades and restored saved fields across code generations.");
        Program.Check(nextLine.Resolve().TryGet(out _) && nextMotion.Resolve().TryGet(out _)
            && !nextLine.TryGet(out _) && !nextMotion.TryGet(out _), "Explicit resolution returns a fresh reference while leaving the expired settings identity unchanged.");
        Program.Check(ManagedFieldSchema.Build(typeof(ModelAssetRef))["assetKind"] as string == "model",
            "The Inspector model picker must receive the exact model asset kind.");
        using (ImmutableValueSnapshot spool = ImmutableValueSnapshot.Capture(model, typeof(ModelAssetRef), new(ResidentBytes: 1)))
            Program.Check(((ModelAssetRef)spool.RestoreValue(typeof(ModelAssetRef))!).Reference == model.Reference,
                "Model GUID settings survive bounded immutable snapshots with their declared type.");
        foreach (Type wrapper in new[] { typeof(ComponentRef<LineRenderer>), typeof(ComponentRef<MotionWarp>) }) {
            using JsonDocument schema = JsonDocument.Parse(JsonSerializer.Serialize(ManagedFieldSchema.Build(wrapper)));
            JsonElement kinds = schema.RootElement.GetProperty("nativeKinds");
            Program.Check(kinds.GetArrayLength() == 1 && kinds[0].GetProperty("type").GetString() == wrapper.GetGenericArguments()[0].FullName,
                "A typed reference picker must expose only its compatible concrete facade.");
        }
        ScriptApiRuntime.Current = null;
    }
    static void Codec()
    {
        ComponentReferenceIdentity identity = new("native", "owner", 1, "", "", 202, 9, 41);
        ComponentRef<RigidBody>[] values = [new(identity), default, new(identity)];
        string json = StructuredFieldCodec.EncodeValue(values, values.GetType());
        Program.Check(!json.Contains("202", StringComparison.Ordinal) && !json.Contains("componentLifetime", StringComparison.Ordinal), "Saved data contains addresses only, never runtime epochs.");
        using JsonDocument document = JsonDocument.Parse(json);
        JsonElement tokens = JsonSerializer.SerializeToElement(SettingsReferenceSnapshot.Capture(values, values.GetType()));
        ComponentRef<RigidBody>[] decoded = (ComponentRef<RigidBody>[])SettingsReferenceSnapshot.Restore(document.RootElement, values.GetType(), tokens)!;
        Program.Check(((IComponentReference)decoded[0]).Identity == identity && decoded[1].IsEmpty && ((IComponentReference)decoded[2]).Identity == identity,
            "Exact tables retain duplicate aliases, order and explicit empty references.");
        using ImmutableValueSnapshot spool = ImmutableValueSnapshot.Capture(values, values.GetType(), new(ResidentBytes: 1));
        decoded = (ComponentRef<RigidBody>[])spool.RestoreValue(values.GetType())!;
        Program.Check(spool.IsSpooled && ((IComponentReference)decoded[2]).Identity == identity, "Spooled snapshots restore the same exact typed identity.");
        Program.Throws<InvalidDataException>(() => SettingsReferenceSnapshot.Restore(document.RootElement, typeof(ComponentRef<Camera>[]), tokens));
        MaterialAssetRef material = new("material-guid"); TextureAssetRef texture = new("texture-guid");
        Program.Check(ManagedFieldSchema.Kind(typeof(MaterialAssetRef)) == "materialAssetRef" && ManagedFieldSchema.Kind(typeof(TextureAssetRef)) == "textureAssetRef"
            && material.Reference == "guid:material-guid" && texture.Reference == "guid:texture-guid", "Actual asset kinds use separate GUID codecs rather than DataAsset metadata.");
        Program.Throws<ArgumentException>(() => new MaterialAssetRef("Assets/material.mat"));
    }
    static ScriptExecutionScope Scope(ManagedScriptRuntime runtime, Bridge bridge, ulong generation, ulong instance)
        => new(bridge, runtime, generation, instance, 101, 1, 1, ScriptCallback.UPDATE);
    static ulong Create(ManagedScriptRuntime runtime, Bridge bridge, ulong generation, string saved, string birth)
    {
        Success(runtime.CreateInstance(generation, TYPE, 101, saved, out ulong instance));
        Success(runtime.BindOwner(generation, instance, 101, 101, 1, "owner-101", birth));
        bridge.Instances[(generation, saved)] = (instance, birth); return instance;
    }
    static void Success(ManagedRuntimeResult result) => Program.Check(result.Success, result.Message);
    sealed class Bridge : Program.RecordingBridge, IScriptNativeBridge
    {
        internal ulong Epoch = 41;
        internal int Resolves;
        internal readonly Dictionary<(ulong Generation, string Saved), (ulong Instance, string Birth)> Instances = new();
        public ulong ReadComponentLifetime(ScriptExecutionScope scope, ulong objectId, uint kind) => Alive.Contains(objectId) ? kind == 0 ? objectId : Epoch : 0;
        public bool HasScriptEntry(ScriptExecutionScope scope, ulong objectId, string savedScriptId) => objectId == 101 && Instances.ContainsKey((scope.Generation, savedScriptId));
        public ComponentReferenceIdentity ResolveComponentReference(ScriptExecutionScope scope, ComponentReferenceIdentity address)
        {
            Resolves++;
            if (address.Kind == "native") return address with { ObjectId = Epoch == 0 ? 0UL : 202UL, SceneLifetime = Epoch == 0 ? 0UL : 1UL, ComponentLifetime = Epoch };
            if (!Instances.TryGetValue((scope.Generation, address.SavedScriptId), out var target)) return address with { ObjectId = 0, SceneLifetime = 0 };
            return address with { ObjectId = 101, SceneLifetime = 1, ScriptInstanceId = target.Instance, Generation = scope.Generation, ScriptBirth = target.Birth };
        }
    }
}
