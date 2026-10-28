/// @file    ScriptJobs.cs
/// @brief   Script 個体に所有させる待機・補間・タイムライン・同期イベント。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;

namespace Fbzz;

public enum JobEase { LINEAR, IN_QUAD, OUT_QUAD, IN_OUT_QUAD, IN_CUBIC, OUT_CUBIC, IN_OUT_CUBIC, IN_SINE, OUT_SINE, IN_OUT_SINE }

/// @note 同じ所有 Script の callback 中だけ解除できる。破棄・無効化・fault・世代退役で自動解除する。
public readonly struct JobHandle
{
    internal JobHandle(ulong generation, ulong instance, ulong token) { Generation = generation; Instance = instance; Token = token; }
    internal ulong Generation { get; }
    internal ulong Instance { get; }
    internal ulong Token { get; }
    public bool IsValid => Token != 0;
}

public readonly struct Wait
{
    internal Wait(float seconds, int frames, Func<bool>? condition) { Seconds = seconds; Frames = frames; Condition = condition; }
    internal float Seconds { get; }
    internal int Frames { get; }
    internal Func<bool>? Condition { get; }
    public static Wait ForSeconds(float seconds)
    { if (!float.IsFinite(seconds) || seconds < 0f) throw new ArgumentOutOfRangeException(nameof(seconds)); return new(seconds, 0, null); }
    public static Wait ForFrames(int frames)
    { if (frames < 0 || frames > 16777216) throw new ArgumentOutOfRangeException(nameof(frames)); return new(0f, frames, null); }
    public static Wait Until(Func<bool> condition)
    { ArgumentNullException.ThrowIfNull(condition); return new(0f, 1, condition); }
}

public sealed class ScriptJobs
{
    internal ScriptJobs(ScriptBinding owner) { m_owner = owner; }
    readonly ScriptBinding m_owner;
    IScriptJobsRuntime Runtime(out ScriptExecutionScope scope)
    {
        scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Components as IScriptJobsRuntime ?? throw new NotSupportedException("Owned jobs are unavailable.");
    }
    [ApiContract(Call = "subscription", ParameterUnits = new[] { "seconds=s" })]
    public JobHandle Wait(float seconds, [SynchronousCallback] Action action, bool unscaled = false)
    {
        ArgumentNullException.ThrowIfNull(action); ScriptCallbackSafety.RequireSynchronous(action, nameof(action));
        if (!float.IsFinite(seconds) || seconds < 0f) throw new ArgumentOutOfRangeException(nameof(seconds));
        IScriptJobsRuntime runtime = Runtime(out ScriptExecutionScope scope);
        return runtime.Schedule(scope, m_owner, action, null, seconds, 0f, 0, unscaled, JobEase.LINEAR, 0f, 0f);
    }
    [ApiContract(Call = "subscription")]
    public JobHandle WaitFrames(int frames, [SynchronousCallback] Action action)
    {
        ArgumentNullException.ThrowIfNull(action); ScriptCallbackSafety.RequireSynchronous(action, nameof(action));
        if (frames < 0 || frames > 16777216) throw new ArgumentOutOfRangeException(nameof(frames));
        IScriptJobsRuntime runtime = Runtime(out ScriptExecutionScope scope);
        return runtime.Schedule(scope, m_owner, action, null, frames, 0f, 1, false, JobEase.LINEAR, 0f, 0f);
    }
    /// @note 長い一フレームでも一回まで配送し、残余時間を次のフレームへ保持する。
    [ApiContract(Call = "subscription", ParameterUnits = new[] { "interval=s", "delay=s" })]
    public JobHandle Repeat(float interval, [SynchronousCallback] Action action, float delay = 0f, bool unscaled = false)
    {
        ArgumentNullException.ThrowIfNull(action); ScriptCallbackSafety.RequireSynchronous(action, nameof(action));
        if (!float.IsFinite(interval) || interval <= 0f || !float.IsFinite(delay) || delay < 0f) throw new ArgumentOutOfRangeException(nameof(interval));
        IScriptJobsRuntime runtime = Runtime(out ScriptExecutionScope scope);
        return runtime.Schedule(scope, m_owner, action, null, delay, interval, 2, unscaled, JobEase.LINEAR, 0f, 0f);
    }
    [ApiContract(Call = "subscription", ParameterUnits = new[] { "seconds=s", "from=caller_defined", "to=caller_defined" })]
    public JobHandle Tween(float from, float to, float seconds, [SynchronousCallback] Action<float> apply, JobEase ease = JobEase.LINEAR, bool unscaled = false, [SynchronousCallback] Action? completed = null)
    {
        ArgumentNullException.ThrowIfNull(apply); ScriptCallbackSafety.RequireSynchronous(apply, nameof(apply));
        if (!float.IsFinite(from) || !float.IsFinite(to) || !float.IsFinite(seconds) || seconds < 0f || !Enum.IsDefined(ease))
            throw new ArgumentOutOfRangeException(nameof(seconds));
        IScriptJobsRuntime runtime = Runtime(out ScriptExecutionScope scope);
        ScriptCallbackSafety.RequireSynchronous(completed, nameof(completed));
        return runtime.Schedule(scope, m_owner, completed, apply, seconds, 0f, 3, unscaled, ease, from, to);
    }
    [ApiContract(Call = "subscription")]
    public bool Cancel(JobHandle handle)
    {
        IScriptJobsRuntime runtime = Runtime(out ScriptExecutionScope scope);
        return runtime.Cancel(scope, m_owner, handle);
    }
    [ApiContract(Call = "subscription")]
    public void CancelAll()
    { IScriptJobsRuntime runtime = Runtime(out ScriptExecutionScope scope); runtime.CancelAll(scope, m_owner); }
    [ApiContract(Call = "subscription", Summary = "Registers a routine; its first and subsequent MoveNext calls run later in fresh Job callbacks owned by this script.", ParameterHelp = new[] { "routine=Synchronous iterator; registration does not execute its body in the caller callback." }, Failure = "null_routine_or_invalid_scope_or_registration_failure_throws_and_disposes_routine; execution_exception_faults_owner_and_cancels_owned_work")]
    public JobHandle Run(IEnumerator<Wait> routine)
    {
        ArgumentNullException.ThrowIfNull(routine);
        IScriptJobsRuntime runtime = Runtime(out ScriptExecutionScope scope);
        return runtime.Run(scope, m_owner, routine);
    }
    [ApiContract(Call = "any", Purity = ApiPurity.MANAGED, RunModes = new[] { "any" }, Lifetime = "managed_timeline_builder", Summary = "Creates an empty timeline builder without scheduling a job.")]
    public ScriptTimeline Timeline() => new(this);
}

/// @note Play は一度だけ。各 step は同じ owner の JOB callback で進む。
public sealed class ScriptTimeline
{
    readonly ScriptJobs m_jobs;
    readonly List<Step> m_steps = new();
    bool m_started;
    bool m_starting;
    internal ScriptTimeline(ScriptJobs jobs) { m_jobs = jobs; }
    void Add(Step step)
    { if (m_started || m_starting || m_steps.Count >= 1024) throw new InvalidOperationException("Timeline is already running or exceeds its step limit."); m_steps.Add(step); }
    [ApiContract(Call = "any", Purity = ApiPurity.MANAGED, RunModes = new[] { "any" }, ParameterUnits = new[] { "seconds=s" }, Lifetime = "managed_timeline_builder", Summary = "Appends a wait before the timeline is started.", Failure = "negative_or_nonfinite_seconds_or_started_or_1024_steps_throws")]
    public ScriptTimeline Wait(float seconds)
    { Fbzz.Wait.ForSeconds(seconds); Add(new(0, seconds, null, null, 0, 0, JobEase.LINEAR)); return this; }
    [ApiContract(Call = "any", Purity = ApiPurity.MANAGED, RunModes = new[] { "any" }, Lifetime = "managed_timeline_builder", Summary = "Validates and appends a synchronous action; it runs later under the owning script Job scope.", Failure = "null_or_async_action_or_started_or_starting_or_1024_steps_throws")]
    public ScriptTimeline Then([SynchronousCallback] Action action)
    { ArgumentNullException.ThrowIfNull(action); ScriptCallbackSafety.RequireSynchronous(action, nameof(action)); Add(new(1, 0, action, null, 0, 0, JobEase.LINEAR)); return this; }
    [ApiContract(Call = "any", Purity = ApiPurity.MANAGED, RunModes = new[] { "any" }, ParameterUnits = new[] { "seconds=s", "from=caller_defined", "to=caller_defined" }, Lifetime = "managed_timeline_builder", Summary = "Validates finite endpoints, nonnegative duration, easing and a synchronous callback before appending a scalar tween.", Failure = "null_or_async_apply_or_nonfinite_endpoints_or_negative_or_nonfinite_seconds_or_invalid_ease_or_started_or_starting_or_1024_steps_throws")]
    public ScriptTimeline Tween(float from, float to, float seconds, [SynchronousCallback] Action<float> apply, JobEase ease = JobEase.LINEAR)
    {
        ArgumentNullException.ThrowIfNull(apply); ScriptCallbackSafety.RequireSynchronous(apply, nameof(apply));
        if (!float.IsFinite(from) || !float.IsFinite(to) || !float.IsFinite(seconds) || seconds < 0f || !Enum.IsDefined(ease))
            throw new ArgumentOutOfRangeException(nameof(seconds));
        Add(new(2, seconds, null, apply, from, to, ease));
        return this;
    }
    [ApiContract(Call = "subscription", Purity = ApiPurity.ENGINE, RunModes = new[] { "Play" }, Lifetime = "owning_script_generation_and_scene", Summary = "Registers this nonempty timeline; its initial and later steps run in owned Job callbacks. A successful registration consumes the builder.", Failure = "empty_or_already_started_or_reentrant_start_or_invalid_scope_throws; registration_failure_can_be_retried; after_successful_start_step_exception_faults_owner_and_cancels_owned_work; successfully_started_builder_is_not_reusable")]
    public JobHandle Play()
    {
        if (m_started || m_starting || m_steps.Count == 0) throw new InvalidOperationException("Timeline cannot be started.");
        m_starting = true;
        try
        {
            JobHandle handle = m_jobs.Run(Routine());
            m_started = true;
            return handle;
        }
        finally { m_starting = false; }
    }
    IEnumerator<Wait> Routine()
    {
        foreach (Step step in m_steps)
        {
            if (step.Kind == 0) yield return Fbzz.Wait.ForSeconds(step.Seconds);
            else if (step.Kind == 1) { step.Action!(); yield return Fbzz.Wait.ForFrames(0); }
            else
            {
                bool done = false;
                m_jobs.Tween(step.From, step.To, step.Seconds, step.Apply!, step.Ease, completed: () => done = true);
                yield return Fbzz.Wait.Until(() => done);
            }
        }
    }
    sealed record Step(int Kind, float Seconds, Action? Action, Action<float>? Apply, float From, float To, JobEase Ease);
}

public sealed class ScriptEvents
{
    internal ScriptEvents(ScriptBinding owner) { m_owner = owner; }
    readonly ScriptBinding m_owner;
    IScriptJobsRuntime Runtime(out ScriptExecutionScope scope)
    { scope = ScriptApiRuntime.RequireScope(m_owner); return scope.Components as IScriptJobsRuntime ?? throw new NotSupportedException("Events are unavailable."); }
    [ApiContract(Call = "subscription", Summary = "Subscribes this script to a typed event; Connect may register subscriptions only.", Failure = "null_handler_or_invalid_scope_or_inactive_receiver_throws")]
    public JobHandle Subscribe<T>([SynchronousCallback] Action<T> handler)
    { ArgumentNullException.ThrowIfNull(handler); ScriptCallbackSafety.RequireSynchronous(handler, nameof(handler)); IScriptJobsRuntime runtime = Runtime(out ScriptExecutionScope scope); return runtime.Subscribe(scope, m_owner, typeof(T), handler); }
    [ApiContract(Call = "subscription")]
    public bool Unsubscribe(JobHandle handle)
    { IScriptJobsRuntime runtime = Runtime(out ScriptExecutionScope scope); return runtime.Cancel(scope, m_owner, handle); }
    [ApiContract(Call = "live")]
    public void Publish<T>(T value)
    { IScriptJobsRuntime runtime = Runtime(out ScriptExecutionScope scope); runtime.Publish(scope, typeof(T), value); }
}

internal interface IScriptJobsRuntime
{
    JobHandle Schedule(ScriptExecutionScope scope, ScriptBinding receiver, Action? action, Action<float>? tween, float delay, float interval, uint kind,
        bool unscaled, JobEase ease, float from, float to);
    bool Cancel(ScriptExecutionScope scope, ScriptBinding receiver, JobHandle handle);
    void CancelAll(ScriptExecutionScope scope, ScriptBinding receiver);
    JobHandle Run(ScriptExecutionScope scope, ScriptBinding receiver, IEnumerator<Wait> routine);
    JobHandle Subscribe(ScriptExecutionScope scope, ScriptBinding receiver, Type type, Delegate handler);
    void Publish(ScriptExecutionScope scope, Type type, object? value);
}
