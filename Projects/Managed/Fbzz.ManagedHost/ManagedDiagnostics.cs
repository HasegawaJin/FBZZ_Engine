/// @file    ManagedDiagnostics.cs
/// @brief   Opt-in QPC-stamped SIMD/GC evidence and actual managed assembly identity without native ABI changes.
/// @author  Hasegawa Jin
/// @date    2026-10-20
using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading;
using Fbzz;

namespace Fbzz.ManagedHost;

internal readonly record struct ManagedGcSample(long BeforeQpc, long AfterQpc, long TotalAllocatedBytesApprox,
    int Gen0, int Gen1, int Gen2, long TotalPauseTimeSpanTicks);
internal readonly record struct ManagedCallbackStart(long Qpc, long MathSerial);
internal readonly record struct ManagedCallbackSample(long StartQpc, long EndQpc, ulong Generation, ulong InstanceId,
    ulong OwnerId, ulong CallId, ScriptCallback Callback, string TypeName, bool Success, long MathCalls,
    ManagedGcSample Gc);
internal readonly record struct ManagedAssemblyEvidence(string Role, ulong Generation, string FullName,
    string Location, string Mvid, string Sha256);

/// @note Samples are process-wide cumulative observations, not GC-event timestamps or thread-running measurements.
/// @see https://learn.microsoft.com/en-us/dotnet/api/system.gc.gettotalpauseduration?view=net-10.0 Runtime GC pause total.
/// @see https://learn.microsoft.com/en-us/dotnet/api/system.gc.gettotalallocatedbytes?view=net-10.0 Approximate allocation excludes native memory.
internal sealed class ManagedDiagnosticSession
{
    readonly string m_output;
    readonly BatchMathCapture m_math;
    readonly ManagedCallbackSample[] m_callbacks;
    readonly int[] m_published;
    readonly ManagedAssemblyEvidence[] m_assemblies = new ManagedAssemblyEvidence[64];
    readonly object m_writeGate = new();
    readonly ManagedGcSample m_initial;
    int m_claimed, m_dropped, m_assemblyCount, m_identityErrors;
    long m_lastHeartbeat;
    internal ManagedDiagnosticSession(string output, int capacity)
    {
        if (capacity < 1024 || capacity > 2097152) throw new ArgumentOutOfRangeException(nameof(capacity));
        m_output = Path.GetFullPath(output);
        m_math = new(capacity); m_callbacks = new ManagedCallbackSample[Math.Max(1024, capacity / 2)];
        m_published = new int[m_callbacks.Length]; m_initial = SampleGc();
    }
    internal BatchMathCapture Batch => m_math;
    internal static ManagedGcSample SampleGc()
    {
        long before = Stopwatch.GetTimestamp();
        long allocated = GC.GetTotalAllocatedBytes(precise: false);
        int gen0 = GC.CollectionCount(0), gen1 = GC.CollectionCount(1), gen2 = GC.CollectionCount(2);
        long pause = GC.GetTotalPauseDuration().Ticks;
        return new(before, Stopwatch.GetTimestamp(), allocated, gen0, gen1, gen2, pause);
    }
    internal ManagedCallbackStart BeginCallback() => new(Stopwatch.GetTimestamp(), BatchMathDiagnostics.ThreadSerial);
    internal void EndCallback(ManagedCallbackStart start, ulong generation, ulong instanceId, ulong ownerId,
        ulong callId, ScriptCallback callback, string typeName, bool success)
    {
        long end = Stopwatch.GetTimestamp(), calls = BatchMathDiagnostics.ThreadSerial - start.MathSerial;
        bool heartbeat = end - m_lastHeartbeat >= Stopwatch.Frequency / 10;
        if (calls == 0 && !heartbeat) return;
        m_lastHeartbeat = end;
        int index = Interlocked.Increment(ref m_claimed) - 1;
        if ((uint)index >= (uint)m_callbacks.Length) { Interlocked.Increment(ref m_dropped); return; }
        m_callbacks[index] = new(start.Qpc, end, generation, instanceId, ownerId, callId, callback,
            typeName, success, calls, SampleGc());
        Volatile.Write(ref m_published[index], 1);
    }
    internal void RegisterAssembly(string role, ulong generation, Assembly assembly)
    {
        lock (m_writeGate)
        {
            string mvid = assembly.ManifestModule.ModuleVersionId.ToString("D");
            for (int i = 0; i < m_assemblyCount; ++i)
                if (m_assemblies[i].Role == role && m_assemblies[i].Generation == generation && m_assemblies[i].Mvid == mvid) return;
            if (m_assemblyCount == m_assemblies.Length) { ++m_identityErrors; return; }
            try
            {
                string location = assembly.Location;
                if (location.Length == 0) { ++m_identityErrors; return; }
                using FileStream stream = File.OpenRead(location);
                string digest = Convert.ToHexString(SHA256.HashData(stream));
                m_assemblies[m_assemblyCount++] = new(role, generation, assembly.FullName ?? "", location, mvid, digest);
            }
            catch (Exception) { ++m_identityErrors; }
        }
    }
    /// @note Writing is a diagnostic checkpoint outside user callback execution; its allocations are outside the selected windows and serialization is QPC-stamped.
    internal void Write(string reason)
    {
        lock (m_writeGate)
        {
            long writeStart = Stopwatch.GetTimestamp();
            ManagedGcSample final = SampleGc();
            int batchCount = m_math.Count, callbackCount = Math.Min(Volatile.Read(ref m_claimed), m_callbacks.Length);
            int batchDropped = m_math.Dropped, callbackDropped = Volatile.Read(ref m_dropped), incompleteMath = 0, incompleteCallbacks = 0;
            for (int i = 0; i < batchCount; ++i) if (!m_math.TryRead(i, out _)) ++incompleteMath;
            for (int i = 0; i < callbackCount; ++i) if (Volatile.Read(ref m_published[i]) == 0) ++incompleteCallbacks;
            string? directory = Path.GetDirectoryName(m_output);
            if (directory is not null) Directory.CreateDirectory(directory);
            string pending = m_output + ".pending";
            using (FileStream file = new(pending, FileMode.Create, FileAccess.Write, FileShare.None))
            using (Utf8JsonWriter w = new(file))
            {
                w.WriteStartObject(); w.WriteNumber("schemaVersion", 1);
                w.WriteBoolean("diagnosticOnly", true); w.WriteBoolean("formalPoolingAllowed", false);
                w.WriteString("checkpoint", reason); w.WriteNumber("processId", Environment.ProcessId);
                w.WriteNumber("qpcFrequency", Stopwatch.Frequency); w.WriteString("qpcClock", "Stopwatch.GetTimestamp; Windows QPC");
                w.WriteString("joinPolicy", "Select raw QPC intervals offline. GC cumulative observations have bracketed sample boundaries, not exact GC event times. Never label process totals as a 6000-frame measurement.");
                w.WriteString("timePolicy", "Batch intervals include pack/math/scatter after validation. Callback elapsed includes child math; never add parent and child durations. No CPU-running attribution.");
                w.WriteString("allocationPolicy", "GetTotalAllocatedBytes(false) is approximate process managed allocation only; JSON checkpoint/startup allocation is outside selected windows.");
                w.WriteString("batchPolicy", "Completed valid calls only; zero-item calls are included. GroupsOf4 is software or hardware four-agent processing; hardwareGroupsOf4 identifies hardware support.");
                w.WriteNumber("batchCapacity", m_math.Capacity); w.WriteNumber("callbackCapacity", m_callbacks.Length);
                w.WriteNumber("batchSnapshotCount", batchCount); w.WriteNumber("callbackSnapshotCount", callbackCount);
                w.WriteNumber("droppedBatchRecords", batchDropped); w.WriteNumber("droppedCallbackRecords", callbackDropped);
                w.WriteNumber("incompleteBatchRecords", incompleteMath); w.WriteNumber("incompleteCallbackRecords", incompleteCallbacks);
                w.WriteNumber("assemblyIdentityErrors", m_identityErrors);
                w.WriteBoolean("hardwareAccelerated", System.Runtime.Intrinsics.Vector128.IsHardwareAccelerated);
                w.WriteString("dotnetEnableHwIntrinsic", Environment.GetEnvironmentVariable("DOTNET_EnableHWIntrinsic"));
                WriteGc(w, "initialGc", m_initial); WriteGc(w, "checkpointGc", final);
                w.WriteStartArray("assemblies");
                for (int i = 0; i < m_assemblyCount; ++i)
                {
                    var a = m_assemblies[i]; w.WriteStartObject(); w.WriteString("role", a.Role); w.WriteNumber("generation", a.Generation);
                    w.WriteString("fullName", a.FullName); w.WriteString("location", a.Location); w.WriteString("mvid", a.Mvid); w.WriteString("sha256", a.Sha256); w.WriteEndObject();
                }
                w.WriteEndArray(); w.WriteStartArray("batchEvents");
                for (int i = 0; i < batchCount; ++i)
                {
                    if (!m_math.TryRead(i, out BatchMathEvent e)) continue;
                    w.WriteStartObject(); w.WriteNumber("sequence", i); w.WriteNumber("startQpc", e.StartQpc); w.WriteNumber("endQpc", e.EndQpc);
                    w.WriteString("operation", e.Operation.ToString()); w.WriteNumber("items", e.Items); w.WriteNumber("groupsOf4", e.GroupsOf4);
                    w.WriteNumber("hardwareGroupsOf4", e.HardwareGroupsOf4); w.WriteNumber("tailItems", e.TailItems); w.WriteNumber("threadId", e.ThreadId);
                    WriteScope(w, e.Generation, e.InstanceId, e.OwnerId, e.CallId, e.Callback); w.WriteEndObject();
                }
                w.WriteEndArray(); w.WriteStartArray("callbackSamples");
                for (int i = 0; i < callbackCount; ++i)
                {
                    if (Volatile.Read(ref m_published[i]) == 0) continue;
                    var e = m_callbacks[i]; w.WriteStartObject(); w.WriteNumber("sequence", i); w.WriteNumber("startQpc", e.StartQpc); w.WriteNumber("endQpc", e.EndQpc);
                    WriteScope(w, e.Generation, e.InstanceId, e.OwnerId, e.CallId, e.Callback); w.WriteString("typeName", e.TypeName);
                    w.WriteBoolean("success", e.Success); w.WriteNumber("completedMathCalls", e.MathCalls); WriteGc(w, "gc", e.Gc); w.WriteEndObject();
                }
                w.WriteEndArray();
                bool changed = m_math.Count != batchCount || Math.Min(Volatile.Read(ref m_claimed), m_callbacks.Length) != callbackCount
                    || m_math.Dropped != batchDropped || Volatile.Read(ref m_dropped) != callbackDropped;
                w.WriteBoolean("recordingChangedDuringSerialization", changed);
                w.WriteBoolean("valid", !changed && batchDropped == 0 && callbackDropped == 0 && incompleteMath == 0 && incompleteCallbacks == 0 && m_identityErrors == 0);
                w.WriteNumber("checkpointWriteStartedQpc", writeStart); w.WriteNumber("checkpointSerializationFinishedQpc", Stopwatch.GetTimestamp()); w.WriteBoolean("checkpointIoCompletionAfterSerializedTimestamp", true);
                w.WriteEndObject(); w.Flush();
            }
            File.Move(pending, m_output, overwrite: true);
        }
    }
    static void WriteScope(Utf8JsonWriter w, ulong generation, ulong instance, ulong owner, ulong call, ScriptCallback callback)
    {
        w.WriteNumber("generation", generation); w.WriteNumber("instanceId", instance); w.WriteNumber("ownerId", owner); w.WriteNumber("callId", call); w.WriteString("callback", callback.ToString());
    }
    static void WriteGc(Utf8JsonWriter w, string name, ManagedGcSample sample)
    {
        w.WriteStartObject(name); w.WriteNumber("beforeQpc", sample.BeforeQpc); w.WriteNumber("afterQpc", sample.AfterQpc);
        w.WriteNumber("totalAllocatedBytesApprox", sample.TotalAllocatedBytesApprox); w.WriteNumber("gen0", sample.Gen0);
        w.WriteNumber("gen1", sample.Gen1); w.WriteNumber("gen2", sample.Gen2); w.WriteNumber("totalPauseTimeSpanTicks", sample.TotalPauseTimeSpanTicks); w.WriteEndObject();
    }
}

internal static class ManagedDiagnostics
{
    static bool s_checked;
    static ManagedDiagnosticSession? s_session;
    internal static bool Enabled => s_session is not null;
    internal static void Initialize()
    {
        if (s_checked) return;
        s_checked = true;
        string? output = Environment.GetEnvironmentVariable("FBZZ_MANAGED_DIAGNOSTICS_OUTPUT");
        if (string.IsNullOrWhiteSpace(output)) return;
        ManagedDiagnosticSession? started = null;
        try
        {
            string? capacityText = Environment.GetEnvironmentVariable("FBZZ_MANAGED_DIAGNOSTICS_CAPACITY");
            int capacity = string.IsNullOrEmpty(capacityText) ? 1048576 : int.Parse(capacityText, System.Globalization.CultureInfo.InvariantCulture);
            ManagedDiagnosticSession session = new(output, capacity);
            if (!BatchMathDiagnostics.TryStart(session.Batch)) return;
            started = session; s_session = session;
            session.RegisterAssembly("api", 0, typeof(Script).Assembly); session.RegisterAssembly("host", 0, typeof(EntryPoint).Assembly);
            AppDomain.CurrentDomain.ProcessExit += static (_, _) => Checkpoint("process-exit");
        }
        catch (Exception) { if (started is not null) BatchMathDiagnostics.Stop(started.Batch); s_session = null; }
    }
    internal static void RegisterGame(Assembly assembly, ulong generation) => s_session?.RegisterAssembly("game", generation, assembly);
    internal static ManagedCallbackStart BeginCallback() => s_session!.BeginCallback();
    internal static void EndCallback(ManagedCallbackStart start, ulong generation, ulong instance, ulong owner, ulong call,
        ScriptCallback callback, string typeName, bool success) => s_session?.EndCallback(start, generation, instance, owner, call, callback, typeName, success);
    internal static void Checkpoint(string reason)
    {
        try { s_session?.Write(reason); }
        catch (Exception) { }
    }
}
