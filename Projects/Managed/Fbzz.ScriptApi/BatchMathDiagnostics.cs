/// @file    BatchMathDiagnostics.cs
/// @brief   Opt-in bounded observations of completed four-agent rotation calls without changing public math contracts.
/// @author  Hasegawa Jin
/// @date    2026-10-20
using System;
using System.Diagnostics;
using System.Threading;
using System.Runtime.Intrinsics;

namespace Fbzz;

internal enum BatchMathOperation { LOOK_ROTATION, SLERP }

/// @note Raw Stopwatch ticks are Windows QPC; parent callback time and these child intervals must never be added.
internal readonly record struct BatchMathEvent(long StartQpc, long EndQpc, BatchMathOperation Operation, int Items,
    int GroupsOf4, int HardwareGroupsOf4, int TailItems, int ThreadId, ulong Generation, ulong InstanceId,
    ulong OwnerId, ulong CallId, ScriptCallback Callback);

/// @note Allocation occurs only when the Host explicitly creates a diagnostic session; a full buffer invalidates the diagnostic.
internal sealed class BatchMathCapture
{
    readonly BatchMathEvent[] m_events;
    readonly int[] m_published;
    int m_claimed, m_dropped;
    internal BatchMathCapture(int capacity)
    {
        if (capacity < 1) throw new ArgumentOutOfRangeException(nameof(capacity));
        m_events = new BatchMathEvent[capacity]; m_published = new int[capacity];
    }
    internal int Capacity => m_events.Length;
    internal int Count => Math.Min(Volatile.Read(ref m_claimed), Capacity);
    internal int Dropped => Volatile.Read(ref m_dropped);
    internal bool TryRead(int index, out BatchMathEvent value)
    {
        if ((uint)index >= (uint)Count || Volatile.Read(ref m_published[index]) == 0) { value = default; return false; }
        value = m_events[index]; return true;
    }
    internal void Complete(BatchMathOperation operation, int count, long start)
    {
        long end = Stopwatch.GetTimestamp();
        int index = Interlocked.Increment(ref m_claimed) - 1;
        if ((uint)index >= (uint)Capacity) { Interlocked.Increment(ref m_dropped); return; }
        ScriptExecutionScope? scope = ScriptApiRuntime.Current;
        m_events[index] = new(start, end, operation, count, count / 4,
            Vector128.IsHardwareAccelerated ? count / 4 : 0, count % 4, Environment.CurrentManagedThreadId,
            scope?.Generation ?? 0, scope?.InstanceId ?? 0, scope?.OwnerId ?? 0, scope?.CallId ?? 0,
            scope?.Callback ?? default);
        Volatile.Write(ref m_published[index], 1);
        BatchMathDiagnostics.AdvanceThreadSerial();
    }
}

/// @note The ordinary path reads one null reference; it never reads a clock, samples GC, allocates or writes a counter.
internal static class BatchMathDiagnostics
{
    static BatchMathCapture? s_capture;
    [ThreadStatic] static long s_threadSerial;
    internal static BatchMathCapture? Capture => Volatile.Read(ref s_capture);
    internal static long ThreadSerial => s_threadSerial;
    internal static void AdvanceThreadSerial() => ++s_threadSerial;
    internal static bool TryStart(BatchMathCapture capture) => Interlocked.CompareExchange(ref s_capture, capture, null) is null;
    internal static void Stop(BatchMathCapture capture) => Interlocked.CompareExchange(ref s_capture, null, capture);
}
