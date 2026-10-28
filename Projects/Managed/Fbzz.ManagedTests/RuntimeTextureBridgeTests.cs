/// @file    RuntimeTextureBridgeTests.cs
/// @brief   Validate the actual Host texture transport, bounded chunks and atomic failure protocol.
/// @author  Hasegawa Jin
/// @date    2026-10-26
using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static unsafe class RuntimeTextureBridgeTests
{
    enum ReplyMode { SUCCESS, BAD_APPEND_ACK, APPEND_FAILURE, BEGIN_REJECTED, COMMIT_REJECTED }
    static ReplyMode s_mode;
    static bool s_valid;
    static int s_selfCalls, s_createCalls, s_beginCalls, s_chunks, s_commits, s_aborts, s_received;
    static int s_width, s_height, s_format;
    static byte[] s_expected = [], s_uploaded = [];
    static string[] s_rectangle = [];

    internal static IReadOnlyList<(string Name, Action Run)> Cases => [
        (nameof(RuntimeTextureHostCreatesForCurrentObject), RuntimeTextureHostCreatesForCurrentObject),
        (nameof(RuntimeTextureHostChunksExactPaddedBytes), RuntimeTextureHostChunksExactPaddedBytes),
        (nameof(RuntimeTextureHostAbortsBeforeFailedTransferCanCommit), RuntimeTextureHostAbortsBeforeFailedTransferCanCommit)
    ];

    static void RuntimeTextureHostCreatesForCurrentObject()
    {
        IScriptNativeBridge bridge = CreateBridge();
        ScriptExecutionScope? previous = ScriptApiRuntime.Current;
        try {
            ScriptApiRuntime.Current = Scope(bridge);
            Reset(); s_width = 512; s_height = 384;
            RuntimeTexture r8 = RuntimeTexture.CreateR8(512, 384);
            Program.Check(s_valid && s_selfCalls == 1 && s_createCalls == 1 && r8.Width == 512
                && r8.Height == 384 && r8.Format == RuntimeTextureFormat.R8,
                "CreateR8 must obtain the current owner object before the ASSETS creation service.");
            Reset(); s_width = 7; s_height = 5; s_format = 1;
            RuntimeTexture rgba = RuntimeTexture.CreateRGBA8(7, 5);
            Program.Check(s_valid && s_selfCalls == 1 && s_createCalls == 1 && rgba.Format == RuntimeTextureFormat.RGBA8,
                "RGBA8 creation must preserve the native format discriminator.");
        } finally { ScriptApiRuntime.Current = previous; }
    }

    static void RuntimeTextureHostChunksExactPaddedBytes()
    {
        IScriptNativeBridge bridge = CreateBridge(); ScriptExecutionScope scope = Scope(bridge);
        Reset();
        byte[] source = new byte[(259 - 1) * 263 + 257];
        for (int index = 0; index < source.Length; ++index) source[index] = (byte)((index * 73 + 19) & 255);
        s_expected = (byte[])source.Clone(); s_uploaded = new byte[source.Length];
        s_rectangle = ["11", "13", "257", "259", "263"];
        Program.Check(bridge.UpdateRuntimeTexture(scope, 202, 77, 11, 13, 257, 259, 263, source),
            "A padded R8 transfer larger than the packet limit must commit successfully.");
        Program.Check(s_valid && s_beginCalls == 1 && s_chunks == 2 && s_commits == 1 && s_aborts == 0
            && s_received == source.Length && s_uploaded.AsSpan().SequenceEqual(s_expected),
            "The Host must send sequential bounded chunks, exact padding and a single final publication.");
        Array.Fill(source, (byte)0);
        Program.Check(s_uploaded.AsSpan().SequenceEqual(s_expected),
            "Native consumption must finish synchronously before the caller may reuse its input span.");
    }

    static void RuntimeTextureHostAbortsBeforeFailedTransferCanCommit()
    {
        IScriptNativeBridge bridge = CreateBridge(); ScriptExecutionScope scope = Scope(bridge);
        foreach (ReplyMode mode in new[] { ReplyMode.BAD_APPEND_ACK, ReplyMode.APPEND_FAILURE }) {
            Reset(mode); s_expected = new byte[70000]; s_uploaded = new byte[70000];
            s_rectangle = ["0", "0", "350", "200", "350"];
            if (mode == ReplyMode.BAD_APPEND_ACK)
                Program.Throws<InvalidDataException>(() => bridge.UpdateRuntimeTexture(scope, 202, 77, 0, 0, 350, 200, 350, s_expected));
            else
                Program.Throws<InvalidOperationException>(() => bridge.UpdateRuntimeTexture(scope, 202, 77, 0, 0, 350, 200, 350, s_expected));
            Program.Check(s_valid && s_beginCalls == 1 && s_chunks == 1 && s_aborts == 1 && s_commits == 0,
                "Malformed acknowledgments and append refusal must abort once without replay or commit.");
        }
        Reset(ReplyMode.BEGIN_REJECTED); s_rectangle = ["0", "0", "1", "1", "1"];
        Program.Check(!bridge.UpdateRuntimeTexture(scope, 202, 77, 0, 0, 1, 1, 1, new byte[1])
            && s_valid && s_beginCalls == 1 && s_chunks == 0 && s_commits == 0 && s_aborts == 0,
            "A rejected begin must not send data or invent a reception to abort.");
        Reset(ReplyMode.COMMIT_REJECTED); s_expected = [99]; s_uploaded = new byte[1];
        s_rectangle = ["0", "0", "1", "1", "1"];
        Program.Check(!bridge.UpdateRuntimeTexture(scope, 202, 77, 0, 0, 1, 1, 1, s_expected)
            && s_valid && s_chunks == 1 && s_commits == 1 && s_aborts == 0,
            "A native commit refusal must return false without replaying publication.");
    }

    static IScriptNativeBridge CreateBridge()
    {
        Type type = typeof(EntryPoint).GetNestedType("NativeBridge", BindingFlags.NonPublic)
            ?? throw new InvalidOperationException("The actual NativeBridge is missing.");
        ConstructorInfo constructor = type.GetConstructor(BindingFlags.Instance | BindingFlags.NonPublic, null, [typeof(nint)], null)
            ?? throw new InvalidOperationException("The actual NativeBridge constructor is missing.");
        nint address = (nint)(delegate* unmanaged[Cdecl]<BridgeRequest*, BridgeReply*, int>)&Dispatch;
        return (IScriptNativeBridge)constructor.Invoke([address]);
    }

    static ScriptExecutionScope Scope(IScriptNativeBridge bridge) => new(bridge, new NoComponents(), 7, 9, 101, 13, 42, ScriptCallback.UPDATE);
    static void Reset(ReplyMode mode = ReplyMode.SUCCESS)
    {
        s_mode = mode; s_valid = true;
        s_selfCalls = s_createCalls = s_beginCalls = s_chunks = s_commits = s_aborts = s_received = 0;
        s_width = s_height = s_format = 0; s_expected = s_uploaded = []; s_rectangle = [];
    }

    [UnmanagedCallersOnly(CallConvs = new Type[] { typeof(CallConvCdecl) })]
    static int Dispatch(BridgeRequest* request, BridgeReply* reply)
    {
        *reply = new BridgeReply { Status = BridgeStatus.OK, CallId = 42 };
        try {
            s_valid &= request->Generation == 7 && request->InstanceId == 9 && request->OwnerId == 101 && request->CallId == 13;
            if (request->Op == BridgeOp.OBJECT_SELF) {
                s_selfCalls++;
                s_valid &= request->Integer == 0 && s_createCalls == 0 && request->OutputCapacity >= 5;
                reply->Integer = 202; return 0;
            }
            s_valid &= request->Op == BridgeOp.SERVICE_ASSETS && request->Integer == 202 && request->Flags == 0
                && request->OutputCapacity >= 1024;
            ReadOnlySpan<byte> input = new(request->Input, checked((int)request->InputBytes));
            if (request->Index == 10) {
                s_createCalls++;
                s_valid &= s_selfCalls == 1 && input.Length == 36 && BinaryPrimitives.ReadUInt64LittleEndian(input) == 0
                    && BinaryPrimitives.ReadInt64LittleEndian(input[8..]) == 0
                    && BinaryPrimitives.ReadUInt32LittleEndian(input[16..]) == 3
                    && BinaryPrimitives.ReadUInt32LittleEndian(input[20..]) == 0
                    && BinaryPrimitives.ReadSingleLittleEndian(input[24..]) == s_width
                    && BinaryPrimitives.ReadSingleLittleEndian(input[28..]) == s_height
                    && BinaryPrimitives.ReadSingleLittleEndian(input[32..]) == s_format;
                ServiceReply(request, reply, 77); return 0;
            }
            if (request->Index == 11) {
                s_beginCalls++;
                s_valid &= s_chunks == 0 && input.Length >= 24 && BinaryPrimitives.ReadUInt64LittleEndian(input) == 0
                    && BinaryPrimitives.ReadInt64LittleEndian(input[8..]) == 77
                    && BinaryPrimitives.ReadUInt32LittleEndian(input[16..]) == 0
                    && BinaryPrimitives.ReadUInt32LittleEndian(input[20..]) == 5;
                int offset = 24;
                foreach (string expected in s_rectangle) {
                    int count = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(input[offset..])); offset += 4;
                    s_valid &= Encoding.UTF8.GetString(input.Slice(offset, count)) == expected; offset += count;
                }
                s_valid &= offset == input.Length;
                ServiceReply(request, reply, s_mode == ReplyMode.BEGIN_REJECTED ? 0 : 88); return 0;
            }
            s_valid &= s_beginCalls == 1 && input.Length >= 8 && BinaryPrimitives.ReadUInt64LittleEndian(input) == 88;
            if (request->Index == 12) {
                s_chunks++;
                int count = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(input[16..]));
                s_valid &= input.Length <= 65536 && input.Length == count + 24 && count > 0
                    && BinaryPrimitives.ReadUInt64LittleEndian(input[8..]) == (ulong)s_received
                    && BinaryPrimitives.ReadUInt32LittleEndian(input[20..]) == 0 && s_commits == 0 && s_aborts == 0;
                input[24..].CopyTo(s_uploaded.AsSpan(s_received));
                s_received += count;
                reply->Integer = s_mode == ReplyMode.BAD_APPEND_ACK ? s_received + 1 : s_received;
                if (s_mode == ReplyMode.APPEND_FAILURE) { reply->Status = BridgeStatus.INVALID_ARGUMENT; return (int)reply->Status; }
                return 0;
            }
            if (request->Index == 13) {
                s_commits++; s_valid &= input.Length == 8 && s_received == s_expected.Length && s_aborts == 0;
                reply->Integer = s_mode == ReplyMode.COMMIT_REJECTED ? 0 : 1; return 0;
            }
            if (request->Index == 14) {
                s_aborts++; s_valid &= input.Length == 8 && s_commits == 0; reply->Integer = 1; return 0;
            }
        } catch { s_valid = false; }
        reply->Status = BridgeStatus.INVALID_ARGUMENT; return (int)reply->Status;
    }

    static void ServiceReply(BridgeRequest* request, BridgeReply* reply, long integer)
    {
        new Span<byte>(request->Output, 20).Clear(); reply->OutputBytes = 20; reply->Integer = integer;
    }

    sealed class NoComponents : IScriptComponentLookup
    {
        public Component[] GetComponents(ScriptExecutionScope scope, GameObject target, Type type) => [];
    }
}
