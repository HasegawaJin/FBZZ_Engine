/// @file    AiBehaviorTree.cs
/// @brief   Shareable script behavior definitions with typed, private agent state and bounded evaluation frequency.
/// @author  Hasegawa Jin
/// @date    2026-10-11
using System;
using System.Collections.Generic;
using System.Runtime.ExceptionServices;

namespace Fbzz.AI;

public enum BtStatus { IDLE, RUNNING, SUCCESS, FAILURE }
public enum BtExitReason { SUCCEEDED, FAILED, ABORTED, RESET, DISPOSED, FAULTED }

/// @note Keys belong to exactly one schema. A default key is invalid; names are for authoring and diagnostics only.
public readonly struct BlackboardKey<T>
{
    internal readonly object? Owner;
    internal readonly int Index;
    internal BlackboardKey(object owner, int index) { Owner = owner; Index = index; }
}

/// @note The layout is immutable after Builder.Build. Reference defaults are shared references; put mutable agent objects in each Blackboard explicitly.
public sealed class BlackboardSchema
{
    readonly object m_owner;
    readonly IKeyDefinition[] m_keys;
    internal BlackboardSchema(object owner, IKeyDefinition[] keys) { m_owner = owner; m_keys = keys; }

    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public int Count => m_keys.Length;

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Lifetime = "caller_owned_independent_state")]
    public Blackboard CreateBlackboard()
    {
        object[] slots = new object[m_keys.Length];
        for (int i = 0; i < slots.Length; ++i) slots[i] = m_keys[i].CreateSlot();
        return new(m_owner, slots);
    }

    public sealed class Builder
    {
        readonly object m_owner = new();
        readonly List<IKeyDefinition> m_keys = new();
        readonly HashSet<string> m_names = new(StringComparer.Ordinal);
        BlackboardSchema? m_built;

        [ApiContract(Purity = ApiPurity.MANAGED, Call = "any")]
        public Builder() { }

        [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "built_schema_or_duplicate_empty_name_throws")]
        public BlackboardKey<T> Add<T>(string name, T defaultValue = default!)
        {
            if (m_built is not null) throw new InvalidOperationException("The blackboard schema is already built.");
            if (string.IsNullOrWhiteSpace(name) || !m_names.Add(name)) throw new ArgumentException("Blackboard key names must be nonempty and unique.", nameof(name));
            int index = m_keys.Count;
            m_keys.Add(new KeyDefinition<T>(defaultValue));
            return new(m_owner, index);
        }

        [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Returns = "immutable_shared_schema")]
        public BlackboardSchema Build() => m_built ??= new(m_owner, m_keys.ToArray());
    }

    internal interface IKeyDefinition { object CreateSlot(); }
    sealed class KeyDefinition<T>(T defaultValue) : IKeyDefinition
    {
        public object CreateSlot() => new BlackboardSlot<T>(defaultValue);
    }
}

internal sealed class BlackboardSlot<T>(T value) { internal T Value = value; }

/// @note Single-thread use. Typed slots avoid string lookup and scalar storage boxing. Custom value types should implement IEquatable&lt;T&gt; for allocation-free change detection.
public sealed class Blackboard
{
    readonly object m_owner;
    readonly object[] m_slots;
    internal ulong Revision { get; private set; }
    internal Blackboard(object owner, object[] slots) { m_owner = owner; m_slots = slots; }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "default_or_foreign_schema_key_throws")]
    public T Get<T>(BlackboardKey<T> key) => Slot(key).Value;

    /// @note Changed decision keys wake the next Tick. Use requestEvaluation=false for frequently refreshed data that should respect the decision interval.
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "default_or_foreign_schema_key_throws", Returns = "whether_value_changed")]
    public bool Set<T>(BlackboardKey<T> key, T value, bool requestEvaluation = true)
    {
        BlackboardSlot<T> slot = Slot(key);
        if (EqualityComparer<T>.Default.Equals(slot.Value, value)) return false;
        slot.Value = value;
        if (requestEvaluation) Revision = unchecked(Revision + 1);
        return true;
    }

    BlackboardSlot<T> Slot<T>(BlackboardKey<T> key)
    {
        if (!ReferenceEquals(m_owner, key.Owner) || (uint)key.Index >= (uint)m_slots.Length)
            throw new ArgumentException("The key does not belong to this blackboard.", nameof(key));
        return (BlackboardSlot<T>)m_slots[key.Index];
    }
}

/// @note Context is a synchronous value snapshot. DeltaSeconds includes skipped evaluations; TimeSeconds uses the caller's Tick clock.
public readonly struct BtContext
{
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Lifetime = "agent_instance")]
    public Blackboard Blackboard { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "seconds")]
    public double DeltaSeconds { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "seconds")]
    public double TimeSeconds { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public int NodeIndex { get; }
    internal BtContext(Blackboard blackboard, double deltaSeconds, double timeSeconds, int nodeIndex)
    { Blackboard = blackboard; DeltaSeconds = deltaSeconds; TimeSeconds = timeSeconds; NodeIndex = nodeIndex; }
}

internal enum BtNodeKind { SEQUENCE, SELECTOR, REACTIVE_SELECTOR, CONDITION, ACTION, WAIT, GUARD }

/// @note Definition data has no cursor, timer or individual agent state. Shared callbacks must read individual state from BtContext.
public sealed class BtNode
{
    internal readonly BtNodeKind Kind;
    internal readonly BtNode[] Children;
    internal readonly Func<BtContext, bool>? Predicate;
    internal readonly Func<BtContext, BtStatus>? Execute;
    internal readonly Action<BtContext, BtExitReason>? Exit;
    internal readonly double Seconds;
    internal readonly string Name;
    internal BtNode(BtNodeKind kind, string name, BtNode[]? children = null,
        Func<BtContext, bool>? predicate = null, Func<BtContext, BtStatus>? execute = null,
        Action<BtContext, BtExitReason>? exit = null, double seconds = 0)
    { Kind = kind; Name = string.IsNullOrWhiteSpace(name) ? kind.ToString() : name; Children = children ?? Array.Empty<BtNode>(); Predicate = predicate; Execute = execute; Exit = exit; Seconds = seconds; }
}

public static class Bt
{
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any")]
    public static BtNode Sequence(params BtNode[] children) => Composite(BtNodeKind.SEQUENCE, children);
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any")]
    public static BtNode Selector(params BtNode[] children) => Composite(BtNodeKind.SELECTOR, children);

    /// @note Rechecks higher branches on each scheduled/event evaluation. Put a Guard before effectful higher-priority work; entering a new action aborts the old action first.
    /// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/behavior-tree-in-unreal-engine---overview
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any")]
    public static BtNode ReactiveSelector(params BtNode[] children) => Composite(BtNodeKind.REACTIVE_SELECTOR, children);

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "null_or_async_callback_throws")]
    public static BtNode Condition([SynchronousCallback] Func<BtContext, bool> predicate, string name = "Condition")
    {
        ArgumentNullException.ThrowIfNull(predicate);
        ScriptCallbackSafety.RequireSynchronous(predicate, nameof(predicate));
        return new(BtNodeKind.CONDITION, name, predicate: predicate);
    }

    /// @note Exit runs once for a started action, including its terminal result or abort. IDLE is not a valid callback result. Exceptions reach the enclosing Script callback after cleanup.
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "null_or_async_callback_throws")]
    public static BtNode Action([SynchronousCallback] Func<BtContext, BtStatus> execute,
        [SynchronousCallback] Action<BtContext, BtExitReason>? onExit = null, string name = "Action")
    {
        ArgumentNullException.ThrowIfNull(execute);
        ScriptCallbackSafety.RequireSynchronous(execute, nameof(execute));
        ScriptCallbackSafety.RequireSynchronous(onExit, nameof(onExit));
        return new(BtNodeKind.ACTION, name, execute: execute, exit: onExit);
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", ParameterUnits = new[] { "seconds=seconds" }, Failure = "nonfinite_or_negative_seconds_throws")]
    public static BtNode Wait(double seconds, string name = "Wait")
    {
        if (!double.IsFinite(seconds) || seconds < 0) throw new ArgumentOutOfRangeException(nameof(seconds));
        return new(BtNodeKind.WAIT, name, seconds: seconds);
    }

    /// @note Guard is reevaluated while its branch runs; false aborts the child before returning FAILURE. Sequence Condition leaves, by contrast, are remembered after success.
    /// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-behavior-tree-node-reference-decorators
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "null_or_async_callback_throws")]
    public static BtNode Guard([SynchronousCallback] Func<BtContext, bool> predicate, BtNode child, string name = "Guard")
    {
        ArgumentNullException.ThrowIfNull(predicate);
        ArgumentNullException.ThrowIfNull(child);
        ScriptCallbackSafety.RequireSynchronous(predicate, nameof(predicate));
        return new(BtNodeKind.GUARD, name, new[] { child }, predicate: predicate);
    }

    static BtNode Composite(BtNodeKind kind, BtNode[] children)
    {
        ArgumentNullException.ThrowIfNull(children);
        BtNode[] copy = (BtNode[])children.Clone();
        foreach (BtNode child in copy) ArgumentNullException.ThrowIfNull(child);
        return new(kind, kind.ToString(), copy);
    }
}

/// @note Build once per behavior definition, then create one instance per agent. Reused nodes compile to distinct occurrences and therefore distinct timer/cursor slots.
public sealed class BehaviorTree
{
    const int MAX_DEPTH = 128;
    const int MAX_NODE_OCCURRENCES = 4096;
    readonly BlackboardSchema m_schema;
    internal readonly CompiledNode[] Nodes;

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "null_or_excessively_deep_large_tree_throws")]
    public BehaviorTree(BlackboardSchema schema, BtNode root)
    {
        ArgumentNullException.ThrowIfNull(schema);
        ArgumentNullException.ThrowIfNull(root);
        m_schema = schema;
        List<CompiledNode> nodes = new();
        Compile(root, nodes, 0, -1);
        Nodes = nodes.ToArray();
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public int NodeCount => Nodes.Length;
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "invalid_node_index_throws")]
    public string GetNodeName(int nodeIndex)
    {
        if ((uint)nodeIndex >= (uint)Nodes.Length) throw new ArgumentOutOfRangeException(nameof(nodeIndex));
        return Nodes[nodeIndex].Definition.Name;
    }
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", ParameterUnits = new[] { "evaluationIntervalSeconds=seconds" }, Lifetime = "caller_owned_disposable_agent_state", Failure = "nonfinite_or_negative_interval_throws")]
    public BehaviorTreeInstance CreateInstance(double evaluationIntervalSeconds = 0.1) => new(this, m_schema.CreateBlackboard(), evaluationIntervalSeconds);

    static int Compile(BtNode node, List<CompiledNode> nodes, int depth, int parent)
    {
        if (depth >= MAX_DEPTH || nodes.Count >= MAX_NODE_OCCURRENCES)
            throw new ArgumentException("Behavior trees are limited to 128 levels and 4096 node occurrences.");
        int index = nodes.Count;
        int[] children = new int[node.Children.Length];
        nodes.Add(default);
        for (int i = 0; i < children.Length; ++i) children[i] = Compile(node.Children[i], nodes, depth + 1, index);
        nodes[index] = new(node, children, nodes.Count, parent);
        return index;
    }

    internal readonly struct CompiledNode(BtNode definition, int[] children, int end, int parent)
    {
        internal readonly BtNode Definition = definition;
        internal readonly int[] Children = children;
        internal readonly int End = end;
        internal readonly int Parent = parent;
    }
}

/// @note Single-thread, caller-driven companion to the native BT. No native call or scheduler registration occurs here. Dispose from the owning Script lifecycle; Reset preserves Blackboard values.
public sealed class BehaviorTreeInstance : IDisposable
{
    BehaviorTree? m_tree;
    readonly int[] m_cursors;
    readonly bool[] m_active;
    readonly double[] m_waitStartedAt;
    double m_interval;
    double m_clock;
    double m_elapsed;
    double m_evaluationDelta;
    ulong m_seenRevision;
    bool m_requested = true;
    bool m_busy;
    bool m_disposed;
    int m_runningLeaf = -1;

    internal BehaviorTreeInstance(BehaviorTree tree, Blackboard blackboard, double interval)
    {
        ValidateInterval(interval);
        m_tree = tree;
        Blackboard = blackboard;
        m_interval = interval;
        m_cursors = new int[tree.NodeCount];
        m_active = new bool[tree.NodeCount];
        m_waitStartedAt = new double[tree.NodeCount];
        Array.Fill(m_cursors, -1);
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Lifetime = "agent_instance")]
    public Blackboard Blackboard { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public BtStatus LastStatus { get; private set; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public bool IsFaulted { get; private set; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Returns = "running_action_or_wait_index; minus_one_if_none")]
    public int ActiveNodeIndex => m_runningLeaf;
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public ulong EvaluationCount { get; private set; }

    /// @note Zero evaluates on every Tick. A changed decision key or RequestEvaluation bypasses a positive interval; no automatic thread or timer is created.
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Write = "any", Units = "seconds", Failure = "disposed_or_nonfinite_negative_interval_throws")]
    public double EvaluationIntervalSeconds
    {
        get => m_interval;
        set { RequireLive(); ValidateInterval(value); m_interval = value; }
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "disposed_instance_throws")]
    public void RequestEvaluation() { RequireLive(); m_requested = true; }

    /// @note Call once per agent update with elapsed simulation seconds. A terminal root starts afresh at the next eligible evaluation. Reentrant Tick/Reset/Dispose is rejected; callback events queue for the next Tick.
    /// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/behavior-tree-in-unreal-engine---overview
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", ParameterUnits = new[] { "deltaSeconds=seconds" }, Failure = "invalid_delta_disposed_faulted_reentrant_throws; callback_fault_aborts_then_rethrows")]
    public BtStatus Tick(double deltaSeconds)
    {
        RequireLive();
        RequireIdle();
        if (IsFaulted) throw new InvalidOperationException("Reset the faulted behavior instance before ticking it again.");
        if (!double.IsFinite(deltaSeconds) || deltaSeconds < 0 || !double.IsFinite(m_clock + deltaSeconds))
            throw new ArgumentOutOfRangeException(nameof(deltaSeconds));
        m_clock += deltaSeconds;
        m_elapsed += deltaSeconds;
        /// @note Keep an exact interval boundary from slipping a frame due to accumulated double rounding.
        bool beforeInterval = m_elapsed < m_interval && m_interval - m_elapsed > m_interval * 1e-12;
        if (!m_requested && m_seenRevision == Blackboard.Revision && beforeInterval) return LastStatus;
        m_busy = true;
        m_requested = false;
        m_seenRevision = Blackboard.Revision;
        m_evaluationDelta = m_elapsed;
        m_elapsed = 0;
        EvaluationCount = unchecked(EvaluationCount + 1);
        try { return LastStatus = Evaluate(0); }
        catch (Exception exception)
        {
            IsFaulted = true;
            Exception? cleanup = AbortRange(0, BtExitReason.FAULTED);
            ClearRuntime();
            LastStatus = BtStatus.FAILURE;
            if (cleanup is not null)
            {
                /// @note Custom exceptions can have unwritable Data; preserve the original action fault in that case too.
                try { exception.Data["Fbzz.AI.CleanupFailure"] = cleanup; }
                catch (Exception) { }
            }
            throw;
        }
        finally { m_busy = false; }
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "disposed_reentrant_throws; cleanup_fault_rethrows_after_reset")]
    public void Reset()
    {
        RequireLive();
        RequireIdle();
        m_busy = true;
        try
        {
            m_evaluationDelta = m_elapsed;
            Exception? failure = AbortRange(0, BtExitReason.RESET);
            ClearRuntime();
            m_clock = 0;
            m_elapsed = 0;
            m_evaluationDelta = 0;
            m_seenRevision = Blackboard.Revision;
            m_requested = true;
            LastStatus = BtStatus.IDLE;
            IsFaulted = failure is not null;
            if (failure is not null) ExceptionDispatchInfo.Capture(failure).Throw();
        }
        finally { m_busy = false; }
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "reentrant_throws; cleanup_fault_rethrows_after_dispose", Lifetime = "idempotent_disposal")]
    public void Dispose()
    {
        RequireIdle();
        if (m_disposed) return;
        m_busy = true;
        m_disposed = true;
        try
        {
            m_evaluationDelta = m_elapsed;
            Exception? failure = AbortRange(0, BtExitReason.DISPOSED);
            ClearRuntime();
            LastStatus = BtStatus.IDLE;
            if (failure is not null) ExceptionDispatchInfo.Capture(failure).Throw();
        }
        finally { m_tree = null; m_busy = false; }
    }

    BtContext Context(int index) => new(Blackboard, m_evaluationDelta, m_clock, index);

    BtStatus Evaluate(int index)
    {
        BehaviorTree.CompiledNode node = m_tree!.Nodes[index];
        switch (node.Definition.Kind)
        {
            case BtNodeKind.CONDITION: return node.Definition.Predicate!(Context(index)) ? BtStatus.SUCCESS : BtStatus.FAILURE;
            case BtNodeKind.ACTION:
                EnterLeaf(index);
                BtStatus result = node.Definition.Execute!(Context(index));
                if (result is not (BtStatus.RUNNING or BtStatus.SUCCESS or BtStatus.FAILURE))
                    throw new InvalidOperationException("Action callbacks must return RUNNING, SUCCESS or FAILURE.");
                if (result != BtStatus.RUNNING)
                {
                    m_active[index] = false;
                    m_runningLeaf = -1;
                    node.Definition.Exit?.Invoke(Context(index), result == BtStatus.SUCCESS ? BtExitReason.SUCCEEDED : BtExitReason.FAILED);
                }
                return result;
            case BtNodeKind.WAIT:
                if (!m_active[index]) { EnterLeaf(index); m_waitStartedAt[index] = m_clock; }
                if (m_clock - m_waitStartedAt[index] < node.Definition.Seconds) return BtStatus.RUNNING;
                m_active[index] = false;
                m_runningLeaf = -1;
                m_waitStartedAt[index] = 0;
                return BtStatus.SUCCESS;
            case BtNodeKind.GUARD:
                if (node.Definition.Predicate!(Context(index))) return Evaluate(node.Children[0]);
                ThrowCleanup(AbortRange(node.Children[0], BtExitReason.ABORTED));
                return BtStatus.FAILURE;
            default: return EvaluateComposite(index, node);
        }
    }

    BtStatus EvaluateComposite(int index, BehaviorTree.CompiledNode node)
    {
        int previous = m_cursors[index];
        bool sequence = node.Definition.Kind == BtNodeKind.SEQUENCE;
        int start = node.Definition.Kind == BtNodeKind.REACTIVE_SELECTOR ? 0 : Math.Max(0, previous);
        for (int i = start; i < node.Children.Length; ++i)
        {
            BtStatus result = Evaluate(node.Children[i]);
            if (result == BtStatus.RUNNING || (!sequence && result == BtStatus.SUCCESS))
            {
                if (previous >= 0 && previous != i) ThrowCleanup(AbortRange(node.Children[previous], BtExitReason.ABORTED));
                m_cursors[index] = result == BtStatus.RUNNING ? i : -1;
                return result;
            }
            if (sequence && result == BtStatus.FAILURE) { m_cursors[index] = -1; return result; }
        }
        m_cursors[index] = -1;
        return sequence ? BtStatus.SUCCESS : BtStatus.FAILURE;
    }

    void EnterLeaf(int index)
    {
        if (m_runningLeaf >= 0 && m_runningLeaf != index)
        {
            int root = m_runningLeaf;
            int parent = m_tree!.Nodes[root].Parent;
            while (parent >= 0 && (index < parent || index >= m_tree.Nodes[parent].End))
            { root = parent; parent = m_tree.Nodes[parent].Parent; }
            ThrowCleanup(AbortRange(root, BtExitReason.ABORTED));
        }
        m_active[index] = true;
        m_runningLeaf = index;
    }

    Exception? AbortRange(int root, BtExitReason reason)
    {
        Exception? firstFailure = null;
        int end = m_tree!.Nodes[root].End;
        for (int i = root; i < end; ++i)
        {
            bool wasActive = m_active[i];
            m_active[i] = false;
            m_cursors[i] = -1;
            m_waitStartedAt[i] = 0;
            if (m_runningLeaf == i) m_runningLeaf = -1;
            if (!wasActive || m_tree.Nodes[i].Definition.Exit is not Action<BtContext, BtExitReason> exit) continue;
            try { exit(Context(i), reason); }
            catch (Exception exception) { firstFailure ??= exception; }
        }
        return firstFailure;
    }

    void ClearRuntime() { Array.Fill(m_cursors, -1); Array.Clear(m_active); Array.Clear(m_waitStartedAt); m_runningLeaf = -1; }
    static void ThrowCleanup(Exception? exception) { if (exception is not null) ExceptionDispatchInfo.Capture(exception).Throw(); }
    static void ValidateInterval(double interval) { if (!double.IsFinite(interval) || interval < 0) throw new ArgumentOutOfRangeException(nameof(interval)); }
    void RequireLive() { ObjectDisposedException.ThrowIf(m_disposed, this); }
    void RequireIdle() { if (m_busy) throw new InvalidOperationException("Behavior tree callbacks cannot reenter Tick, Reset or Dispose."); }
}
