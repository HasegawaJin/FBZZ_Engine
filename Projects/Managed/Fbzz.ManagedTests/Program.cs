/// @file    Program.cs
/// @brief   実 Host の反射・配送・世代棄却・collectible 解放を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

static class Program
{
    static int Main()
    {
        (string Name, Action Test)[] tests =
        [
            ("PublicAndPrivateSettings", PublicAndPrivateSettings),
            ("OverriddenLifecycleAndScope", OverriddenLifecycleAndScope),
            ("InheritedLifecycleAndConstructor", InheritedLifecycleAndConstructor),
            ("CatalogRejectsInvalidContracts", CatalogRejectsInvalidContracts),
            ("FaultClearsScopeAndStopsEntry", FaultClearsScopeAndStopsEntry),
            ("CandidateFailureKeepsOldGeneration", CandidateFailureKeepsOldGeneration),
            ("ThreadAndGenerationChecks", ThreadAndGenerationChecks),
            ("MutatingAbiDoesNotRequestReplay", MutatingAbiDoesNotRequestReplay),
            ("RetiredContextCanBeCollected", RetiredContextCanBeCollected),
            ("AutomaticCollectionAllowsRapidReload", AutomaticCollectionAllowsRapidReload),
            ("PinnedContextsEnforceLimit", PinnedContextsEnforceLimit),
            ("LiveCandidateCapacityDoesNotRequestCollection", LiveCandidateCapacityDoesNotRequestCollection),
            .. Stage2Tests.Cases,
            .. CollisionTests.Cases,
            .. LifecyclePhysicsBridgeTests.Cases,
            .. ObjectControlsBridgeTests.Cases,
            .. ApiCatalogTests.Cases,
            .. AuthoringReferenceTests.Cases,
            .. InspectorSchemaTests.Cases,
            .. PrimitiveCodecTests.Cases,
            .. ObservationTests.Cases,
            .. EditorFeatureTests.Cases,
            .. GizmoMetadataAbiTests.Cases,
            .. GenerationOwnershipTests.Cases,
            .. ComponentReferenceTests.Cases,
            .. DataAssetDocumentTests.Cases,
            .. SettingsTransactionTests.Cases,
            .. SettingsCapacityTests.Cases,
            .. SettingsPageAndUploadTests.Cases,
            .. InputBridgeTests.Cases,
            .. AssetLeaseTests.Cases,
            .. RuntimeTextureTests.Cases,
            .. RuntimeTextureBridgeTests.Cases,
            .. ApiContractMetadataTests.Cases,
            .. SdkDocumentationTests.Cases,
            .. ScriptCallbackContractsTests.Cases,
            .. AudioSourceApiTests.Cases,
            .. ParticleServicesApiTests.Cases,
            .. RagdollControlsApiTests.Cases,
            .. FootIKApiTests.Cases
        ];
        int failures = 0;
        foreach ((string name, Action test) in tests)
        {
            try { test(); Console.WriteLine($"[PASS] {name}"); }
            catch (Exception exception) { failures++; Console.WriteLine($"[FAIL] {name}: {exception}"); }
            finally { ScriptApiRuntime.Current = null; }
        }
        Console.WriteLine($"MANAGED TESTS passed={tests.Length - failures} failed={failures}");
        return failures == 0 ? 0 : 1;
    }

    internal static string FixturePath(string version) => Path.Combine(AppContext.BaseDirectory, "Fixtures", version, "Fbzz.ManagedFixtures.dll");

    static void PublicAndPrivateSettings()
    {
        using HostFixture fixture = new();
        ScriptTypeDescriptor type = fixture.Type("ManagedFixtures.SettingsComponent");
        Script instance = type.CreateInstance();
        Check(Field(type, "speed").IsSetting, "Public mutable fields must be settings.");
        Check(Field(type, "privateSpeed").IsSetting, "Setting must include private fields.");
        Check(!Field(type, "runtimeElapsed").IsSetting, "Omitted modifiers remain private runtime state.");
        Check(Field(type, "runtimeElapsed").IsInspectable, "Private primitive state must remain inspectable.");
        Check(!Field(type, "transient").IsSetting, "NonSerialized public state must not be saved.");
        Check(!Field(type, "readOnly").IsSetting, "Readonly fields cannot be edited settings.");
        Check(!type.Fields.Any((ScriptFieldDescriptor field) => field.Name == "sharedState"), "Static state must not be an instance field.");
        ScriptFieldDescriptor speed = Field(type, "speed");
        Check(speed.TryWrite(instance, 6f) && Equals(speed.Read(instance), 6f), "Public setting writes must preserve their declared type.");
        Check(!speed.TryWrite(instance, float.NaN) && Equals(speed.Read(instance), 6f), "Invalid writes must leave the value unchanged.");
        Check(!speed.TryWrite(instance, 6), "An int must not silently coerce into a float setting.");
        Check(!Field(type, "runtimeElapsed").TryWrite(instance, 9f), "Debug fields cannot use the settings write path.");
        ScriptFieldDescriptor privateSpeed = Field(type, "privateSpeed");
        Check(privateSpeed.Key == "stableSpeed" && privateSpeed.PreviousKeys.Contains("oldSpeed"), "Declared save key migration must survive the catalog.");
        Check(privateSpeed.Metadata?.Unit == "m/s", "Schema must preserve optional setting metadata.");
        Check(!privateSpeed.Visible && privateSpeed.Range is ValueRangeAttribute hiddenRange && Convert.ToDouble(hiddenRange.Maximum) == 10, "Hidden metadata and range must remain available to schema consumers.");
        Check(privateSpeed.TryWrite(instance, 5f) && Equals(privateSpeed.Read(instance), 5f), "Private settings must be writable through the host schema.");
        Check(!privateSpeed.TryWrite(instance, 11f) && Equals(privateSpeed.Read(instance), 5f), "Range failures must preserve the previous setting.");
        Check(!type.Fields.Any((ScriptFieldDescriptor field) => field.Name == "GetterTrap"), "Catalog must exclude properties without executing their getter.");
    }

    static void OverriddenLifecycleAndScope()
    {
        using HostFixture fixture = new();
        ulong id = fixture.Create("ManagedFixtures.LifecycleComponent");
        fixture.Invoke(id, ScriptCallback.AWAKE);
        fixture.Invoke(id, ScriptCallback.START);
        fixture.Invoke(id, ScriptCallback.UPDATE);
        fixture.Invoke(id, ScriptCallback.UPDATE);
        ScriptTypeDescriptor type = fixture.Type("ManagedFixtures.LifecycleComponent");
        Script instance = fixture.Instance(id);
        Check(Equals(Field(type, "awakeCount").Read(instance), 1), "An Awake override must execute once per explicit host dispatch.");
        Check(Equals(Field(type, "startCount").Read(instance), 1), "A Start override must execute.");
        Check(Equals(Field(type, "updateCount").Read(instance), 2), "An Update override must execute without manual registration.");
        Check(fixture.Bridge.Position.Equals(new Vector3(2f, 0f, 0f)), "Two fixed 0.25 second updates at 4 m/s must move 2 metres.");
        Check(fixture.Bridge.LastGeneration == 1 && fixture.Bridge.LastOwner == 101, "Engine calls must carry the current caller generation and owner.");
        Check(ScriptApiRuntime.Current is null, "Host scope must end with the callback.");
        Throws<InvalidOperationException>(() => { _ = instance.Transform.WorldPosition; });
        fixture.Invoke(id, ScriptCallback.ON_DESTROY);
        Check(Equals(Field(type, "destroyCount").Read(instance), 1), "Cleanup callbacks must use the same discovered lifecycle path.");
    }

    static void InheritedLifecycleAndConstructor()
    {
        using HostFixture fixture = new();
        ScriptTypeDescriptor derived = fixture.Type("ManagedFixtures.InheritedCallbackComponent");
        Script instance = derived.CreateInstance();
        derived.Callbacks.Single((ScriptCallbackDescriptor callback) => callback.Callback == ScriptCallback.UPDATE).CreateInvocation(instance)();
        Check(Equals(Field(derived, "derivedCalls").Read(instance), 1), "Most-derived callback must be selected.");
        Check(Equals(Field(derived, "baseCalls").Read(instance), 0), "Hidden base callback must not be dispatched a second time.");
        Check(Field(derived, "baseSpeed").IsSetting, "Private inherited settings must be included.");
        ScriptTypeDescriptor inherited = fixture.Type("ManagedFixtures.InheritedOnlyCallbackComponent");
        Script inheritedInstance = inherited.CreateInstance();
        inherited.Callbacks.Single((ScriptCallbackDescriptor callback) => callback.Callback == ScriptCallback.UPDATE).CreateInvocation(inheritedInstance)();
        Check(Equals(Field(inherited, "baseCalls").Read(inheritedInstance), 1), "A base lifecycle override must be inherited by Host discovery.");
        ScriptTypeDescriptor constructor = fixture.Type("ManagedFixtures.PrivateConstructorComponent");
        FieldInfo count = constructor.Type.GetField("constructorCalls", BindingFlags.Static | BindingFlags.Public)!;
        count.SetValue(null, 0);
        CatalogBuildResult catalog = ScriptCatalogBuilder.Build(constructor.Type.Assembly);
        Check(catalog.Success && Equals(count.GetValue(null), 0), "Type discovery must not run user constructors.");
        Script constructed = constructor.CreateInstance();
        Check(Equals(count.GetValue(null), 1) && Equals(Field(constructor, "value").Read(constructed), 7), "A non-public parameterless constructor must work.");
    }

    static void CatalogRejectsInvalidContracts()
    {
        ModuleBuilder module = AssemblyBuilder.DefineDynamicAssembly(new AssemblyName("InvalidCatalogFixture"), AssemblyBuilderAccess.RunAndCollect).DefineDynamicModule("main");
        TypeBuilder type = module.DefineType("Arbitrary.Namespace.InvalidComponent", TypeAttributes.Public, typeof(Script));
        type.DefineDefaultConstructor(MethodAttributes.Public);
        MethodBuilder update = type.DefineMethod("Update", MethodAttributes.Private, typeof(int), Type.EmptyTypes);
        ILGenerator updateBody = update.GetILGenerator();
        updateBody.Emit(OpCodes.Ldc_I4_0); updateBody.Emit(OpCodes.Ret);
        FieldBuilder invalidSetting = type.DefineField("unsupported", typeof(object), FieldAttributes.Private);
        invalidSetting.SetCustomAttribute(new CustomAttributeBuilder(typeof(SettingAttribute).GetConstructor(Type.EmptyTypes)!, []));
        ConstructorInfo keyConstructor = typeof(FieldKeyAttribute).GetConstructor([typeof(string)])!;
        FieldBuilder first = type.DefineField("first", typeof(float), FieldAttributes.Public);
        FieldBuilder second = type.DefineField("second", typeof(float), FieldAttributes.Public);
        first.SetCustomAttribute(new CustomAttributeBuilder(keyConstructor, ["duplicate"]));
        second.SetCustomAttribute(new CustomAttributeBuilder(keyConstructor, ["duplicate"]));
        FieldBuilder nullKey = type.DefineField("nulKey", typeof(float), FieldAttributes.Public);
        nullKey.SetCustomAttribute(new CustomAttributeBuilder(keyConstructor, ["prefix\0suffix"]));
        FieldBuilder nullPrevious = type.DefineField("nulPrevious", typeof(float), FieldAttributes.Public);
        nullPrevious.SetCustomAttribute(new CustomAttributeBuilder(typeof(PreviousFieldKeyAttribute).GetConstructor([typeof(string)])!, ["old\0suffix"]));
        Type emitted = type.CreateType()!;
        CatalogBuildResult result = ScriptCatalogBuilder.Build(emitted.Assembly);
        Check(!result.Success && result.Catalog is null, "A partial invalid catalog must not become a candidate.");
        Check(result.Diagnostics.Any((ScriptDiagnostic diagnostic) => diagnostic.Code == "CS_LIFECYCLE_SIGNATURE"), "Lifecycle signature errors must name the contract.");
        Check(result.Diagnostics.Any((ScriptDiagnostic diagnostic) => diagnostic.Code == "CS_FIELD_UNSUPPORTED"), "Unsupported explicit settings must be rejected.");
        Check(result.Diagnostics.Any((ScriptDiagnostic diagnostic) => diagnostic.Code == "CS_FIELD_KEY"), "Duplicate save keys must be rejected.");
        Check(result.Diagnostics.Any((ScriptDiagnostic diagnostic) => diagnostic.Code == "CS_FIELD_KEY" && diagnostic.MemberName == "nulKey")
            && result.Diagnostics.Any((ScriptDiagnostic diagnostic) => diagnostic.Code == "CS_FIELD_KEY" && diagnostic.MemberName == "nulPrevious"),
            "NUL in a current or previous saved field key must fail before the binary separator protocol is used.");
    }

    static void FaultClearsScopeAndStopsEntry()
    {
        using HostFixture fixture = new();
        ulong id = fixture.Create("ManagedFixtures.ThrowingCallbackComponent");
        ManagedRuntimeResult first = fixture.Runtime.TryInvoke(1, id, 101, 1, ScriptCallback.UPDATE);
        Check(!first.Success && first.Message.Contains("Intentional callback failure.", StringComparison.Ordinal), "Callback exceptions must become an actionable result.");
        Check(ScriptApiRuntime.Current is null, "A throwing callback must not leave API scope enabled.");
        ManagedRuntimeResult second = fixture.Runtime.TryInvoke(1, id, 101, 2, ScriptCallback.UPDATE);
        Check(!second.Success, "A faulted entry must stop ordinary callbacks.");
        ScriptTypeDescriptor type = fixture.Type("ManagedFixtures.ThrowingCallbackComponent");
        Check(Equals(Field(type, "updateCount").Read(fixture.Instance(id)), 1), "The faulted Update must not execute again.");
        Check(fixture.Runtime.TryInvoke(1, id, 101, 3, ScriptCallback.ON_DESTROY).Success, "Fault cleanup must remain allowed.");
    }

    static void CandidateFailureKeepsOldGeneration()
    {
        using HostFixture fixture = new();
        ulong id = fixture.Create("ManagedFixtures.LifecycleComponent");
        ManagedRuntimeResult discovered = LoadFixture(fixture.Runtime, FixturePath("Invalid"), 2);
        Check(discovered.Success && fixture.Runtime.GetCatalog(2) is not null, "Passive discovery must not execute even an Engine-using constructor.");
        ManagedRuntimeResult failed = fixture.Runtime.CreateInstance(2, "ManagedFixtures.LifecycleComponent", 101, out ulong rejected);
        Check(!failed.Success && rejected == 0, "Explicit creation must reject constructor Engine access without changing the existing generation.");
        Check(fixture.Runtime.RetireGeneration(2).Success, "The inert discovered catalog can be retired independently.");
        Check(fixture.Runtime.GetCatalog(1) is not null, "A bad candidate must keep the live catalog.");
        fixture.Invoke(id, ScriptCallback.UPDATE);
        Check(fixture.Bridge.Position.Equals(new Vector3(1f, 0f, 0f)), "The old generation must still dispatch after candidate rejection.");
        Check(LoadFixture(fixture.Runtime, FixturePath("V2"), 3).Success, "An independent corrected candidate must prepare successfully.");
        ScriptTypeDescriptor next = fixture.Runtime.GetCatalog(3)!.Find("ManagedFixtures.LifecycleComponent")!;
        Check(Equals(Field(next, "speed").Read(next.CreateInstance()), 8f), "V2 must retain the same full type name with changed defaults.");
    }

    static void ThreadAndGenerationChecks()
    {
        using HostFixture fixture = new();
        ulong id = fixture.Create("ManagedFixtures.LifecycleComponent");
        Check(!fixture.Runtime.TryInvoke(1, id, 101, 0, ScriptCallback.UPDATE).Success, "A zero callback token must be rejected.");
        Check(!fixture.Runtime.TryInvoke(1, id, 999, 1, ScriptCallback.UPDATE).Success, "An owner mismatch must be rejected.");
        bool success = true;
        Thread thread = new(() => { success = fixture.Runtime.TryInvoke(1, id, 101, 1, ScriptCallback.UPDATE).Success; });
        thread.Start(); thread.Join();
        Check(!success && fixture.Bridge.Position.Equals(Vector3.ZERO), "Cross-thread callbacks must be rejected before native calls.");
        Check(fixture.Runtime.ReleaseInstance(1, id).Success, "Valid instance release must succeed.");
        Check(!fixture.Runtime.TryInvoke(1, id, 101, 1, ScriptCallback.UPDATE).Success, "Released instances must reject stale dispatch.");
    }

    static void RetiredContextCanBeCollected()
    {
        using ManagedScriptRuntime runtime = new(new RecordingBridge());
        WeakReference context = LoadAndRetire(runtime, 1);
        /// @note collectible 解放は協調的なので、実行時間を待たず有限回の GC / finalizer cycle で確認する。
        /// @see https://learn.microsoft.com/dotnet/standard/assembly/unloadability unload の検証。
        for (int attempt = 0; attempt < 10 && context.IsAlive; attempt++)
        {
            GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
        }
        runtime.CollectRetired();
        Check(!context.IsAlive && runtime.PendingUnloadCount == 0,
            $"Host-held catalog and delegates must not pin a retired context (alive={context.IsAlive}, pending={runtime.PendingUnloadCount}).");
    }

    static unsafe void MutatingAbiDoesNotRequestReplay()
    {
        BridgeRequest initialize = new()
        {
            AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.INITIALIZE, Generation = BridgeAbi.SCHEMA_HASH,
            Index = (uint)sizeof(BridgeRequest), Flags = (uint)sizeof(BridgeReply),
            Integer = (long)(nint)(delegate* unmanaged[Cdecl]<BridgeRequest*, BridgeReply*, int>)&UnusedNativeDispatch
        };
        Check(AbiDispatch(initialize).Status == BridgeStatus.OK, "Real managed ABI must initialize.");
        try
        {
            BridgeRequest load = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.LOAD_CANDIDATE, Generation = 500, Flags = BridgeAbi.INTERNAL_MODE };
            Check(AbiDispatch(load, FixturePath("V1")).Status == BridgeStatus.OK, "Real managed ABI must load the fixture.");
            BridgeRequest create = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.CREATE, Generation = 500, OwnerId = 101 };
            BridgeReply created = AbiDispatch(create, "ManagedFixtures.ThrowingCallbackComponent\n11111111-1111-4111-8111-111111111111");
            Check(created.Status == BridgeStatus.OK && created.InstanceId != 0, "Real managed ABI must create the throwing entry.");
            BridgeRequest bind = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.BIND_OWNER, Generation = 500, InstanceId = created.InstanceId, OwnerId = 101, Integer = 101, CallId = 1 };
            Check(AbiDispatch(bind, "owner-101\n22222222-2222-4222-8222-222222222222").Status == BridgeStatus.OK, "The real ABI must bind the scene owner before dispatch.");
            BridgeRequest invoke = new()
            {
                AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.INVOKE, Generation = 500,
                InstanceId = created.InstanceId, OwnerId = 101, CallId = 1, Index = (uint)ScriptCallback.UPDATE
            };
            BridgeReply failed = AbiDispatch(invoke);
            Check(failed.Status == BridgeStatus.CALLBACK_FAILED && failed.OutputBytes == 0,
                "Zero diagnostic capacity must preserve callback failure instead of requesting replay.");
            BridgeRequest read = new()
            {
                AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.READ_FIELD,
                Generation = 500, InstanceId = created.InstanceId
            };
            BridgeReply count = AbiDispatch(read, "updateCount");
            Check(count.Status == BridgeStatus.OK && count.Integer == 1, "The mutating callback must execute exactly once.");
            BridgeRequest getType = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.GET_TYPE, Generation = 500 };
            Check(AbiDispatch(getType).Status == BridgeStatus.BUFFER_TOO_SMALL, "Readonly payload discovery may request a larger buffer.");
            load.Generation = 501;
            BridgeReply rejected = AbiDispatch(load, FixturePath("Invalid") + ".missing");
            Check(rejected.Status == BridgeStatus.LOAD_FAILED && rejected.OutputBytes == 0,
                "Failed candidate preparation must preserve its failure status with zero diagnostic capacity.");
            Check(AbiDispatch(load, FixturePath("Invalid")).Status == BridgeStatus.OK,
                "Passive discovery must not execute the fixture's invalid constructor.");
            create.Generation = 501;
            BridgeReply invalidConstruction = AbiDispatch(create, "ManagedFixtures.LifecycleComponent\n22222222-2222-4222-8222-222222222222");
            Check(invalidConstruction.Status == BridgeStatus.LOAD_FAILED && invalidConstruction.InstanceId == 0 && invalidConstruction.OutputBytes == 0,
                "Failed explicit construction must retain its failure status with zero diagnostic capacity, without requesting mutation replay.");
        }
        finally
        {
            BridgeRequest shutdown = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.SHUTDOWN };
            Check(AbiDispatch(shutdown).Status == BridgeStatus.OK, "Real managed ABI must release its session.");
        }
    }

    static unsafe BridgeReply AbiDispatch(BridgeRequest request, string input = "")
    {
        byte[] bytes = Encoding.UTF8.GetBytes(input);
        fixed (byte* pointer = bytes)
        {
            request.Input = pointer; request.InputBytes = (uint)bytes.Length;
            BridgeReply reply = default;
            int status = EntryPoint.DispatchManaged(ref request, ref reply);
            Check(status == (int)reply.Status, "ABI return code and reply status must match.");
            return reply;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new Type[] { typeof(CallConvCdecl) })]
    static unsafe int UnusedNativeDispatch(BridgeRequest* request, BridgeReply* reply)
    {
        if (reply is not null) reply->Status = BridgeStatus.UNSUPPORTED;
        return (int)BridgeStatus.UNSUPPORTED;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static WeakReference LoadAndRetire(ManagedScriptRuntime runtime, ulong generation, bool allowCollectionRetry = false)
    {
        ManagedRuntimeResult loaded = LoadFixture(runtime, FixturePath("V1"), generation);
        for (int retry = 0; allowCollectionRetry && loaded.Status == BridgeStatus.COLLECTION_PENDING && retry < 10; retry++)
        {
            /// @note Background GC と finalizer は別スレッドなので、時刻や sleep を使わず有限回だけ実行権を譲る。
            for (int poll = 0; poll < 16384 && runtime.PendingUnloadCount >= 3; poll++)
            {
                Thread.Yield();
                runtime.CollectRetired();
            }
            loaded = LoadFixture(runtime, FixturePath("V1"), generation);
        }
        Check(loaded.Success, $"Unload fixture must load (generation={generation}, status={loaded.Status}, pending={runtime.PendingUnloadCount}).");
        ScriptTypeDescriptor type = runtime.GetCatalog(generation)!.Types[0];
        Type nestedSettings = type.Type.Assembly.GetType("ManagedFixtures.CompletionSettings", throwOnError: true)!;
        _ = ManagedFieldSchema.Build(nestedSettings);
        _ = ManagedFieldSchema.Build(nestedSettings);
        AssemblyLoadContext context = AssemblyLoadContext.GetLoadContext(type.Type.Assembly)!;
        WeakReference weak = new(context, trackResurrection: true);
        Check(runtime.CreateInstance(generation, "ManagedFixtures.LifecycleComponent", 101, out ulong id).Success, "Unload fixture must bind callbacks.");
        BindFixture(runtime, generation, id);
        Check(runtime.TryInvoke(generation, id, 101, 1, ScriptCallback.UPDATE).Success, "Unload fixture must create executable delegates.");
        Check(runtime.RetireGeneration(generation).Success, "Unload fixture must retire.");
        return weak;
    }

    static void AutomaticCollectionAllowsRapidReload()
    {
        using ManagedScriptRuntime runtime = new(new RecordingBridge());
        List<ScriptCatalog> pins = new();
        for (ulong generation = 1; generation <= 3; generation++)
            pins.Add(PinAndRetire(runtime, generation));
        Check(runtime.PendingUnloadCount == 3, "The rapid reload fixture must start with three real retired contexts.");
        Check(LoadFixture(runtime, FixturePath("V1"), 4).Status == BridgeStatus.COLLECTION_PENDING,
            "Temporary pins must defer the candidate while requesting real background collection.");
        Check(runtime.CollectionRequestTotal == 1 && runtime.GetCatalog(4) is null,
            "Collection preparation must issue one request without starting the candidate.");
        pins.Clear();
        for (ulong generation = 4; generation <= 15; generation++)
            LoadAndRetire(runtime, generation, allowCollectionRetry: true);
        Check(runtime.CollectionRequestTotal > 0, "Repeated reload must use the actual Host background collection path.");
        Check(runtime.PendingUnloadCount <= 3, "Retired-context storage must remain within the reload limit.");
        GC.KeepAlive(pins);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static ScriptCatalog PinAndRetire(ManagedScriptRuntime runtime, ulong generation)
    {
        Check(LoadFixture(runtime, FixturePath("V1"), generation).Success, "A free slot must accept a candidate.");
        ScriptCatalog pin = runtime.GetCatalog(generation)!;
        Check(runtime.RetireGeneration(generation).Success, "Pinned fixture must retire.");
        return pin;
    }

    static void PinnedContextsEnforceLimit()
    {
        RecordingBridge bridge = new();
        using ManagedScriptRuntime runtime = new(bridge);
        Check(LoadFixture(runtime, FixturePath("V1"), 50).Success, "The live generation must load.");
        Check(runtime.CreateInstance(50, "ManagedFixtures.LifecycleComponent", 101, out ulong liveId).Success, "The live instance must bind.");
        BindFixture(runtime, 50, liveId);
        List<ScriptCatalog> pins = new();
        for (ulong generation = 1; generation <= 3; generation++)
            pins.Add(PinAndRetire(runtime, generation));
        runtime.CollectRetired();
        Check(runtime.PendingUnloadCount == 3, "Real type references must pin all three retired contexts.");
        Check(LoadFixture(runtime, FixturePath("V2"), 0).Status == BridgeStatus.INVALID_ARGUMENT, "Invalid generations must fail before collection.");
        Check(LoadFixture(runtime, FixturePath("V2"), 50).Status == BridgeStatus.INVALID_ARGUMENT, "Duplicate generations must fail before collection.");
        Check(LoadFixture(runtime, "RelativeFixture.dll", 100).Status == BridgeStatus.INVALID_ARGUMENT, "Relative paths must fail before collection.");
        Check(LoadFixture(runtime, Path.Combine(AppContext.BaseDirectory, "MissingFixture.dll"), 100).Status == BridgeStatus.LOAD_FAILED, "Missing assemblies must fail before collection.");
        bridge.DuringTranslate = () =>
        {
            Check(LoadFixture(runtime, FixturePath("V2"), 100).Status == BridgeStatus.INVALID_SCOPE, "Callbacks cannot initiate preparation or collection.");
            Check(runtime.CollectionRequestTotal == 0, "A rejected callback preparation must not request GC.");
        };
        Check(runtime.TryInvoke(50, liveId, 101, 1, ScriptCallback.UPDATE).Success, "The old generation must still invoke while contexts are pinned.");
        bridge.DuringTranslate = null;
        Check(runtime.CollectionRequestTotal == 0, "Validation, callbacks, and weak monitoring must not request collection.");
        for (int request = 1; request <= 10; request++)
        {
            ulong generation = (ulong)(100 + request);
            ManagedRuntimeResult pending = LoadFixture(runtime, FixturePath(request % 2 == 0 ? "V1" : "V2"), generation);
            Check(pending.Status == BridgeStatus.COLLECTION_PENDING, "Each accepted collection request must defer the unstarted candidate.");
            Check(runtime.CollectionRequestCount == request && runtime.CollectionRequestTotal == (ulong)request, "Changing candidate names must not reset the shared collection budget.");
            Check(runtime.GetCatalog(generation) is null && runtime.GetCatalog(50) is not null, "Pending collection must preserve the live catalog without preparing a new one.");
            Check(runtime.TryInvoke(50, liveId, 101, (ulong)(request + 1), ScriptCallback.UPDATE).Success, "Pending collection must preserve the live instance and its delegates.");
        }
        ManagedRuntimeResult limited = LoadFixture(runtime, FixturePath("V2"), 111);
        Check(limited.Status == BridgeStatus.PENDING_LIMIT && runtime.CollectionRequestTotal == 10, "Pinned contexts must refuse further candidates after the finite collection budget.");
        Check(runtime.PendingUnloadCount == 3 && runtime.GetCatalog(111) is null, "Limit rejection must not change retained contexts or create a candidate.");
        GC.KeepAlive(pins);
    }

    static void LiveCandidateCapacityDoesNotRequestCollection()
    {
        using ManagedScriptRuntime runtime = new(new RecordingBridge());
        Check(LoadFixture(runtime, FixturePath("V1"), 1).Success, "The first live generation must load.");
        Check(LoadFixture(runtime, FixturePath("V2"), 2).Success, "The second live generation must load.");
        Check(LoadFixture(runtime, FixturePath("V1"), 3).Status == BridgeStatus.PENDING_LIMIT, "Two live candidates must reject a third before collection.");
        Check(runtime.CollectionRequestTotal == 0 && runtime.GetCatalog(1) is not null && runtime.GetCatalog(2) is not null,
            "Live-generation capacity rejection must preserve both catalogs without requesting GC.");
    }

    static ManagedRuntimeResult LoadFixture(ManagedScriptRuntime runtime, string path, ulong generation) => runtime.LoadCandidate(path, generation, internalMode: true);
    internal static void BindFixture(ManagedScriptRuntime runtime, ulong generation, ulong id, ulong owner = 101, ulong objectId = 101, string guid = "owner-101", ulong scene = 1)
        => Check(runtime.BindOwner(generation, id, owner, objectId, scene, guid).Success, "Fixture owner must bind to a live scene.");
    internal static ScriptFieldDescriptor Field(ScriptTypeDescriptor type, string name) => type.Fields.Single((ScriptFieldDescriptor field) => field.Name == name);
    internal static void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
    internal static void Throws<T>(Action action) where T : Exception
    {
        try { action(); }
        catch (T) { return; }
        throw new InvalidOperationException($"Expected {typeof(T).Name}.");
    }

    sealed class HostFixture : IDisposable
    {
        internal HostFixture()
        {
            Runtime = new(Bridge);
            ManagedRuntimeResult result = LoadFixture(Runtime, FixturePath("V1"), 1);
            Check(result.Success, result.Message);
        }
        internal RecordingBridge Bridge { get; } = new();
        internal ManagedScriptRuntime Runtime { get; }
        internal ScriptTypeDescriptor Type(string name) => Runtime.GetCatalog(1)!.Find(name) ?? throw new InvalidOperationException(name);
        internal ulong Create(string name)
        {
            ManagedRuntimeResult result = Runtime.CreateInstance(1, name, 101, out ulong id);
            Check(result.Success, result.Message);
            BindFixture(Runtime, 1, id);
            return id;
        }
        internal Script Instance(ulong id)
        {
            Check(Runtime.TryInstance(1, id, out ManagedScriptRuntime.ScriptInstance? instance), "Bound instance must be present.");
            return instance!.Instance;
        }
        internal void Invoke(ulong id, ScriptCallback callback)
        {
            ManagedRuntimeResult result = Runtime.TryInvoke(1, id, 101, 1, callback);
            Check(result.Success, result.Message);
        }
        public void Dispose() { Runtime.Dispose(); }
    }

    /// @note Native bridge だけを固定値へ置き換え、catalog・generation・instance・callback は本物の Host を使う。
    internal class RecordingBridge : IScriptNativeBridge
    {
        internal readonly Dictionary<ulong, Vector3> Positions = new();
        internal readonly HashSet<ulong> Alive = [101, 202, 203];
        internal readonly Dictionary<string, ulong> Guids = new(StringComparer.Ordinal)
        { ["owner-101"] = 101, ["target-guid"] = 202, ["child-guid"] = 203 };
        internal readonly Dictionary<ulong, ulong[]> Components = new();
        internal Vector3 Position { get => Positions.GetValueOrDefault(101UL); set => Positions[101] = value; }
        internal ulong LastGeneration;
        internal ulong LastOwner;
        internal ulong LastObject;
        internal int ResolveCount;
        internal Action? DuringTranslate;
        void Record(ScriptExecutionScope scope, ulong objectId)
        { LastGeneration = scope.Generation; LastOwner = scope.OwnerId; LastObject = objectId; }
        void RequireTarget(ulong objectId)
        {
            if (!Alive.Contains(objectId)) throw new InvalidOperationException("Native target identity has expired.");
        }
        public bool IsAlive(ScriptExecutionScope scope, ulong objectId) { Record(scope, objectId); return Alive.Contains(objectId); }
        public GameObject? CreateObject(ScriptExecutionScope scope, string name, Vector3 position, Quaternion rotation, bool usePose) => throw new NotSupportedException();
        public GameObject? InstantiateObject(ScriptExecutionScope scope, string prefab, Vector3 position, Quaternion rotation, bool usePose) => throw new NotSupportedException();
        public bool CanCreate(ScriptExecutionScope scope, int count) => throw new NotSupportedException();
        public bool DestroyObject(ScriptExecutionScope scope, ulong objectId, float delay) => throw new NotSupportedException();
        public BatchResult DestroyObjects(ScriptExecutionScope scope, ReadOnlySpan<GameObject?> objects, float delay) => throw new NotSupportedException();
        public Vector3 ReadPhysicsVector(ScriptExecutionScope scope, ulong objectId, uint field) => throw new NotSupportedException();
        public float ReadPhysicsScalar(ScriptExecutionScope scope, ulong objectId, uint field) => throw new NotSupportedException();
        public bool ReadPhysicsFlag(ScriptExecutionScope scope, ulong objectId, uint field) => throw new NotSupportedException();
        public void WritePhysicsVector(ScriptExecutionScope scope, ulong objectId, uint field, Vector3 value) => throw new NotSupportedException();
        public void WritePhysicsScalar(ScriptExecutionScope scope, ulong objectId, uint field, float value) => throw new NotSupportedException();
        public void WritePhysicsFlag(ScriptExecutionScope scope, ulong objectId, uint field, bool value) => throw new NotSupportedException();
        public void PhysicsBodyCommand(ScriptExecutionScope scope, ulong objectId, uint command, Vector3 value, Vector3 point, uint axes) => throw new NotSupportedException();
        public Vector3 ReadWorldGravity(ScriptExecutionScope scope) => throw new NotSupportedException();
        public bool LayersCollide(ScriptExecutionScope scope, int a, int b) => throw new NotSupportedException();
        public bool Raycast(ScriptExecutionScope scope, Vector3 origin, Vector3 direction, float maxDistance, out RaycastHit hit) => throw new NotSupportedException();
        public bool SphereCast(ScriptExecutionScope scope, Vector3 center, float radius, Vector3 direction, float maxDistance, out RaycastHit hit) => throw new NotSupportedException();
        public RaycastHit[] RaycastAll(ScriptExecutionScope scope, Vector3 origin, Vector3 direction, float maxDistance) => throw new NotSupportedException();
        public GameObject[] OverlapSphere(ScriptExecutionScope scope, Vector3 center, float radius) => throw new NotSupportedException();
        public Quaternion ReadRotation(ScriptExecutionScope scope, ulong objectId, bool world) => throw new NotSupportedException();
        public void WriteLocalRotation(ScriptExecutionScope scope, ulong objectId, Quaternion value) => throw new NotSupportedException();
        public Vector3 ReadLocalScale(ScriptExecutionScope scope, ulong objectId) => throw new NotSupportedException();
        public void WriteLocalScale(ScriptExecutionScope scope, ulong objectId, Vector3 value) => throw new NotSupportedException();
        public Vector3 ReadDirection(ScriptExecutionScope scope, ulong objectId, uint direction) => throw new NotSupportedException();
        public void Rotate(ScriptExecutionScope scope, ulong objectId, Vector3 axis, float degrees) => throw new NotSupportedException();
        public void LookAt(ScriptExecutionScope scope, ulong objectId, Vector3 worldTarget) => throw new NotSupportedException();
        public Vector3 ReadPosition(ScriptExecutionScope scope, ulong objectId, bool world)
        { Record(scope, objectId); RequireTarget(objectId); return Positions.GetValueOrDefault(objectId); }
        public void WritePosition(ScriptExecutionScope scope, ulong objectId, Vector3 value, bool world)
        { Record(scope, objectId); RequireTarget(objectId); Positions[objectId] = value; }
        public void TranslateLocal(ScriptExecutionScope scope, ulong objectId, Vector3 delta)
        { Record(scope, objectId); RequireTarget(objectId); DuringTranslate?.Invoke(); Positions[objectId] = Positions.GetValueOrDefault(objectId) + delta; }
        public bool ReadKey(ScriptExecutionScope scope, KeyCode key, uint state) { Record(scope, scope.OwnerId); return key == KeyCode.W && state == 0; }
        public float ReadAction(ScriptExecutionScope scope, string action, uint state) { Record(scope, scope.OwnerId); return 0f; }
        public float DeltaSeconds(ScriptExecutionScope scope) { Record(scope, scope.OwnerId); return 0.25f; }
        public ulong ResolveGameObject(ulong generation, ulong instanceId, ulong ownerId, string guid)
        { ResolveCount++; return Guids.TryGetValue(guid, out ulong id) && Alive.Contains(id) ? id : 0; }
        public ulong[] GetManagedComponents(ScriptExecutionScope scope, ulong objectId)
        { Record(scope, objectId); return Alive.Contains(objectId) && Components.TryGetValue(objectId, out ulong[]? ids) ? ids : []; }
        public string GetObjectName(ScriptExecutionScope scope, ulong objectId)
        {
            Record(scope, objectId); RequireTarget(objectId);
            return objectId switch { 101 => "Owner", 202 => "Target", 203 => "Weapon", _ => "Replacement" };
        }
        public GameObject? FindByName(ScriptExecutionScope scope, string name, bool includeInactive)
        {
            Record(scope, 0);
            if (name == "Ambiguous") throw new InvalidOperationException("Multiple object names matched.");
            return name == "Target" && Alive.Contains(202) ? new GameObject(202, scope.SceneLifetime, "target-guid") : null;
        }
        public GameObject? FindChild(ScriptExecutionScope scope, ulong objectId, ReadOnlySpan<string> names, bool includeInactive)
        {
            Record(scope, objectId); RequireTarget(objectId);
            return names.SequenceEqual(["Visual", "Weapon"]) && Alive.Contains(203) ? new GameObject(203, scope.SceneLifetime, "child-guid") : null;
        }
        public BatchResult TranslateWorld(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects, Vector3 displacement, bool activeOnly)
            => throw new NotSupportedException("This fixture does not use object batches.");
        public BatchResult ReadWorldPositions(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects, Span<Vector3> positions, Span<ObjectStatus> statuses)
            => throw new NotSupportedException("This fixture does not use object batches.");
        public BatchResult SetWorldPositions(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects, ReadOnlySpan<Vector3> positions, Span<ObjectStatus> statuses)
            => throw new NotSupportedException("This fixture does not use object batches.");
    }
}
