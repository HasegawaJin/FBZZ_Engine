/// @file    ManagedScriptJobs.cs
/// @brief   owner ごとの delegate を native scheduler と同期イベントへ接続する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Fbzz;

namespace Fbzz.ManagedHost;

internal interface IScriptJobNativeBridge
{
    void RegisterJob(ScriptExecutionScope scope, ulong receiver, ulong token, float delay, float interval, uint kind, bool unscaled, JobEase ease, float from, float to);
    void CancelJob(ScriptExecutionScope scope, ulong receiver, ulong token);
    void CancelOwner(ScriptExecutionScope scope, ulong receiver);
    void SubscribeEvent(ScriptExecutionScope scope, ulong receiver, ulong token, string channel);
    void PublishEvent(ScriptExecutionScope scope, string channel, string json);
    GameObject ResolveEventReference(ScriptExecutionScope scope, string guid);
}

public sealed partial class ManagedScriptRuntime : IScriptJobsRuntime
{
    readonly Dictionary<ulong, OwnedJob> m_jobs = new();
    readonly HashSet<ulong> m_connecting = new();
    ulong m_nextJob = 1;
    ulong m_activeRoutine;

    IScriptJobNativeBridge JobBridge(ScriptExecutionScope scope)
    {
        if (!ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, scope) || scope.ThreadId != m_threadId || !TryInstance(scope.Generation, scope.InstanceId, out ScriptInstance? record)
            || record.Faulted || !record.Binding.Active) throw new InvalidOperationException("Owned jobs require the current live Script.");
        if (ScriptLifecycleContract.GetExecutionPolicy(scope.Callback) == "no_engine_api")
            throw new InvalidOperationException($"{scope.Callback} does not permit owned jobs or subscriptions.");
        return m_bridge as IScriptJobNativeBridge ?? throw new NotSupportedException("Native owned jobs are unavailable.");
    }
    ScriptInstance Receiver(ScriptExecutionScope scope, ScriptBinding receiver)
    {
        if (receiver.Generation != scope.Generation || receiver.GameObject.SceneLifetime != scope.SceneLifetime || !receiver.Active
            || !TryInstance(receiver.Generation, receiver.InstanceId, out ScriptInstance? record) || !ReferenceEquals(record.Binding, receiver) || record.Faulted)
            throw new InvalidOperationException("Job receiver is unavailable or belongs to another Scene / generation.");
        return record;
    }
    JobHandle IScriptJobsRuntime.Schedule(ScriptExecutionScope scope, ScriptBinding receiver, Action? action, Action<float>? tween, float delay, float interval, uint kind,
        bool unscaled, JobEase ease, float from, float to)
    {
        ScriptCallbackSafety.RequireSynchronous(action, nameof(action));
        ScriptCallbackSafety.RequireSynchronous(tween, nameof(tween));
        IScriptJobNativeBridge bridge = JobBridge(scope);
        Receiver(scope, receiver);
        if (m_jobs.Count >= 32768) throw new InvalidOperationException("Owned jobs exceed their capacity.");
        ulong token = m_nextJob++;
        OwnedJob job = new(receiver.Generation, receiver.InstanceId, receiver.OwnerId) { Action = action, Tween = tween,
            ParentToken = m_jobs.TryGetValue(m_activeRoutine, out OwnedJob? parent) && parent.InstanceId == receiver.InstanceId ? m_activeRoutine : 0 };
        m_jobs.Add(token, job);
        try { bridge.RegisterJob(scope, receiver.InstanceId, token, delay, interval, kind, unscaled, ease, from, to); }
        catch { m_jobs.Remove(token); throw; }
        return new(receiver.Generation, receiver.InstanceId, token);
    }
    bool IScriptJobsRuntime.Cancel(ScriptExecutionScope scope, ScriptBinding receiver, JobHandle handle)
    {
        IScriptJobNativeBridge bridge = JobBridge(scope);
        Receiver(scope, receiver);
        if (handle.Generation != receiver.Generation || handle.Instance != receiver.InstanceId) throw new InvalidOperationException("Job handle belongs to another owner or code generation.");
        if (!m_jobs.TryGetValue(handle.Token, out OwnedJob? job)) return false;
        CancelChildren(scope, handle.Token, bridge);
        bridge.CancelJob(scope, receiver.InstanceId, handle.Token);
        RemoveJob(handle.Token, job);
        return true;
    }
    void IScriptJobsRuntime.CancelAll(ScriptExecutionScope scope, ScriptBinding receiver)
    { Receiver(scope, receiver); JobBridge(scope).CancelOwner(scope, receiver.InstanceId); ClearOwnedJobs(receiver.Generation, receiver.InstanceId); }
    JobHandle IScriptJobsRuntime.Subscribe(ScriptExecutionScope scope, ScriptBinding receiver, Type type, Delegate handler)
    {
        ScriptCallbackSafety.RequireSynchronous(handler, nameof(handler));
        IScriptJobNativeBridge bridge = JobBridge(scope);
        Receiver(scope, receiver);
        if (m_jobs.Count >= 32768) throw new InvalidOperationException("Owned jobs exceed their capacity.");
        if (!StructuredFieldCodec.IsSupported(type)) throw new InvalidDataException("Event payload requires a supported serializable value.");
        ulong token = m_nextJob++;
        OwnedJob job = new(receiver.Generation, receiver.InstanceId, receiver.OwnerId) { EventType = type, Handler = handler,
            Pending = m_connecting.Contains(scope.InstanceId), PendingCaller = m_connecting.Contains(scope.InstanceId) ? scope.InstanceId : 0 };
        m_jobs.Add(token, job);
        try { if (!job.Pending) bridge.SubscribeEvent(scope, receiver.InstanceId, token, Channel(type)); }
        catch { m_jobs.Remove(token); throw; }
        return new(receiver.Generation, receiver.InstanceId, token);
    }
    void IScriptJobsRuntime.Publish(ScriptExecutionScope scope, Type type, object? value)
    {
        if (ScriptLifecycleContract.GetExecutionPolicy(scope.Callback) != "live")
            throw new InvalidOperationException($"{scope.Callback} does not permit event publication.");
        JobBridge(scope).PublishEvent(scope, Channel(type), StructuredFieldCodec.Encode(value, type));
    }

    JobHandle IScriptJobsRuntime.Run(ScriptExecutionScope scope, ScriptBinding receiver, IEnumerator<Wait> routine)
    {
        IScriptJobNativeBridge bridge = JobBridge(scope);
        Receiver(scope, receiver);
        if (m_jobs.Count >= 32768) throw new InvalidOperationException("Owned jobs exceed their capacity.");
        ulong token = m_nextJob++;
        OwnedJob job = new(receiver.Generation, receiver.InstanceId, receiver.OwnerId) { Routine = routine };
        m_jobs.Add(token, job);
        try
        {
            /// @note Initial MoveNext also runs in the receiver's fresh JOB scope, never in the registering caller's lifecycle.
            bridge.RegisterJob(scope, job.InstanceId, token, 0f, 0f, 1U, false, JobEase.LINEAR, 0f, 0f);
            return new(receiver.Generation, receiver.InstanceId, token);
        }
        catch { CancelChildren(scope, token, bridge, bestEffort: true); RemoveJob(token, job); throw; }
    }
    bool AdvanceRoutine(ScriptExecutionScope scope, ulong token, OwnedJob job, IScriptJobNativeBridge bridge)
    {
        ulong previous = m_activeRoutine;
        bool advanced;
        try { m_activeRoutine = token; advanced = job.Routine!.MoveNext(); }
        finally { m_activeRoutine = previous; }
        if (!Retained(token, job)) return false;
        if (!advanced) { CancelChildren(scope, token, bridge); return false; }
        Wait wait = job.Routine.Current;
        if (!Retained(token, job)) return false;
        job.Condition = wait.Condition;
        bridge.RegisterJob(scope, job.InstanceId, token, wait.Condition is not null ? 1 : wait.Frames != 0 ? wait.Frames : wait.Seconds,
            0f, wait.Condition is not null || wait.Frames != 0 ? 1U : 0U, false, JobEase.LINEAR, 0f, 0f);
        return true;
    }
    static string Channel(Type type) => "managed:" + (type.FullName ?? type.Name);
    bool Retained(ulong token, OwnedJob job) => m_jobs.TryGetValue(token, out OwnedJob? current) && ReferenceEquals(current, job);
    void CommitConnections(ScriptExecutionScope scope)
    {
        IScriptJobNativeBridge bridge = JobBridge(scope);
        foreach (KeyValuePair<ulong, OwnedJob> entry in m_jobs.Where((KeyValuePair<ulong, OwnedJob> entry)
            => entry.Value.Generation == scope.Generation && entry.Value.PendingCaller == scope.InstanceId && entry.Value.Pending).ToArray())
        { bridge.SubscribeEvent(scope, entry.Value.InstanceId, entry.Key, Channel(entry.Value.EventType!)); entry.Value.Pending = false; }
        foreach (OwnedJob job in m_jobs.Values.Where((OwnedJob job) => job.PendingCaller == scope.InstanceId)) job.PendingCaller = 0;
    }
    void RemoveJob(ulong token, OwnedJob job)
    {
        if (!Retained(token, job)) return;
        m_jobs.Remove(token);
        ScriptExecutionScope? previous = ScriptApiRuntime.Current;
        try { ScriptApiRuntime.Current = null; job.Routine?.Dispose(); } catch (Exception) { }
        finally { ScriptApiRuntime.Current = previous; }
    }
    void CancelChildren(ScriptExecutionScope scope, ulong parent, IScriptJobNativeBridge bridge, bool bestEffort = false)
    {
        foreach (KeyValuePair<ulong, OwnedJob> child in m_jobs.Where((KeyValuePair<ulong, OwnedJob> child) => child.Value.ParentToken == parent).ToArray())
        {
            CancelChildren(scope, child.Key, bridge, bestEffort);
            try { bridge.CancelJob(scope, child.Value.InstanceId, child.Key); }
            catch (Exception) when (bestEffort) { }
            finally { if (bestEffort) RemoveJob(child.Key, child.Value); }
            if (!bestEffort) RemoveJob(child.Key, child.Value);
        }
    }
    void ClearOwnedJobs(ulong generation, ulong instance)
    {
        foreach (KeyValuePair<ulong, OwnedJob> entry in m_jobs.Where((KeyValuePair<ulong, OwnedJob> entry)
            => entry.Value.Generation == generation && (entry.Value.InstanceId == instance || entry.Value.PendingCaller == instance)).ToArray()) RemoveJob(entry.Key, entry.Value);
        m_connecting.Remove(instance);
    }
    public ManagedRuntimeResult CancelOwnedJobs(ulong generation, ulong instanceId)
    {
        if (Environment.CurrentManagedThreadId != m_threadId) return new(BridgeStatus.INVALID_SCOPE);
        if (!TryInstance(generation, instanceId, out _)) return new(BridgeStatus.STALE_GENERATION);
        ClearOwnedJobs(generation, instanceId);
        return new(BridgeStatus.OK);
    }
    public ManagedRuntimeResult InvalidateOwnedAssets(ulong generation, ulong instanceId)
    {
        if (Environment.CurrentManagedThreadId != m_threadId) return new(BridgeStatus.INVALID_SCOPE);
        if (!TryInstance(generation, instanceId, out ScriptInstance? record)) return new(BridgeStatus.STALE_GENERATION);
        record.Binding.Assets.InvalidateOwned();
        return new(BridgeStatus.OK);
    }
    void CancelFailedJobs(ScriptExecutionScope scope)
    {
        if (m_bridge is not IScriptJobNativeBridge bridge) return;
        try
        {
            foreach (KeyValuePair<ulong, OwnedJob> job in m_jobs.Where((KeyValuePair<ulong, OwnedJob> job)
                => job.Value.PendingCaller == scope.InstanceId).ToArray()) bridge.CancelJob(scope, job.Value.InstanceId, job.Key);
            bridge.CancelOwner(scope, scope.InstanceId);
        }
        catch (Exception) { }
    }
    /// @note 再送しない所有者 callback。各購読者に新 scope を与え、発火元 scope は finally で復元する。
    public ManagedRuntimeResult InvokeOwned(ulong generation, ulong instanceId, ulong ownerId, ulong callId, ulong token,
        bool eventDelivery, string json, float progress, bool completed, SequenceEvent? nativeSequence = null)
    {
        if (Environment.CurrentManagedThreadId != m_threadId || callId == 0) return new(BridgeStatus.INVALID_SCOPE);
        if (!TryInstance(generation, instanceId, out ScriptInstance? record) || record.OwnerId != ownerId || !record.Binding.Active)
            return new(BridgeStatus.STALE_GENERATION);
        if (record.Faulted) return new(BridgeStatus.CALLBACK_FAILED, "The Script is faulted.");
        if (!m_jobs.TryGetValue(token, out OwnedJob? job) || job.Generation != generation || job.InstanceId != instanceId || job.Pending)
            return new(BridgeStatus.NOT_FOUND);
        ScriptExecutionScope? previous = ScriptApiRuntime.Current;
        ScriptExecutionScope scope = new(m_bridge, this, generation, instanceId, ownerId, callId, record.Binding.GameObject.SceneLifetime,
            eventDelivery ? ScriptCallback.EVENT : ScriptCallback.JOB);
        try
        {
            ScriptApiRuntime.Current = scope;
            if (eventDelivery)
            {
                if (job.EventType is null || job.Handler is null) throw new InvalidDataException("Token is not an event subscription.");
                if (nativeSequence is SequenceEvent sequence) {
                    if (job.EventType != typeof(SequenceEvent)) throw new InvalidDataException("Native Sequence events require their exact SDK payload type.");
                    ((Action<SequenceEvent>)job.Handler)(sequence);
                } else {
                    IScriptJobNativeBridge native = JobBridge(scope);
                    object? payload;
                    try { ScriptApiRuntime.Current = null; payload = StructuredFieldCodec.Decode(json, job.EventType, (string guid) => native.ResolveEventReference(scope, guid)); }
                    finally { ScriptApiRuntime.Current = scope; }
                    job.Handler.DynamicInvoke(payload);
                }
            }
            else if (job.Routine is not null)
            {
                IScriptJobNativeBridge bridge = JobBridge(scope);
                bool ready = job.Condition is null || job.Condition();
                if (Retained(token, job)) {
                    if (!ready) bridge.RegisterJob(scope, job.InstanceId, token, 1f, 0f, 1, false, JobEase.LINEAR, 0f, 0f);
                    else if (!AdvanceRoutine(scope, token, job, bridge)) RemoveJob(token, job);
                }
            }
            else
            {
                if (completed) m_jobs.Remove(token);
                if (job.Tween is Action<float> tween) { tween(progress); if (completed) job.Action?.Invoke(); }
                else job.Action?.Invoke();
            }
            return new(BridgeStatus.OK);
        }
        catch (Exception exception)
        {
            record.Faulted = true;
            CancelFailedJobs(scope);
            ClearOwnedJobs(generation, instanceId);
            return new(BridgeStatus.CALLBACK_FAILED, Describe(exception));
        }
        finally { ScriptApiRuntime.Current = previous; }
    }
    sealed class OwnedJob
    {
        internal OwnedJob(ulong generation, ulong instance, ulong owner) { Generation = generation; InstanceId = instance; OwnerId = owner; }
        internal ulong Generation { get; }
        internal ulong InstanceId { get; }
        internal ulong OwnerId { get; }
        internal Action? Action;
        internal Action<float>? Tween;
        internal IEnumerator<Wait>? Routine;
        internal Func<bool>? Condition;
        internal Type? EventType;
        internal Delegate? Handler;
        internal bool Pending;
        internal ulong PendingCaller;
        internal ulong ParentToken;
    }
}
