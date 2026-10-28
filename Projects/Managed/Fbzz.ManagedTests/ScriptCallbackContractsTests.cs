/// @file    ScriptCallbackContractsTests.cs
/// @brief   Verify synchronous delegate rejection, override discovery and recoverable Timeline startup through the Host.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Threading.Tasks;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class ScriptCallbackContractsTests
{
    internal static (string Name, Action Test)[] Cases =>
    [
        ("AsyncEngineDelegatesRejectBeforeRegistration", AsyncEngineDelegatesRejectBeforeRegistration),
        ("HostRejectsAsyncMulticastTargets", HostRejectsAsyncMulticastTargets),
        ("SynchronousOwnedCallbacksStillRunAndCancel", SynchronousOwnedCallbacksStillRunAndCancel),
        ("RoutineFirstAndLaterStepsUseReceiverJobScope", RoutineFirstAndLaterStepsUseReceiverJobScope),
        ("TimelineFailedStartCanRetry", TimelineFailedStartCanRetry),
        ("TimelineStepFailureFaultsOwnerAndCancelsChildren", TimelineStepFailureFaultsOwnerAndCancelsChildren),
        ("TimelineReentrancyAndInvalidStepsAreRejected", TimelineReentrancyAndInvalidStepsAreRejected),
        ("LifecycleDefaultsAndHiddenMethodsAreNotCallbacks", LifecycleDefaultsAndHiddenMethodsAreNotCallbacks),
        ("SharedPolicyProtectsManagedFallbacks", SharedPolicyProtectsManagedFallbacks)
        , ("EngineNotificationPayloadsShareExactContractsAndSequenceCodec", EngineNotificationPayloadsShareExactContractsAndSequenceCodec)
        , ("ValueScopesPreserveNestedRetainedAndCrossThreadAuthority", ValueScopeAuthority)
        , ("ValueScopesKeepRealCallbacksAllocationFreeAfterWarmup", ScopeAllocation)
        , ("ValueScopeWitnessesRemainDistinctAcrossGenerationReload", ScopeReload)
    ];

    private static void AsyncEngineDelegatesRejectBeforeRegistration()
    {
        using World world = new();
        int invoked = 0;
        Action asynchronous = async () => { invoked++; await Task.Yield(); };
        Action<float> tween = async _ => { invoked++; await Task.Yield(); };
        Action<int> handler = async _ => { invoked++; await Task.Yield(); };
        world.InUpdate(() =>
        {
            ScriptJobs jobs = world.Instance.Jobs;
            Program.Throws<ArgumentException>(() => jobs.Wait(0, asynchronous));
            Program.Throws<ArgumentException>(() => jobs.WaitFrames(1, asynchronous));
            Program.Throws<ArgumentException>(() => jobs.Repeat(1, asynchronous));
            Program.Throws<ArgumentException>(() => jobs.Tween(0, 1, 1, tween));
            Program.Throws<ArgumentException>(() => jobs.Tween(0, 1, 1, _ => { }, completed: asynchronous));
            Program.Throws<ArgumentException>(() => world.Instance.Events.Subscribe(handler));
            Program.Throws<ArgumentException>(() => jobs.Timeline().Then(asynchronous));
            Program.Throws<ArgumentException>(() => jobs.Timeline().Tween(0, 1, 1, tween));
        });
        Program.Check(invoked == 0 && world.Bridge.RegisterCount == 0 && world.Bridge.SubscribeCount == 0,
            "Async delegates must be rejected before any invocation, token registration or native reservation.");
    }

    private static void HostRejectsAsyncMulticastTargets()
    {
        using World world = new();
        int invoked = 0;
        Action action = () => invoked++;
        action += async () => { invoked++; await Task.Yield(); };
        Action<float> tween = async _ => { invoked++; await Task.Yield(); };
        Action<int> handler = async _ => { invoked++; await Task.Yield(); };
        world.InUpdate(() =>
        {
            ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
            ScriptBinding binding = world.Instance.Binding!;
            IScriptJobsRuntime runtime = world.Runtime;
            Program.Throws<ArgumentException>(() => runtime.Schedule(scope, binding, action, null, 0, 0, 0, false, JobEase.LINEAR, 0, 0));
            Program.Throws<ArgumentException>(() => runtime.Schedule(scope, binding, null, tween, 1, 0, 3, false, JobEase.LINEAR, 0, 1));
            Program.Throws<ArgumentException>(() => runtime.Subscribe(scope, binding, typeof(int), handler));
        });
        Program.Check(invoked == 0 && world.Bridge.RegisterCount == 0 && world.Bridge.SubscribeCount == 0,
            "Host-side safety must reject every async multicast target even when the public facade is bypassed.");
    }

    private static void SynchronousOwnedCallbacksStillRunAndCancel()
    {
        using World world = new();
        JobHandle job = default, subscription = default;
        int ran = 0, received = 0;
        world.InUpdate(() =>
        {
            job = world.Instance.Jobs.Wait(0.5f, () => { ran++; world.Instance.Transform.WorldPosition = Vector3.ZERO; });
            subscription = world.Instance.Events.Subscribe<int>(value => received += value);
        });
        Program.Check(world.Runtime.InvokeOwned(1, world.Id, 101, 2, job.Token, false, "", 0, true).Success, "A synchronous owned job must execute in a fresh live scope.");
        Program.Check(world.Runtime.InvokeOwned(1, world.Id, 101, 3, subscription.Token, true, "{\"$version\":1,\"value\":7}", 0, false).Success,
            "A typed synchronous event must retain its payload and scope.");
        world.InUpdate(() =>
        {
            Program.Check(!world.Instance.Jobs.Cancel(job), "A completed job is already absent.");
            Program.Check(world.Instance.Events.Unsubscribe(subscription), "A live subscription must still support explicit cancellation.");
        });
        Program.Check(ran == 1 && received == 7 && ScriptApiRuntime.Current is null, "Callbacks must run once and leave no ambient scope.");
        Program.Check(world.Runtime.InvokeOwned(1, world.Id, 101, 4, subscription.Token, true, "", 0, false).Status == BridgeStatus.NOT_FOUND,
            "A canceled subscription must not deliver again.");
    }

    private static void TimelineFailedStartCanRetry()
    {
        using World world = new();
        ScriptTimeline? timeline = null;
        world.InUpdate(() => timeline = world.Instance.Jobs.Timeline().Wait(0.1f).Then(() => { }));
        Program.Throws<InvalidOperationException>(() => timeline!.Play());
        world.Bridge.FailNextRegistration = true;
        world.InUpdate(() => Program.Throws<InvalidOperationException>(() => timeline!.Play()));
        Program.Check(world.Bridge.Registrations.Count == 0, "Rejected startup must leave no native job reservation.");
        JobHandle handle = default;
        world.InUpdate(() => handle = timeline!.Play());
        Program.Check(handle.IsValid && world.Bridge.Registrations.Count == 1, "Both scope and scheduler startup failures must leave the Timeline retryable.");
        world.InUpdate(() => Program.Throws<InvalidOperationException>(() => timeline!.Play()));
        Program.Check(world.Bridge.Registrations.Count == 1, "A successfully started Timeline remains a single-use object.");
    }

    private static void TimelineReentrancyAndInvalidStepsAreRejected()
    {
        using World world = new();
        ScriptTimeline? timeline = null;
        int reentrancyRejected = 0;
        JobHandle handle = default;
        world.InUpdate(() =>
        {
            timeline = world.Instance.Jobs.Timeline();
            Program.Throws<ArgumentOutOfRangeException>(() => timeline.Tween(0, 1, float.NaN, _ => { }));
            Program.Throws<ArgumentOutOfRangeException>(() => timeline.Tween(0, 1, 1, _ => { }, (JobEase)999));
            timeline.Then(() =>
            {
                Program.Throws<InvalidOperationException>(() => timeline.Play());
                Program.Throws<InvalidOperationException>(() => timeline.Then(() => { }));
                reentrancyRejected++;
            });
            handle = timeline.Play();
            Program.Check(handle.IsValid && reentrancyRejected == 0, "Play must register without executing a step in the lifecycle caller's scope.");
        });
        Program.Check(world.Deliver(handle).Success, "A Timeline step must run on its owned JOB delivery.");
        Program.Check(reentrancyRejected == 1 && world.Bridge.RegisterCount == 2, "A started Timeline must reject recursive Play and edits inside its first step.");
    }

    private static void TimelineStepFailureFaultsOwnerAndCancelsChildren()
    {
        using World world = new();
        ScriptTimeline? timeline = null;
        world.InUpdate(() => timeline = world.Instance.Jobs.Timeline().Tween(0, 1, 1, _ => { }));
        JobHandle handle = default;
        world.InUpdate(() => handle = timeline!.Play());
        world.Bridge.FailRegistrationNumber = 3;
        ManagedRuntimeResult result = world.Deliver(handle);
        Program.Check(result.Status == BridgeStatus.CALLBACK_FAILED && world.Bridge.Registrations.Count == 0 && world.Bridge.CancelOwnerCount == 1,
            "A step failure after successful Play must fault the owner and cancel both parent and registered Tween child.");
        Program.Check(world.Runtime.TryInvoke(1, world.Id, 101, 5, ScriptCallback.UPDATE).Status == BridgeStatus.CALLBACK_FAILED,
            "A later step failure must stop the Script rather than turn a successfully started Timeline into a retryable builder.");
        Program.Check(ScriptApiRuntime.Current is null, "The fault path must restore the ambient callback scope.");
    }

    private static void RoutineFirstAndLaterStepsUseReceiverJobScope()
    {
        using World world = new();
        Program.Check(world.Runtime.CreateInstance(1, "ManagedFixtures.LifecycleComponent", 202, out ulong receiverId).Success, "The receiver must construct.");
        Program.BindFixture(world.Runtime, 1, receiverId, 202, 202, "target-guid");
        Program.Check(world.Runtime.TryInstance(1, receiverId, out ManagedScriptRuntime.ScriptInstance? record), "The receiver must register.");
        Script receiver = record!.Instance;
        List<(ScriptCallback Callback, ulong Instance)> scopes = new();
        IEnumerable<Wait> Routine()
        {
            ScriptExecutionScope first = ScriptApiRuntime.RequireScope();
            scopes.Add((first.Callback, first.InstanceId));
            receiver.Transform.WorldPosition = Vector3.ZERO;
            Program.Throws<InvalidOperationException>(() => { _ = receiver.Time.DeltaSeconds; });
            yield return Wait.ForFrames(1);
            ScriptExecutionScope later = ScriptApiRuntime.RequireScope();
            scopes.Add((later.Callback, later.InstanceId));
        }
        JobHandle handle = default;
        world.InUpdate(() => handle = receiver.Jobs.Run(Routine().GetEnumerator()));
        Program.Check(handle.IsValid && scopes.Count == 0, "Run must not advance receiver code in the registering caller's scope.");
        Program.Check(world.Deliver(handle, receiverId, 202).Success && scopes.Count == 1, "The first MoveNext must occur in the receiver's owned JOB.");
        Program.Check(world.Deliver(handle, receiverId, 202).Success && scopes.Count == 2,
            "A later MoveNext must use the same receiver ownership contract.");
        Program.Check(scopes.All(value => value.Callback == ScriptCallback.JOB && value.Instance == receiverId) && ScriptApiRuntime.Current is null,
            "Both initial and resumed routine work must have receiver identity, JOB restrictions and bounded scope lifetime.");
    }

    private static void LifecycleDefaultsAndHiddenMethodsAreNotCallbacks()
    {
        using World world = new();
        ScriptTypeDescriptor empty = world.Runtime.GetCatalog(1)!.Find("ManagedFixtures.PrivateConstructorComponent")!;
        Program.Check(empty.Callbacks.Count == 0, "Unused base lifecycle defaults must not create dispatch entries.");
        ModuleBuilder module = AssemblyBuilder.DefineDynamicAssembly(new AssemblyName("HiddenLifecycleProbe"), AssemblyBuilderAccess.RunAndCollect).DefineDynamicModule("main");
        TypeBuilder type = module.DefineType("HiddenLifecycleProbe.Actor", TypeAttributes.Public, typeof(Script));
        type.DefineDefaultConstructor(MethodAttributes.Public);
        MethodBuilder hidden = type.DefineMethod("Update", MethodAttributes.Private, typeof(void), Type.EmptyTypes);
        hidden.GetILGenerator().Emit(OpCodes.Ret);
        CatalogBuildResult catalog = ScriptCatalogBuilder.Build(type.CreateType()!.Assembly);
        Program.Check(!catalog.Success && catalog.Diagnostics.Any(static value => value.Code == "CS_LIFECYCLE_SIGNATURE"),
            "The Host must reject a matching method that does not override the SDK lifecycle slot, even without an analyzer.");
        foreach (ScriptLifecycleDefinition callback in ScriptLifecycleContract.Callbacks)
        {
            MethodInfo method = typeof(Script).GetMethod(callback.Name, BindingFlags.Instance | BindingFlags.NonPublic)!;
            Program.Check(method.IsFamily && method.IsVirtual && method.ReturnType == typeof(void), "Every shared lifecycle must be available for IDE override completion.");
        }
    }

    private static void SharedPolicyProtectsManagedFallbacks()
    {
        using World world = new();
        ScriptExecutionScope? previous = ScriptApiRuntime.Current;
        try
        {
            foreach (ScriptLifecycleDefinition callback in ScriptLifecycleContract.Callbacks.Where(static value => value.ExecutionPolicy == "no_engine_api"))
            {
                ScriptApiRuntime.Current = new(world.Bridge, world.Runtime, 1, world.Id, 101, 2, 1, callback.Callback);
                Program.Throws<InvalidOperationException>(() => { _ = world.Instance.Transform.WorldPosition; });
            }
            ScriptApiRuntime.Current = new(world.Bridge, world.Runtime, 1, world.Id, 101, 3, 1, ScriptCallback.CONNECT);
            _ = world.Instance.Transform.WorldPosition;
            Program.Check(world.Instance.Jobs.Wait(0, () => { }).IsValid, "Connect permits read access and owned registration.");
            Program.Throws<InvalidOperationException>(() => world.Instance.Events.Publish(7));
        }
        finally { ScriptApiRuntime.Current = previous; }
        Program.Check(world.Bridge.PublishCount == 0, "Connect publication must be rejected before reaching a permissive fallback bridge.");
    }

    private static void EngineNotificationPayloadsShareExactContractsAndSequenceCodec()
    {
        Bridge bridge = new();
        using ManagedScriptRuntime runtime = new(bridge);
        Program.Check(runtime.LoadCandidate(Program.FixturePath("V1"), 1, internalMode: true).Success, "Engine notification fixture must load.");
        Program.Check(runtime.CreateInstance(1, "ManagedFixtures.EngineNotificationComponent", 101, out ulong id).Success, "Typed notification fixture must construct.");
        Program.BindFixture(runtime, 1, id);
        ScriptTypeDescriptor type = runtime.GetCatalog(1)!.Find("ManagedFixtures.EngineNotificationComponent")!;
        Program.Check(type.Callbacks.Count(value => ScriptCatalogBuilder.IsEngineNotification(value.Callback)) == 8,
            "All typed and parameterless Engine notifications must use the shared callback metadata.");
        AnimationEvent animation = new("足音\"\\合図", int.MinValue, 0.5f, 2.25f);
        Program.Check(runtime.TryInvokeEngineNotification(1, id, 101, 2, ScriptCallback.ON_ANIMATION_EVENT, animation).Success,
            "The exact AnimationEvent typed delegate must receive the snapshot.");
        Program.Check(runtime.TryInstance(1, id, out ManagedScriptRuntime.ScriptInstance? record), "Typed notification fixture must register.");
        Program.Check(Equals(Program.Field(type, "eventName").Read(record!.Instance), animation.Name)
            && Equals(Program.Field(type, "eventInteger").Read(record.Instance), animation.IntParam)
            && ScriptApiRuntime.Current is null, "Owned names, full-width values and bounded callback scope must survive delivery.");
        Program.Check(runtime.TryInvoke(1, id, 101, 3, ScriptCallback.ON_ANIMATION_EVENT).Status == BridgeStatus.INVALID_ARGUMENT
            && runtime.TryInvokeEngineNotification(1, id, 101, 4, ScriptCallback.ON_ANIMATION_EVENT, new SequenceEvent("wrong", 0, 0, 0, "")).Status == BridgeStatus.INVALID_ARGUMENT
            && runtime.TryInvokeEngineNotification(1, id, 101, 5, ScriptCallback.ON_SEQUENCE_EVENT, null).Status == BridgeStatus.INVALID_ARGUMENT,
            "Missing or mismatched typed payloads must reject before entering the callback.");
        SequenceEvent sequence = new("全体合図", int.MaxValue, -0.25f, 3.5f, "Sequence名");
        Program.Check(StructuredFieldCodec.IsSupported(typeof(SequenceEvent)), "The exact SDK SequenceEvent must be a supported owned event payload.");
        string json = StructuredFieldCodec.Encode(sequence, typeof(SequenceEvent));
        SequenceEvent restored = (SequenceEvent)StructuredFieldCodec.Decode(json, typeof(SequenceEvent), _ => throw new InvalidOperationException("No references expected."))!;
        Program.Check(restored.Name == sequence.Name && restored.IntParam == sequence.IntParam && restored.FloatParam == sequence.FloatParam
            && restored.SequenceTime == sequence.SequenceTime && restored.SequenceName == sequence.SequenceName,
            "Managed publication and native subscription must share the same exact typed Sequence value.");
    }

    /// @note Retaining a value witness must not turn reused native IDs into a new callback's permission.
    static void ValueScopeAuthority()
    {
        using World world = new();
        ScriptExecutionScope retained = default;
        world.InUpdate(() => {
            ScriptExecutionScope outer = ScriptApiRuntime.RequireScope(); retained = outer;
            ManagedRuntimeResult nested = world.Runtime.TryInvoke(1, world.Id, 101, 2, ScriptCallback.UPDATE);
            Program.Check(nested.Success && ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, outer), "Nested callbacks must restore every outer scope field and identity.");
            ScriptExecutionScope duplicate = new(outer.Bridge, outer.Components, outer.Generation, outer.InstanceId,
                outer.OwnerId, outer.CallId, outer.SceneLifetime, outer.Callback);
            Program.Check(!ScriptApiRuntime.IsSameScope(duplicate, outer), "Equal native context fields cannot make a newly constructed witness identical.");
            Program.Throws<InvalidOperationException>(() => ((IScriptComponentLookup)world.Runtime).GetComponents(duplicate, world.Instance.GameObject, typeof(Script)));
            bool rejected = false;
            Task.Run(() => {
                ScriptApiRuntime.Current = outer;
                try { ScriptApiRuntime.RequireScope(); }
                catch (InvalidOperationException) { rejected = true; }
                finally { ScriptApiRuntime.Current = null; }
            }).GetAwaiter().GetResult();
            Program.Check(rejected && ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, outer), "A copied scope cannot authorize another thread or replace the caller's TLS context.");
        });
        Program.Check(ScriptApiRuntime.Current is null, "A completed callback must not leave value authority installed.");
        world.InUpdate(() => {
            Program.Check(retained.CallId == ScriptApiRuntime.RequireScope().CallId && !ScriptApiRuntime.IsSameScope(retained, ScriptApiRuntime.Current),
                "Even a repeated native CallId must produce a distinct immutable callback witness.");
            Program.Throws<InvalidOperationException>(() => ((IScriptComponentLookup)world.Runtime).GetComponents(retained, world.Instance.GameObject, typeof(Script)));
        });
    }
    /// @note This verifies managed allocation only, not timing; the real fixture performs Update plus two time reads and a Transform call.
    static void ScopeAllocation()
    {
        using World world = new();
        for (ulong call = 1; call <= 64; call++) world.Runtime.TryInvoke(1, world.Id, 101, call, ScriptCallback.UPDATE);
        long before = GC.GetAllocatedBytesForCurrentThread(); bool success = true;
        for (ulong call = 65; call <= 320; call++) success &= world.Runtime.TryInvoke(1, world.Id, 101, call, ScriptCallback.UPDATE).Success;
        long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
        Program.Check(success && allocated == 0 && ScriptApiRuntime.Current is null,
            $"Warmed real callbacks must not allocate execution-scope objects or leave current authority. bytes={allocated}; calls=256; success={success}; currentIsNull={ScriptApiRuntime.Current is null}.");
        Program.Check(world.Runtime.CreateInstance(1, "ManagedFixtures.PrivateConstructorComponent", 101, out ulong empty).Success, "The empty lifecycle fixture must construct.");
        Program.BindFixture(world.Runtime, 1, empty);
        world.Runtime.TryInvoke(1, empty, 101, 321, ScriptCallback.UPDATE);
        before = GC.GetAllocatedBytesForCurrentThread();
        for (ulong call = 322; call <= 577; call++) success &= world.Runtime.TryInvoke(1, empty, 101, call, ScriptCallback.UPDATE).Success;
        allocated = GC.GetAllocatedBytesForCurrentThread() - before;
        Program.Check(success && allocated == 0,
            $"Absent callbacks must preserve their existing allocation-free success path. bytes={allocated}; calls=256; success={success}; currentIsNull={ScriptApiRuntime.Current is null}.");
    }
    static void ScopeReload()
    {
        using World world = new(); ScriptExecutionScope original = default;
        world.InUpdate(() => original = ScriptApiRuntime.RequireScope());
        Program.Check(world.Runtime.LoadCandidate(Program.FixturePath("V2"), 2, internalMode: true).Success, "A new code generation must prepare while no callback is active.");
        Program.Check(world.Runtime.CreateInstance(2, "ManagedFixtures.LifecycleComponent", 101, out ulong replacement).Success, "The replacement generation must construct.");
        Program.BindFixture(world.Runtime, 2, replacement);
        bool checkedNew = false;
        world.Bridge.DuringTranslate = () => {
            world.Bridge.DuringTranslate = null;
            ScriptExecutionScope current = ScriptApiRuntime.RequireScope();
            Program.Check(current.Generation == 2 && current.Identity != original.Identity && original.Generation == 1,
                "Loading a generation must never mutate or recycle a retained execution witness.");
            GameObject currentOwner = new(101, current.SceneLifetime, "owner-101");
            Program.Throws<InvalidOperationException>(() => ((IScriptComponentLookup)world.Runtime).GetComponents(original, currentOwner, typeof(Script)));
            checkedNew = true;
        };
        Program.Check(world.Runtime.TryInvoke(2, replacement, 101, 1, ScriptCallback.UPDATE).Success && checkedNew && ScriptApiRuntime.Current is null,
            "The replacement callback must preserve generation ownership and restore idle authority.");
    }

    private sealed class Bridge : Program.RecordingBridge, IScriptJobNativeBridge
    {
        internal int RegisterCount, SubscribeCount, PublishCount, RegisterAttempts, FailRegistrationNumber, CancelCount, CancelOwnerCount;
        internal ulong LastRegisteredToken;
        internal bool FailNextRegistration;
        internal readonly HashSet<ulong> Registrations = new();
        public void RegisterJob(ScriptExecutionScope scope, ulong receiver, ulong token, float delay, float interval, uint kind, bool unscaled, JobEase ease, float from, float to)
        {
            RegisterAttempts++;
            if (FailNextRegistration || RegisterAttempts == FailRegistrationNumber)
            { FailNextRegistration = false; throw new InvalidOperationException("Test scheduler refused startup."); }
            RegisterCount++; Registrations.Add(token); LastRegisteredToken = token;
        }
        public void CancelJob(ScriptExecutionScope scope, ulong receiver, ulong token) { CancelCount++; Registrations.Remove(token); }
        public void CancelOwner(ScriptExecutionScope scope, ulong receiver) { CancelOwnerCount++; Registrations.Clear(); }
        public void SubscribeEvent(ScriptExecutionScope scope, ulong receiver, ulong token, string channel) { SubscribeCount++; }
        public void PublishEvent(ScriptExecutionScope scope, string channel, string json) { PublishCount++; }
        public GameObject ResolveEventReference(ScriptExecutionScope scope, string guid) => new(202, scope.SceneLifetime, guid);
    }

    private sealed class World : IDisposable
    {
        internal World()
        {
            Runtime = new ManagedScriptRuntime(Bridge);
            ManagedRuntimeResult load = Runtime.LoadCandidate(Program.FixturePath("V1"), 1, internalMode: true);
            Program.Check(load.Success, load.Message);
            Program.Check(Runtime.CreateInstance(1, "ManagedFixtures.LifecycleComponent", 101, out ulong id).Success, "The real lifecycle fixture must construct.");
            Id = id;
            Program.BindFixture(Runtime, 1, Id);
            Program.Check(Runtime.TryInstance(1, Id, out ManagedScriptRuntime.ScriptInstance? record), "Fixture instance must register.");
            Instance = record!.Instance;
        }
        internal Bridge Bridge { get; } = new();
        internal ManagedScriptRuntime Runtime { get; }
        internal ulong Id { get; }
        internal Script Instance { get; }
        internal ManagedRuntimeResult Deliver(JobHandle handle, ulong? receiver = null, ulong owner = 101)
        {
            Bridge.Registrations.Remove(handle.Token);
            return Runtime.InvokeOwned(1, receiver ?? Id, owner, 2, handle.Token, false, "", 0, true);
        }
        internal void InUpdate(Action action)
        {
            Bridge.DuringTranslate = () => { Bridge.DuringTranslate = null; action(); };
            ManagedRuntimeResult result = Runtime.TryInvoke(1, Id, 101, 1, ScriptCallback.UPDATE);
            Program.Check(result.Success, result.Message);
        }
        public void Dispose() => Runtime.Dispose();
    }
}
