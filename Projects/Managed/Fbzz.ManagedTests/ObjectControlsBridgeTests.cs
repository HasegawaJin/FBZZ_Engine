/// @file    ObjectControlsBridgeTests.cs
/// @brief   一括生成の一回性と caller buffer の容量不足・参照 cache を実 marshaller で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
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
internal static unsafe class ObjectControlsBridgeTests
{
    public static (string Name, Action Test)[] Cases =>
    [
        ("ObjectQueryBufferPreservesCallerAndReusesReferences", QueryBuffer),
        ("ObjectQueriesAndCreationGrowBeyondLegacyCapacity", LargeCollections),
        ("ObjectQueryRejectsUnrepresentableAndTruncatedCounts", InvalidCounts),
        ("ObjectQueryCacheRetiresChurnAndPreservesFailedResults", QueryCache),
        ("ObjectPoolAcceptsGrowingScenesAndRejectsNegativeCapacity", Pool),
        ("BulkSpawnRejectsInvalidInputsAndNeverReplays", Spawn),
        ("GameObjectEqualityUsesSceneAndEntityLifetime", Equality),
        ("NumericObjectWritesAvoidWarmPacketAllocation", NumericPacketAllocation),
        ("ObjectTextReadsAllocateOnlyReturnedStrings", TextReadAllocation)
    ];
    static readonly byte[] s_guid = Encoding.UTF8.GetBytes("01234567-89ab-cdef-0123-456789abcdef");
    static void Equality()
    {
        GameObject first = new(11, 3, "same-guid");
        GameObject wrapper = new(11, 3, "same-guid");
        GameObject replacement = new(12, 3, "same-guid");
        GameObject otherScene = new(11, 4, "same-guid");
        Program.Check(first == wrapper && first.GetHashCode() == wrapper.GetHashCode(), "Wrappers for the same lifetime must compare equally without a Script scope.");
        Program.Check(first != replacement && first != otherScene && first != null, "GUID reuse, scene changes and CLR null must retain distinct identities.");
        Program.Check(new GameObject(0, 3, "missing-a") != new GameObject(0, 3, "missing-b"), "Unresolved references retain their distinct saved GUIDs.");
    }
    static uint s_count;
    static BridgeStatus s_status;
    static int s_calls;
    static bool s_scopeSuspended;
    static bool s_truncated;
    static bool s_unrepresentableBytes;
    static ulong s_firstId = 300;
    static bool s_trailingBytes;
    static bool s_numericPacket, s_numericWireValid;
    static byte[]? s_textReply;
    /// @note Verify the real marshaller; the fake endpoint performs no managed decoding or recording allocation.
    static void NumericPacketAllocation()
    {
        using Fixture fixture = new(); s_count = 0; s_status = BridgeStatus.OK; s_numericPacket = true; s_numericWireValid = true;
        try {
            for (int index = 0; index < 32; index++) fixture.Bridge.WriteObject(fixture.Scope, 202, 0, index & 1, "");
            long before = GC.GetAllocatedBytesForCurrentThread();
            for (int index = 0; index < 256; index++) fixture.Bridge.WriteObject(fixture.Scope, 202, 0, index & 1, "");
            long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
            Program.Check(allocated == 0 && s_numericWireValid, "Numeric object writes must retain the exact eight-byte packet without allocating transfer arrays.");
        } finally { s_numericPacket = false; }
    }
    static void TextReadAllocation()
    {
        using Fixture fixture = new(); s_count = 0; s_status = BridgeStatus.OK;
        try {
            foreach (string text in new[] { "short-name", new string('海', 700) }) {
                s_textReply = Encoding.UTF8.GetBytes(text);
                for (int index = 0; index < 32; index++) { _ = fixture.Bridge.ReadObjectText(fixture.Scope, 202, 2); _ = Encoding.UTF8.GetString(s_textReply); }
                long before = GC.GetAllocatedBytesForCurrentThread(); int expectedChars = 0;
                for (int index = 0; index < 128; index++) expectedChars += Encoding.UTF8.GetString(s_textReply).Length;
                long required = GC.GetAllocatedBytesForCurrentThread() - before;
                before = GC.GetAllocatedBytesForCurrentThread(); int actualChars = 0;
                for (int index = 0; index < 128; index++) actualChars += fixture.Bridge.ReadObjectText(fixture.Scope, 202, 2).Length;
                long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
                Program.Check(allocated == required && actualChars == expectedChars && actualChars == text.Length * 128,
                    "UTF8 text reads must allocate their returned strings but no extra response byte arrays, including grown pool packets.");
            }
        } finally { s_textReply = null; }
    }
    static void QueryBuffer()
    {
        using Fixture fixture = new();
        s_count = 3; s_status = BridgeStatus.OK; s_calls = 0;
        GameObject?[] small = [fixture.Script.GameObject];
        ObjectQueryResult missing = fixture.Script.Scene.ReadObjects(small);
        Program.Check(missing.Status == BatchStatus.BUFFER_TOO_SMALL && missing.RequiredCapacity == 3 && missing.Count == 0
            && ReferenceEquals(small[0], fixture.Script.GameObject) && s_calls == 1, "Capacity failure must leave all caller slots unchanged.");
        GameObject?[] output = new GameObject?[4];
        ObjectQueryResult complete = fixture.Script.Scene.ReadObjects(output);
        Program.Check(complete.Status == BatchStatus.OK && complete.Count == 3 && output[3] is null, "A full query must preserve unused slots.");
        GameObject? first = output[0];
        complete = fixture.Script.Scene.ReadObjects(output);
        Program.Check(complete.Count == 3 && ReferenceEquals(first, output[0]), "Steady queries must reuse the same lifetime wrapper.");
        long before = GC.GetAllocatedBytesForCurrentThread();
        for (int index = 0; index < 100; index++) fixture.Script.Scene.ReadObjects(output);
        long bytes = GC.GetAllocatedBytesForCurrentThread() - before;
        Program.Check(bytes == 0, "Warmed caller-buffer queries must not allocate managed transfer arrays or wrappers.");
        GameObject?[] oversized = new GameObject?[5000]; Array.Fill(oversized, fixture.Script.GameObject);
        complete = fixture.Script.Scene.ReadObjects(oversized);
        Program.Check(complete.Status == BatchStatus.OK && complete.Count == 3 && ReferenceEquals(oversized[4999], fixture.Script.GameObject),
            "Extra caller capacity must be accepted with its unused tail unchanged.");
        s_status = BridgeStatus.INVALID_SCOPE;
        Program.Throws<InvalidOperationException>(() => fixture.Script.Scene.ReadObjects(oversized));
        Program.Check(ReferenceEquals(oversized[0], output[0]) && ReferenceEquals(oversized[4999], fixture.Script.GameObject),
            "A failed query must not publish partial caller output.");
        s_status = BridgeStatus.OK;
    }
    static void Spawn()
    {
        using Fixture fixture = new();
        SpawnPose[] poses = [new SpawnPose(Vector3.ZERO, Quaternion.IDENTITY), new SpawnPose(Vector3.RIGHT, Quaternion.IDENTITY)];
        GameObject?[] output = [fixture.Script.GameObject, fixture.Script.GameObject];
        s_count = 2; s_status = BridgeStatus.CAPACITY_LIMIT; s_calls = 0;
        BatchResult failed = fixture.Script.Scene.CreateMany("Enemy", poses, output);
        Program.Check(failed.Status == BatchStatus.CAPACITY_LIMIT && s_calls == 1 && ReferenceEquals(output[0], fixture.Script.GameObject)
            && ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, fixture.Scope), "Mutation failures cannot replay or modify caller output.");
        s_status = BridgeStatus.OK; s_calls = 0;
        BatchResult result = fixture.Script.Scene.CreateMany("Enemy", poses, output);
        Program.Check(result.Succeeded && result.AppliedCount == 2 && s_calls == 1 && s_scopeSuspended && output[0]!.ObjectId == 300,
            "Bulk creation must suspend outer managed authority while dispatching once.");
        s_calls = 0;
        poses[1] = new SpawnPose(new Vector3(float.NaN, 0, 0), Quaternion.IDENTITY);
        Program.Check(fixture.Script.Scene.CreateMany("Enemy", poses, output).Status == BatchStatus.INVALID_ARGUMENT && s_calls == 0,
            "Every pose must pass validation before dispatch.");
    }
    static void LargeCollections()
    {
        using Fixture fixture = new();
        const int COUNT = 5001;
        s_count = COUNT; s_status = BridgeStatus.OK; s_calls = 0;
        GameObject?[] output = new GameObject?[COUNT + 1]; output[^1] = fixture.Script.GameObject;
        GameObject?[] small = [fixture.Script.GameObject];
        ObjectQueryResult insufficient = fixture.Script.Scene.ReadObjects(small);
        Program.Check(insufficient.Status == BatchStatus.BUFFER_TOO_SMALL && insufficient.RequiredCapacity == COUNT
            && ReferenceEquals(small[0], fixture.Script.GameObject), "Large query capacity failures must report the whole collection without publishing a prefix.");
        ObjectQueryResult result = fixture.Script.Scene.ReadObjects(output);
        Program.Check(result.Status == BatchStatus.OK && result.Count == COUNT && output[COUNT - 1]!.ObjectId == 300UL + COUNT - 1
            && ReferenceEquals(output[^1], fixture.Script.GameObject), "Caller-buffer queries must decode every unique identity beyond4096 and preserve the tail.");
        GameObject? first = output[0]; GameObject? last = output[COUNT - 1];
        long before = GC.GetAllocatedBytesForCurrentThread();
        for (int index = 0; index < 8; index++) fixture.Script.Scene.ReadObjects(output);
        long bytes = GC.GetAllocatedBytesForCurrentThread() - before;
        Program.Check(bytes == 0 && ReferenceEquals(output[0], first) && ReferenceEquals(output[COUNT - 1], last),
            "A warmed large query must retain its whole reference cache without transfer allocation.");
        s_calls = 0;
        GameObject[] owned = fixture.Script.Scene.FindAll();
        Program.Check(owned.Length == COUNT && owned[^1].ObjectId == last!.ObjectId && s_calls == 2,
            "Owned-array queries must resize once and return every identity beyond4096.");
        SpawnPose[] poses = new SpawnPose[COUNT]; Array.Fill(poses, new SpawnPose(Vector3.ZERO, Quaternion.IDENTITY));
        s_calls = 0;
        BatchResult created = fixture.Script.Scene.CreateMany("Enemy", poses, output);
        Program.Check(created.Succeeded && created.AppliedCount == COUNT && s_calls == 1 && s_scopeSuspended
            && output[COUNT - 1]!.ObjectId == last!.ObjectId && ReferenceEquals(output[^1], fixture.Script.GameObject),
            "Bulk creation beyond4096 must dispatch once and preserve unused caller slots.");
        s_status = BridgeStatus.CAPACITY_LIMIT; s_calls = 0; first = output[0];
        BatchResult failed = fixture.Script.Scene.CreateMany("Enemy", poses, output);
        Program.Check(failed.Status == BatchStatus.CAPACITY_LIMIT && failed.AppliedCount == 0 && s_calls == 1
            && ReferenceEquals(output[0], first), "A native resource failure must not be replayed or publish caller output.");
        s_status = BridgeStatus.OK;
    }
    static void InvalidCounts()
    {
        using Fixture fixture = new();
        s_status = BridgeStatus.OK; s_count = uint.MaxValue; s_truncated = true;
        GameObject?[] output = [fixture.Script.GameObject];
        try
        {
            Program.Throws<InvalidDataException>(() => fixture.Script.Scene.ReadObjects(output));
            Program.Throws<InvalidDataException>(() => fixture.Script.Scene.FindAll());
            s_count = 1;
            s_unrepresentableBytes = true;
            Program.Throws<InvalidDataException>(() => fixture.Script.Scene.ReadObjects(output));
            Program.Throws<InvalidDataException>(() => fixture.Script.Scene.FindAll());
            s_unrepresentableBytes = false;
            Program.Throws<InvalidDataException>(() => fixture.Script.Scene.ReadObjects(output));
            Program.Throws<InvalidDataException>(() => fixture.Script.Scene.FindAll());
            Program.Check(ReferenceEquals(output[0], fixture.Script.GameObject), "Unrepresentable and truncated counts must never publish caller output.");
        }
        finally { s_truncated = false; s_unrepresentableBytes = false; }
    }
    static void Pool()
    {
        using Fixture fixture = new();
        Program.Throws<ArgumentOutOfRangeException>(() => fixture.Script.Scene.CreatePool("Enemy", -1));
        s_count = 5001; s_status = BridgeStatus.OK;
        using ObjectPool pool = fixture.Script.Scene.CreatePool("Enemy", 5001);
        Program.Check(pool.Capacity == 5001, "Pool capacity must follow successfully created objects beyond4096.");
        GameObject? rented = pool.Rent();
        Program.Check(rented is not null && pool.Return(rented), "A large pool must retain normal rental and return semantics.");
    }
    static void QueryCache()
    {
        using Fixture fixture = new();
        const int COUNT = 64;
        s_status = BridgeStatus.OK; s_count = COUNT;
        GameObject?[] output = new GameObject?[COUNT];
        Dictionary<ulong, GameObject> cache = (Dictionary<ulong, GameObject>)fixture.Bridge.GetType()
            .GetField("m_objectQueryCache", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(fixture.Bridge)!;
        for (int round = 0; round < 96; round++)
        {
            s_firstId = 300UL + (ulong)round * COUNT;
            Program.Check(fixture.Script.Scene.ReadObjects(output).Count == COUNT && cache.Count == COUNT
                && cache.ContainsKey(s_firstId) && !cache.ContainsKey(s_firstId + COUNT),
                "Same-scene churn must retain only the latest live result while transferring every replacement.");
        }
        ulong firstId = s_firstId;
        GameObject first = output[0]!; GameObject last = output[^1]!;
        s_firstId += COUNT; s_trailingBytes = true;
        try
        {
            Program.Throws<InvalidDataException>(() => fixture.Script.Scene.ReadObjects(output));
            Program.Check(cache.Count == COUNT && ReferenceEquals(cache[firstId], first) && !cache.ContainsKey(s_firstId)
                && ReferenceEquals(output[0], first) && ReferenceEquals(output[^1], last),
                "A late malformed row set must preserve both the successful reference cache and all caller output.");
        }
        finally { s_trailingBytes = false; }
        s_firstId = firstId + 1; s_count = 1;
        Program.Check(fixture.Script.Scene.ReadObjects(output, name: "subset").Count == 1 && cache.Count == 1 && !cache.ContainsKey(firstId),
            "A subset query must retire references absent from that successful result.");
        s_firstId = firstId; s_count = COUNT;
        Program.Check(fixture.Script.Scene.ReadObjects(output).Count == COUNT && output[0] == first && !ReferenceEquals(output[0], first),
            "Returning to a broader query may recreate a wrapper while preserving its immutable lifetime identity.");
        first = output[0]!;
        int calls = s_calls;
        long before = GC.GetAllocatedBytesForCurrentThread();
        for (int repeat = 0; repeat < 8; repeat++) fixture.Script.Scene.ReadObjects(output);
        long bytes = GC.GetAllocatedBytesForCurrentThread() - before;
        Program.Check(bytes == 0 && s_calls - calls == 8 && ReferenceEquals(output[0], first) && cache.Count == COUNT,
            "Repeated stable queries after churn must reuse their bounded cache with one dispatch and no managed allocation.");
        s_count = 0;
        Program.Check(fixture.Script.Scene.ReadObjects(output).Count == 0 && cache.Count == 0 && ReferenceEquals(output[0], first),
            "An empty successful result must release every cached wrapper without touching unused caller slots.");
    }
    [UnmanagedCallersOnly(CallConvs = new Type[] { typeof(CallConvCdecl) })]
    static int Dispatch(BridgeRequest* request, BridgeReply* reply)
    {
        ++s_calls; *reply = default;
        reply->Status = s_status; reply->Index = s_count; reply->CallId = 42;
        s_scopeSuspended = ScriptApiRuntime.Current is null;
        if (s_status != BridgeStatus.OK) return (int)s_status;
        if (s_numericPacket && request->Op == BridgeOp.OBJECT_WRITE) {
            s_numericWireValid &= request->InputBytes == 8 && request->Input is not null
                && BinaryPrimitives.ReadInt64LittleEndian(new ReadOnlySpan<byte>(request->Input, 8)) is 0 or 1;
        }
        if (s_textReply is byte[] text && request->Op == BridgeOp.OBJECT_READ) {
            reply->OutputBytes = (uint)text.Length;
            if (request->OutputCapacity < text.Length) { reply->Status = BridgeStatus.BUFFER_TOO_SMALL; return (int)reply->Status; }
            text.AsSpan().CopyTo(new Span<byte>(request->Output, text.Length)); return 0;
        }
        if (request->Op != BridgeOp.SCENE_QUERY && request->Op != BridgeOp.SCENE_SPAWN_MANY)
        {
            reply->Integer = request->Op == BridgeOp.OBJECTS_DESTROY && request->Flags >> 8 == 2
                ? request->Index + request->InputBytes / 16 : 1;
            return 0;
        }
        uint size = s_unrepresentableBytes ? uint.MaxValue : s_truncated ? 4 : checked(4 + s_count * (12 + (uint)s_guid.Length));
        if (s_trailingBytes) size++;
        reply->OutputBytes = size;
        if (request->OutputCapacity < size) { reply->Status = BridgeStatus.BUFFER_TOO_SMALL; return (int)reply->Status; }
        Span<byte> bytes = new(request->Output, (int)size); BinaryPrimitives.WriteUInt32LittleEndian(bytes, s_count); int offset = 4;
        if (s_truncated) return 0;
        for (uint index = 0; index < s_count; index++)
        {
            BinaryPrimitives.WriteUInt64LittleEndian(bytes[offset..], s_firstId + index); offset += 8;
            BinaryPrimitives.WriteUInt32LittleEndian(bytes[offset..], (uint)s_guid.Length); offset += 4;
            s_guid.CopyTo(bytes[offset..]); offset += s_guid.Length;
        }
        return 0;
    }
    sealed class Probe : Script { }
    sealed class Lookup : IScriptComponentLookup
    { public Component[] GetComponents(ScriptExecutionScope scope, GameObject target, Type type) => []; }
    sealed class Fixture : IDisposable
    {
        internal readonly Script Script;
        internal readonly ScriptExecutionScope Scope;
        internal readonly IScriptNativeBridge Bridge;
        internal Fixture()
        {
            Type type = typeof(EntryPoint).GetNestedType("NativeBridge", BindingFlags.NonPublic)!;
            nint address = (nint)(delegate* unmanaged[Cdecl]<BridgeRequest*, BridgeReply*, int>)&Dispatch;
            Bridge = (IScriptNativeBridge)Activator.CreateInstance(type, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic, null, [address], null)!;
            ScriptBinding binding = new(1, 7, 101) { GameObject = new GameObject(202, 42, Encoding.UTF8.GetString(s_guid)) };
            Script = new Probe { Binding = binding };
            Scope = new ScriptExecutionScope(Bridge, new Lookup(), 1, 7, 101, 8, 42, ScriptCallback.UPDATE);
            s_firstId = 300; s_trailingBytes = false;
            ScriptApiRuntime.Current = Scope;
        }
        public void Dispose() => ScriptApiRuntime.Current = null;
    }
}
