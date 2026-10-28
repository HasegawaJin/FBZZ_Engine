/// @file    ObjectBatchBridgeTests.cs
/// @brief   本物の NativeBridge で一括 payload・借用 buffer・失敗時未変更と再送禁止を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Buffers.Binary;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static unsafe class ObjectBatchBridgeTests
{
    private enum ReplyMode
    {
        SUCCESS, INVALID_ARGUMENT, BUFFER_TOO_SMALL, INVALID_SCOPE, SCENE_MISMATCH, PHYSICS_BODY,
        RETURN_MISMATCH, UNKNOWN_STATUS, BAD_LENGTH, BAD_COUNTS, UNKNOWN_SLOT, SLOT_COUNT_MISMATCH,
        FAILURE_COUNTS, FAILURE_LENGTH
    }

    private static readonly byte[] s_input = new byte[40001 * 28];
    private static ReplyMode s_mode;
    private static int s_dispatches;
    private static BridgeRequest s_request;
    private static bool s_payloadValid;
    private static bool s_nonfiniteRead;
    private static bool s_nanFinalRow;
    private static BridgeRequest s_phaseRequest;
    private static byte[] s_phaseInput = [], s_phaseOutput = [];
    private static int s_phaseReceived;
    private static BridgeReply s_preparedReply;
    private static int s_resultPages, s_failResultPage, s_publications;

    internal static void Run()
    {
        (string Name, Action Test)[] tests =
        [
            ("WireSlotsAndCallerTails", WireSlotsAndCallerTails),
            ("ExpectedFailuresPreserveCallerBuffers", ExpectedFailuresPreserveCallerBuffers),
            ("InvalidRepliesPreserveCallerBuffers", InvalidRepliesPreserveCallerBuffers),
            ("PreflightBoundsAndEmptyDispatch", PreflightBoundsAndEmptyDispatch),
            ("ScratchReuseHasNoWarmAllocations", ScratchReuseHasNoWarmAllocations),
            ("LateResultPageFailureCannotPublishMutation", LateResultPageFailureCannotPublishMutation)
        ];
        foreach ((string name, Action test) in tests)
        {
            test();
            Console.WriteLine("[PASS] ObjectBatchBridge." + name);
        }
        Console.WriteLine($"OBJECT BATCH BRIDGE TESTS passed={tests.Length} failed=0");
    }

    private static void WireSlotsAndCallerTails()
    {
        IScriptNativeBridge bridge = CreateBridge();
        ScriptExecutionScope scope = Scope(bridge);
        GameObject[] objects = [new(202, 42, "first"), null!, new(0, 42, "missing"), new(999, 42, "expired"), new(303, 42, "second"), new(202, 42, "first")];
        Vector3 sentinel = new(-10, -20, -30);
        Vector3[] positions = new Vector3[8]; Array.Fill(positions, sentinel);
        ObjectStatus[] statuses = new ObjectStatus[8]; Array.Fill(statuses, (ObjectStatus)77);
        Reset();
        BatchResult read = bridge.ReadWorldPositions(scope, objects, positions, statuses);
        Result(read, BatchStatus.OK, 6, 3, 3);
        Program.Check(s_dispatches == 1 && s_payloadValid && s_request.Op == BridgeOp.OBJECTS_READ_WORLD_POSITIONS
            && s_request.InputBytes == 96 && s_request.OutputCapacity == 96, "Read must use fixed per-slot rows and one dispatch.");
        ulong[] ids = [202, 0, 0, 999, 303, 202];
        ulong[] lifetimes = [42, 0, 42, 42, 42, 42];
        for (int index = 0; index < objects.Length; index++)
        {
            Program.Check(BinaryPrimitives.ReadUInt64LittleEndian(s_input.AsSpan(index * 16)) == ids[index]
                && BinaryPrimitives.ReadUInt64LittleEndian(s_input.AsSpan(index * 16 + 8)) == lifetimes[index],
                "Rows must preserve order, duplicates and distinct null/missing identities.");
            ObjectStatus expected = index == 1 ? ObjectStatus.UNSET : index is 2 or 3 ? ObjectStatus.EXPIRED : ObjectStatus.VALID;
            Program.Check(statuses[index] == expected, "Each result must retain its input slot.");
            Program.Check(positions[index].Equals(expected == ObjectStatus.VALID ? new Vector3((float)ids[index], 2, 3) : sentinel),
                "Only valid slots may replace a caller position.");
        }
        Program.Check(positions[6].Equals(sentinel) && positions[7].Equals(sentinel) && statuses[6] == (ObjectStatus)77 && statuses[7] == (ObjectStatus)77,
            "Read must leave both caller tails unchanged.");
        positions[1] = new(float.NaN, 0, 0); positions[2] = new(float.PositiveInfinity, 0, 0); positions[3] = new(float.NegativeInfinity, 0, 0);
        Reset();
        BatchResult set = bridge.SetWorldPositions(scope, objects, positions, statuses);
        Result(set, BatchStatus.OK, 6, 3, 3);
        Program.Check(s_dispatches == 1 && s_payloadValid && s_request.InputBytes == 168 && s_request.OutputCapacity == 24,
            "Set must pack reference + XYZ without struct padding and dispatch once.");
        Program.Check(float.IsNaN(BinaryPrimitives.ReadSingleLittleEndian(s_input.AsSpan(28 + 16)))
            && float.IsPositiveInfinity(BinaryPrimitives.ReadSingleLittleEndian(s_input.AsSpan(56 + 16)))
            && float.IsNegativeInfinity(BinaryPrimitives.ReadSingleLittleEndian(s_input.AsSpan(84 + 16))),
            "Host must not reject nonfinite data in slots whose liveness is decided by native.");
        Program.Check(positions[6].Equals(sentinel) && statuses[6] == (ObjectStatus)77, "Set must keep excess spans untouched.");
        Reset();
        BatchResult translated = bridge.TranslateWorld(scope, objects, new Vector3(4, 5, 6), true);
        Result(translated, BatchStatus.OK, 6, 2, 4);
        Program.Check(s_dispatches == 1 && s_payloadValid && s_request.Flags == 1 && s_request.InputBytes == 96
            && s_request.OutputCapacity == 0 && s_request.Output == null && s_request.X == 4 && s_request.Y == 5 && s_request.Z == 6,
            "Translation must pass one common delta, active filter and no output buffer.");
        Reset(); s_nonfiniteRead = true;
        Vector3 before = positions[0]; ObjectStatus previous = statuses[0];
        Program.Throws<InvalidDataException>(() => bridge.ReadWorldPositions(scope, objects, positions, statuses));
        Program.Check(positions[0].Equals(before) && statuses[0] == previous,
            "A nonfinite valid world position must reject before modifying caller output.");
    }

    private static void ExpectedFailuresPreserveCallerBuffers()
    {
        IScriptNativeBridge bridge = CreateBridge(); ScriptExecutionScope scope = Scope(bridge);
        GameObject[] objects = [new(202, 42, "first")];
        Vector3[] positions = [new(7, 8, 9)]; ObjectStatus[] statuses = [(ObjectStatus)77];
        (ReplyMode Mode, BatchStatus Status)[] failures =
        [
            (ReplyMode.INVALID_ARGUMENT, BatchStatus.INVALID_ARGUMENT), (ReplyMode.BUFFER_TOO_SMALL, BatchStatus.BUFFER_TOO_SMALL),
            (ReplyMode.INVALID_SCOPE, BatchStatus.INVALID_SCOPE), (ReplyMode.SCENE_MISMATCH, BatchStatus.SCENE_MISMATCH),
            (ReplyMode.PHYSICS_BODY, BatchStatus.PHYSICS_BODY)
        ];
        foreach ((ReplyMode mode, BatchStatus status) in failures)
        {
            Reset(mode);
            Result(bridge.ReadWorldPositions(scope, objects, positions, statuses), status, 1, 0, 0);
            Program.Check(s_dispatches == 1 && positions[0].Equals(new Vector3(7, 8, 9)) && statuses[0] == (ObjectStatus)77,
                "Expected failure must not decode scratch or retry the call.");
            Reset(mode);
            Result(bridge.SetWorldPositions(scope, objects, positions, statuses), status, 1, 0, 0);
            Program.Check(s_dispatches == 1 && positions[0].Equals(new Vector3(7, 8, 9)) && statuses[0] == (ObjectStatus)77,
                "Set failures must leave caller status untouched and never replay a mutation.");
        }
    }

    private static void InvalidRepliesPreserveCallerBuffers()
    {
        IScriptNativeBridge bridge = CreateBridge(); ScriptExecutionScope scope = Scope(bridge);
        GameObject[] objects = [new(202, 42, "first")];
        Vector3[] positions = [new(7, 8, 9)]; ObjectStatus[] statuses = [(ObjectStatus)77];
        ReplyMode[] modes = [ReplyMode.RETURN_MISMATCH, ReplyMode.UNKNOWN_STATUS, ReplyMode.BAD_LENGTH, ReplyMode.BAD_COUNTS,
            ReplyMode.UNKNOWN_SLOT, ReplyMode.SLOT_COUNT_MISMATCH, ReplyMode.FAILURE_COUNTS, ReplyMode.FAILURE_LENGTH];
        foreach (ReplyMode mode in modes)
        {
            Reset(mode);
            Program.Throws<InvalidDataException>(() => bridge.ReadWorldPositions(scope, objects, positions, statuses));
            Program.Check(s_dispatches == 1 && positions[0].Equals(new Vector3(7, 8, 9)) && statuses[0] == (ObjectStatus)77,
                "Every reply must be validated before writing the first caller slot.");
        }
        GameObject[] two = [new(202, 42, "first"), new(303, 42, "second")];
        Vector3[] twoPositions = [new(7, 8, 9), new(10, 11, 12)];
        ObjectStatus[] twoStatuses = [(ObjectStatus)77, (ObjectStatus)77];
        Reset(ReplyMode.UNKNOWN_SLOT);
        Program.Throws<InvalidDataException>(() => bridge.ReadWorldPositions(scope, two, twoPositions, twoStatuses));
        Program.Check(s_dispatches == 1 && twoPositions[0].Equals(new Vector3(7, 8, 9)) && twoPositions[1].Equals(new Vector3(10, 11, 12))
            && twoStatuses[0] == (ObjectStatus)77 && twoStatuses[1] == (ObjectStatus)77,
            "A malformed last slot must not commit an earlier valid position or status.");
        Reset();
        Program.Check(bridge.ReadWorldPositions(scope, objects, positions, statuses).Succeeded,
            "A protocol exception must release the scratch lease for later calls.");
    }

    private static void PreflightBoundsAndEmptyDispatch()
    {
        IScriptNativeBridge bridge = CreateBridge(); ScriptExecutionScope scope = Scope(bridge);
        GameObject[] one = [new(202, 42, "first")];
        Vector3[] positions = [new(7, 8, 9)]; ObjectStatus[] statuses = [(ObjectStatus)77];
        Reset();
        Result(bridge.ReadWorldPositions(scope, one, [], statuses), BatchStatus.BUFFER_TOO_SMALL, 1, 0, 0);
        Result(bridge.ReadWorldPositions(scope, one, positions, []), BatchStatus.BUFFER_TOO_SMALL, 1, 0, 0);
        Result(bridge.SetWorldPositions(scope, one, [], statuses), BatchStatus.INVALID_ARGUMENT, 1, 0, 0);
        Result(bridge.SetWorldPositions(scope, one, positions, []), BatchStatus.BUFFER_TOO_SMALL, 1, 0, 0);
        Result(bridge.TranslateWorld(scope, one, new Vector3(float.NaN, 0, 0), false), BatchStatus.INVALID_ARGUMENT, 1, 0, 0);
        GameObject[] foreign = [new(0, 43, "missing")];
        Result(bridge.ReadWorldPositions(scope, foreign, positions, statuses), BatchStatus.SCENE_MISMATCH, 1, 0, 0);
        Program.Check(s_dispatches == 0 && positions[0].Equals(new Vector3(7, 8, 9)) && statuses[0] == (ObjectStatus)77,
            "Shape, delta and scene failures must be rejected before native access.");
        GameObject[] overLimit = new GameObject[32769];
        Result(bridge.TranslateWorld(scope, overLimit, default, false), BatchStatus.OK, 32769, 0, 32769);
        Program.Check(s_dispatches > 3 && s_phaseReceived == overLimit.Length * 16, "Packet budgets must not reject a valid larger collection.");
        Reset();
        Result(bridge.ReadWorldPositions(scope, [], positions, statuses), BatchStatus.OK, 0, 0, 0);
        Result(bridge.SetWorldPositions(scope, [], positions, statuses), BatchStatus.OK, 0, 0, 0);
        Result(bridge.TranslateWorld(scope, [], default, false), BatchStatus.OK, 0, 0, 0);
        Program.Check(s_dispatches == 3 && s_request.Index == 0 && s_request.InputBytes == 0 && s_request.OutputCapacity == 0,
            "Empty requests must still perform exactly one scope/phase validation dispatch each.");
        Reset(ReplyMode.INVALID_SCOPE);
        Result(bridge.TranslateWorld(scope, [], default, false), BatchStatus.INVALID_SCOPE, 0, 0, 0);
        Program.Check(s_dispatches == 1, "An empty request must not bypass a native scope refusal.");
    }

    private static void ScratchReuseHasNoWarmAllocations()
    {
        IScriptNativeBridge bridge = CreateBridge(); ScriptExecutionScope scope = Scope(bridge);
        GameObject[] objects = new GameObject[384]; Array.Fill(objects, new GameObject(202, 42, "first"));
        Vector3[] positions = new Vector3[384]; ObjectStatus[] statuses = new ObjectStatus[384];
        Reset();
        for (int index = 0; index < 16; index++)
        {
            bridge.ReadWorldPositions(scope, objects, positions, statuses);
            bridge.SetWorldPositions(scope, objects, positions, statuses);
            bridge.TranslateWorld(scope, objects, default, false);
        }
        object? input = Scratch(bridge, "m_batchInput"); object? output = Scratch(bridge, "m_batchOutput");
        int dispatches = s_dispatches; bool succeeded = true;
        long before = GC.GetAllocatedBytesForCurrentThread();
        for (int index = 0; index < 64; index++)
        {
            succeeded &= bridge.ReadWorldPositions(scope, objects, positions, statuses).Succeeded;
            succeeded &= bridge.SetWorldPositions(scope, objects, positions, statuses).Succeeded;
            succeeded &= bridge.TranslateWorld(scope, objects, default, false).Succeeded;
        }
        long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
        Program.Check(succeeded && allocated == 0 && s_dispatches - dispatches == 192,
            $"Warm buffers must allocate no managed memory and dispatch once per call (allocated={allocated}).");
        Program.Check(ReferenceEquals(input, Scratch(bridge, "m_batchInput")) && ReferenceEquals(output, Scratch(bridge, "m_batchOutput")),
            "Smaller/repeated operations must retain both persistent buffers.");
        GameObject[] maximum = new GameObject[32768];
        Result(bridge.TranslateWorld(scope, maximum, default, false), BatchStatus.OK, 32768, 0, 32768);
    }

    private static IScriptNativeBridge CreateBridge()
    {
        Type type = typeof(EntryPoint).GetNestedType("NativeBridge", BindingFlags.NonPublic)
            ?? throw new InvalidOperationException("The actual NativeBridge is missing.");
        ConstructorInfo constructor = type.GetConstructor(BindingFlags.Instance | BindingFlags.NonPublic, null, [typeof(nint)], null)
            ?? throw new InvalidOperationException("The actual NativeBridge constructor is missing.");
        nint address = (nint)(delegate* unmanaged[Cdecl]<BridgeRequest*, BridgeReply*, int>)&Dispatch;
        return (IScriptNativeBridge)constructor.Invoke([address]);
    }

    private static ScriptExecutionScope Scope(IScriptNativeBridge bridge) => new(bridge, new NoComponents(), 7, 9, 101, 13, 42, ScriptCallback.UPDATE);
    private static object? Scratch(IScriptNativeBridge bridge, string name) => bridge.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)?.GetValue(bridge);

    private static void Result(BatchResult result, BatchStatus status, int input, int applied, int skipped)
    {
        Program.Check(result.Status == status && result.Succeeded == (status == BatchStatus.OK) && result.InputCount == input
            && result.AppliedCount == applied && result.SkippedCount == skipped, "Batch result must retain status and truthful slot counts.");
    }

    private static void Reset(ReplyMode mode = ReplyMode.SUCCESS)
    {
        s_mode = mode; s_dispatches = 0; s_request = default; s_payloadValid = false; s_nonfiniteRead = s_nanFinalRow = false;
        s_resultPages = s_failResultPage = s_publications = 0;
    }

    [UnmanagedCallersOnly(CallConvs = new Type[] { typeof(CallConvCdecl) })]
    private static int Dispatch(BridgeRequest* request, BridgeReply* reply)
    {
        s_dispatches++; s_request = *request; *reply = default;
        if (request->Flags >> 8 != 0) return FillBatchPhase(request, reply);
        return FillBatch(request, reply);
    }
    private static int FillBatchPhase(BridgeRequest* request, BridgeReply* reply)
    {
        *reply = new BridgeReply { Status = BridgeStatus.OK, CallId = 42 };
        uint phase = request->Flags >> 8;
        bool set = request->Op == BridgeOp.OBJECTS_SET_WORLD_POSITIONS, read = request->Op == BridgeOp.OBJECTS_READ_WORLD_POSITIONS;
        int stride = set ? 28 : 16, outputStride = read ? 16 : set ? 4 : 0;
        if (phase == 1) {
            s_phaseRequest = *request; s_phaseRequest.Flags &= 255; s_phaseRequest.Integer = 0; s_phaseReceived = 0;
            s_phaseInput = new byte[checked((int)request->Index * stride)]; s_phaseOutput = new byte[checked((int)request->Index * outputStride)]; reply->Integer = 77; return 0;
        }
        if (phase == 2) {
            int bytes = checked((int)request->InputBytes);
            if (bytes > BridgeAbi.MAX_BATCH_PACKET_BYTES || (int)request->Index * stride != s_phaseReceived || s_phaseReceived + bytes > s_phaseInput.Length) { reply->Status = BridgeStatus.INVALID_ARGUMENT; return (int)reply->Status; }
            new ReadOnlySpan<byte>(request->Input, bytes).CopyTo(s_phaseInput.AsSpan(s_phaseReceived)); s_phaseReceived += bytes; reply->Integer = s_phaseReceived / stride; return 0;
        }
        if (phase == 3) {
            BridgeRequest aggregate = s_phaseRequest;
            fixed (byte* input = s_phaseInput) fixed (byte* output = s_phaseOutput) {
                aggregate.Input = input; aggregate.InputBytes = (uint)s_phaseInput.Length; aggregate.Output = output; aggregate.OutputCapacity = (uint)s_phaseOutput.Length;
                int code = FillBatch(&aggregate, reply); reply->CallId = 42; reply->OutputBytes = 0; reply->Generation = (ulong)s_phaseOutput.Length; s_preparedReply = *reply; return code;
            }
        }
        if (phase == 4) {
            s_resultPages++;
            if (s_resultPages == s_failResultPage) { reply->Status = BridgeStatus.INVALID_SCOPE; return (int)reply->Status; }
            ulong offset = ((ulong)request->Reserved1 << 32) | request->Reserved0;
            int bytes = Math.Min((int)request->OutputCapacity, s_phaseOutput.Length - (int)offset);
            s_phaseOutput.AsSpan((int)offset, bytes).CopyTo(new Span<byte>(request->Output, bytes));
            reply->OutputBytes = (uint)bytes; reply->Generation = (ulong)s_phaseOutput.Length; reply->Integer = (long)offset + bytes; reply->Flags = offset + (uint)bytes == (ulong)s_phaseOutput.Length ? 1U : 0U; return 0;
        }
        if (phase == 6) { s_publications++; *reply = s_preparedReply; return (int)reply->Status; }
        return 0;
    }
    private static void LateResultPageFailureCannotPublishMutation()
    {
        IScriptNativeBridge bridge = CreateBridge(); ScriptExecutionScope scope = Scope(bridge);
        GameObject[] objects = new GameObject[40001]; Array.Fill(objects, new GameObject(202, 42, "same"));
        Vector3[] positions = new Vector3[40001]; ObjectStatus[] statuses = new ObjectStatus[40003]; Array.Fill(statuses, (ObjectStatus)77);
        Reset(); s_failResultPage = 2;
        Result(bridge.SetWorldPositions(scope, objects, positions, statuses), BatchStatus.INVALID_SCOPE, objects.Length, 0, 0);
        Program.Check(s_publications == 0 && statuses[0] == (ObjectStatus)77 && statuses[^1] == (ObjectStatus)77,
            "A second prepared-result page failure must happen before publication and leave caller output unchanged.");
        Reset(); Result(bridge.SetWorldPositions(scope, objects, positions, statuses), BatchStatus.OK, objects.Length, objects.Length, 0);
        Program.Check(s_publications == 1 && statuses[0] == ObjectStatus.VALID && statuses[^1] == (ObjectStatus)77,
            "All prepared results must validate before the single fixed-ack publication; preserve caller tail.");
        Array.Fill(positions, new Vector3(7, 8, 9)); Array.Fill(statuses, (ObjectStatus)77); Reset(); s_nanFinalRow = true;
        Program.Throws<InvalidDataException>(() => bridge.ReadWorldPositions(scope, objects, positions, statuses));
        Program.Check(s_publications == 0 && positions[0].Equals(new Vector3(7, 8, 9)) && positions[^1].Equals(new Vector3(7, 8, 9)) && statuses[0] == (ObjectStatus)77,
            "A nonfinite VALID row in received result pages must reject before publication or caller output.");
    }
    private static int FillBatch(BridgeRequest* request, BridgeReply* reply)
    {
        bool read = request->Op == BridgeOp.OBJECTS_READ_WORLD_POSITIONS;
        bool set = request->Op == BridgeOp.OBJECTS_SET_WORLD_POSITIONS;
        int inputStride = set ? 28 : 16; int outputStride = read ? 16 : set ? 4 : 0;
        int count = (int)request->Index; int outputBytes = count * outputStride;
        s_payloadValid = request->InputBytes == count * inputStride && request->OutputCapacity == outputBytes
            && request->AbiVersion == BridgeAbi.VERSION && request->Generation == 7 && request->InstanceId == 9
            && request->OwnerId == 101 && request->CallId == 13 && request->Integer == 0;
        if (!s_payloadValid) { reply->Status = BridgeStatus.INVALID_ARGUMENT; return (int)reply->Status; }
        new ReadOnlySpan<byte>(request->Input, (int)request->InputBytes).CopyTo(s_input);
        Span<byte> output = new(request->Output, outputBytes);
        output.Fill(0xcc);
        if (s_mode is ReplyMode.INVALID_ARGUMENT or ReplyMode.BUFFER_TOO_SMALL or ReplyMode.INVALID_SCOPE or ReplyMode.SCENE_MISMATCH or ReplyMode.PHYSICS_BODY or ReplyMode.FAILURE_COUNTS or ReplyMode.FAILURE_LENGTH)
        {
            reply->Status = s_mode switch
            {
                ReplyMode.BUFFER_TOO_SMALL => BridgeStatus.BUFFER_TOO_SMALL, ReplyMode.INVALID_SCOPE => BridgeStatus.INVALID_SCOPE,
                ReplyMode.SCENE_MISMATCH => BridgeStatus.SCENE_MISMATCH, ReplyMode.PHYSICS_BODY => BridgeStatus.PHYSICS_BODY,
                _ => BridgeStatus.INVALID_ARGUMENT
            };
            reply->OutputBytes = s_mode == ReplyMode.BUFFER_TOO_SMALL ? (uint)outputBytes : 0;
            if (s_mode == ReplyMode.FAILURE_COUNTS) reply->Index = 1;
            if (s_mode == ReplyMode.FAILURE_LENGTH) reply->OutputBytes = (uint)outputBytes + 1;
            return (int)reply->Status;
        }
        ReadOnlySpan<byte> input = s_input.AsSpan(0, (int)request->InputBytes);
        uint valid = 0;
        for (int index = 0; index < count; index++)
        {
            ulong id = BinaryPrimitives.ReadUInt64LittleEndian(input[(index * inputStride)..]);
            ulong lifetime = BinaryPrimitives.ReadUInt64LittleEndian(input[(index * inputStride + 8)..]);
            ObjectStatus status = id == 0 && lifetime == 0 ? ObjectStatus.UNSET : id is 0 or 999 ? ObjectStatus.EXPIRED : ObjectStatus.VALID;
            bool applied = status == ObjectStatus.VALID && !(outputStride == 0 && request->Flags == 1 && id == 303);
            if (applied) valid++;
            if (outputStride == 0) continue;
            Span<byte> row = output.Slice(index * outputStride, outputStride);
            BinaryPrimitives.WriteUInt32LittleEndian(row, (uint)status);
            if (read)
            {
                BinaryPrimitives.WriteSingleLittleEndian(row[4..], s_nonfiniteRead || (s_nanFinalRow && index + 1 == count) ? float.NaN : (float)id);
                BinaryPrimitives.WriteSingleLittleEndian(row[8..], s_nonfiniteRead ? float.PositiveInfinity : 2);
                BinaryPrimitives.WriteSingleLittleEndian(row[12..], 3);
            }
        }
        reply->Index = valid; reply->Flags = (uint)count - valid; reply->OutputBytes = (uint)outputBytes;
        if (s_mode == ReplyMode.BAD_LENGTH) reply->OutputBytes++;
        if (s_mode == ReplyMode.BAD_COUNTS) reply->Index++;
        if (s_mode == ReplyMode.UNKNOWN_SLOT) BinaryPrimitives.WriteUInt32LittleEndian(output[((count - 1) * outputStride)..], 99);
        if (s_mode == ReplyMode.SLOT_COUNT_MISMATCH) { reply->Index = 0; reply->Flags = (uint)count; }
        if (s_mode == ReplyMode.UNKNOWN_STATUS) reply->Status = (BridgeStatus)99;
        return s_mode == ReplyMode.RETURN_MISMATCH ? (int)BridgeStatus.INVALID_ARGUMENT : (int)reply->Status;
    }

    private sealed class NoComponents : IScriptComponentLookup
    {
        public Component[] GetComponents(ScriptExecutionScope scope, GameObject target, Type type) => [];
    }
}
