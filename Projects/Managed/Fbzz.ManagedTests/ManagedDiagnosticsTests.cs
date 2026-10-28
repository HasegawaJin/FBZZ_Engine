/// @file    ManagedDiagnosticsTests.cs
/// @brief   Verify ordinary-path allocation, real batch coverage, bounded overflow and GC/assembly evidence output.
/// @author  Hasegawa Jin
/// @date    2026-10-20
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class ManagedDiagnosticsTests
{
    internal static void Run()
    {
        DisabledBatchDoesNotAllocate();
        ActualCallsReportGroupsTailsAndOverflow();
        SessionWritesGcAndActualAssemblyIdentity();
    }
    static void DisabledBatchDoesNotAllocate()
    {
        Program.Check(BatchMathDiagnostics.Capture is null, "Ordinary tests must not have an active diagnostic capture.");
        Vector3[] directions = Enumerable.Repeat(Vector3.FORWARD, 5).ToArray();
        Quaternion[] output = new Quaternion[5], rotations = Enumerable.Repeat(Quaternion.IDENTITY, 5).ToArray();
        float[] factors = new float[5];
        for (int i = 0; i < 32; ++i) { QuaternionBatch.LookRotation(directions, [], output); QuaternionBatch.Slerp(rotations, rotations, factors, output); }
        long before = GC.GetAllocatedBytesForCurrentThread();
        for (int i = 0; i < 32; ++i) { QuaternionBatch.LookRotation(directions, [], output); QuaternionBatch.Slerp(rotations, rotations, factors, output); }
        long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
        Program.Check(allocated == 0 && BatchMathDiagnostics.Capture is null, "OFF batch calls must not allocate or create a diagnostic session.");
    }
    static void ActualCallsReportGroupsTailsAndOverflow()
    {
        BatchMathCapture capture = new(8);
        Program.Check(BatchMathDiagnostics.TryStart(capture), "A diagnostic owner must be exclusive.");
        try
        {
            Program.Check(!BatchMathDiagnostics.TryStart(new(1)), "A second owner must not replace an active capture.");
            Vector3[] forward = Enumerable.Repeat(Vector3.FORWARD, 5).ToArray();
            Quaternion[] output = new Quaternion[7], values = Enumerable.Repeat(Quaternion.IDENTITY, 7).ToArray();
            QuaternionBatch.LookRotation([], [], []);
            QuaternionBatch.LookRotation(forward, [], output);
            QuaternionBatch.Slerp(values, values, new float[7], output);
            Program.Throws<ArgumentException>(() => QuaternionBatch.LookRotation(forward, [], new Quaternion[4]));
            Program.Check(capture.Count == 3 && capture.Dropped == 0, "Only completed valid calls, including an empty call, are recorded.");
            Program.Check(capture.TryRead(0, out BatchMathEvent empty) && empty.Items == 0 && empty.GroupsOf4 == 0 && empty.TailItems == 0, "Zero-item coverage must be explicit.");
            Program.Check(capture.TryRead(1, out BatchMathEvent look) && look.Operation == BatchMathOperation.LOOK_ROTATION && look.Items == 5 && look.GroupsOf4 == 1 && look.TailItems == 1, "Look coverage must come from the actual executed call.");
            Program.Check(capture.TryRead(2, out BatchMathEvent slerp) && slerp.Operation == BatchMathOperation.SLERP && slerp.Items == 7 && slerp.GroupsOf4 == 1 && slerp.TailItems == 3, "Slerp must count its own tail independently.");
            Program.Check(look.EndQpc >= look.StartQpc && slerp.EndQpc >= slerp.StartQpc && look.HardwareGroupsOf4 == (System.Runtime.Intrinsics.Vector128.IsHardwareAccelerated ? 1 : 0), "QPC ordering and hardware/software coverage must be explicit.");
            Program.Check(output.All(q => q.X == 0 && q.Y == 0 && q.Z == 0 && q.W == 1), "Diagnostics must preserve computed rotations.");
        }
        finally { BatchMathDiagnostics.Stop(capture); }
        BatchMathCapture bounded = new(1);
        Program.Check(BatchMathDiagnostics.TryStart(bounded), "The previous owner must be detached.");
        try { QuaternionBatch.LookRotation([], [], []); QuaternionBatch.LookRotation([], [], []); }
        finally { BatchMathDiagnostics.Stop(bounded); }
        Program.Check(bounded.Count == 1 && bounded.Dropped == 1 && bounded.TryRead(0, out _), "Overflow must be visible without overwriting captured rows.");
    }
    static void SessionWritesGcAndActualAssemblyIdentity()
    {
        string directory = Path.Combine(Path.GetTempPath(), "FbzzManagedDiagnostics-" + Guid.NewGuid().ToString("N"));
        string path = Path.Combine(directory, "diagnostics.json");
        ManagedDiagnosticSession session = new(path, 1024);
        session.RegisterAssembly("api", 0, typeof(Script).Assembly);
        session.RegisterAssembly("host", 0, typeof(EntryPoint).Assembly);
        session.RegisterAssembly("game", 42, typeof(ManagedDiagnosticsTests).Assembly);
        Program.Check(BatchMathDiagnostics.TryStart(session.Batch), "A session must own its real math capture.");
        try
        {
            ManagedCallbackStart start = session.BeginCallback();
            QuaternionBatch.LookRotation(new[] { Vector3.FORWARD, Vector3.RIGHT, -Vector3.FORWARD, Vector3.UP, Vector3.ZERO }, [], new Quaternion[5]);
            session.EndCallback(start, 42, 43, 44, 45, ScriptCallback.LATE_UPDATE, "DiagnosticCohort", true);
            session.Write("test-checkpoint");
            using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(path));
            JsonElement root = document.RootElement;
            Program.Check(root.GetProperty("valid").GetBoolean() && root.GetProperty("diagnosticOnly").GetBoolean() && !root.GetProperty("formalPoolingAllowed").GetBoolean(), "A diagnostic must not masquerade as formal FPS evidence.");
            Program.Check(root.GetProperty("qpcFrequency").GetInt64() == Stopwatch.Frequency && root.GetProperty("batchEvents").GetArrayLength() == root.GetProperty("batchSnapshotCount").GetInt32() && root.GetProperty("batchSnapshotCount").GetInt32() == 1 && !root.GetProperty("recordingChangedDuringSerialization").GetBoolean(), "Export must retain one stable QPC-stamped prefix and reject concurrent additions.");
            JsonElement sample = root.GetProperty("callbackSamples")[0];
            Program.Check(sample.GetProperty("generation").GetUInt64() == 42 && sample.GetProperty("completedMathCalls").GetInt64() == 1 && sample.GetProperty("typeName").GetString() == "DiagnosticCohort", "Callback identity must join the math and GC observation.");
            JsonElement gc = sample.GetProperty("gc"), initial = root.GetProperty("initialGc");
            Program.Check(gc.GetProperty("beforeQpc").GetInt64() <= gc.GetProperty("afterQpc").GetInt64() && gc.GetProperty("totalAllocatedBytesApprox").GetInt64() >= 0 && gc.GetProperty("totalPauseTimeSpanTicks").GetInt64() >= initial.GetProperty("totalPauseTimeSpanTicks").GetInt64(), "Real cumulative GC metrics must have their own bracketed sample boundaries.");
            JsonElement api = root.GetProperty("assemblies").EnumerateArray().Single(a => a.GetProperty("role").GetString() == "api");
            using FileStream stream = File.OpenRead(typeof(Script).Assembly.Location);
            Program.Check(api.GetProperty("location").GetString() == typeof(Script).Assembly.Location && api.GetProperty("mvid").GetString() == typeof(Script).Assembly.ManifestModule.ModuleVersionId.ToString("D") && api.GetProperty("sha256").GetString() == Convert.ToHexString(SHA256.HashData(stream)), "Identity must describe the actual resident API assembly and file bytes.");
        }
        finally { BatchMathDiagnostics.Stop(session.Batch); if (Directory.Exists(directory)) Directory.Delete(directory, recursive: true); }
    }
}
