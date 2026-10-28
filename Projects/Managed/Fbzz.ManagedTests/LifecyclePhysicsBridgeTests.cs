/// @file    LifecyclePhysicsBridgeTests.cs
/// @brief   実 NativeBridge の生成権限停止・変更再送禁止・物理応答と更新 phase を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Buffers.Binary;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static unsafe class LifecyclePhysicsBridgeTests
{
    public static (string Name, Action Test)[] Cases =>
    [
        ("NativeCreationSuspendsAndRestoresScope", CreationScope),
        ("LifecycleAndPhysicsMutationsNeverReplay", MutationsNeverReplay),
        ("PhysicsQueriesPreserveWireAndReadonlyRetry", QueryWireAndRetry),
        ("PhysicsQueriesRejectInvalidReplies", InvalidQueryReplies),
        ("PhysicsQueriesPageLargeSnapshotsAndProtectCallerBuffers", LargeQueryBuffers),
        ("RigidBodyMutationsRequireFixedUpdate", FixedUpdateOnly)
    ];

    const string TARGET_GUID = "01234567-89ab-cdef-0123-456789abcdef";
    static readonly byte[] s_guid = Encoding.UTF8.GetBytes(TARGET_GUID);
    static byte[] s_response = [];
    static byte[] s_input = [];
    static BridgeStatus s_status;
    static uint s_count;
    static uint? s_outputBytes;
    static ulong s_scene;
    static int s_dispatches;
    static BridgeRequest s_request;
    static bool s_wireValid;
    static bool s_scopeSuspended;
    static bool s_scopeAccessRejected;
    static ScriptExecutionScope? s_expectedScope;

    static void CreationScope()
    {
        using ApiFixture fixture = new();
        Reset();
        GameObject? created = fixture.Script.Scene.Create("日本語の生成", new Vector3(1f, 2f, 3f), Quaternion.IDENTITY);
        Program.Check(created is not null && created.ObjectId == 202 && created.SceneLifetime == 42
            && created.Guid == TARGET_GUID && s_dispatches == 1 && s_wireValid && s_scopeSuspended && s_scopeAccessRejected,
            "Creation must suspend managed Engine access while preserving caller IDs and the returned lifetime reference.");
        Program.Check(ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, fixture.Scope)
            && s_request.Op == BridgeOp.SCENE_CREATE && s_request.Flags == 1
            && s_request.X == 1f && s_request.Y == 2f && s_request.Z == 3f
            && s_input.Length > 16 && BinaryPrimitives.ReadSingleLittleEndian(s_input.AsSpan(12)) == 1f
            && Encoding.UTF8.GetString(s_input.AsSpan(16)) == "日本語の生成",
            "The pose prefix and UTF-8 name must survive one native call, and the original callback scope must return.");

        Reset();
        GameObject? prefab = fixture.Script.Scene.Instantiate("Assets/Prefabs/Test.prefab");
        Program.Check(prefab is not null && s_request.Op == BridgeOp.SCENE_INSTANTIATE && s_request.Flags == 0
            && Encoding.UTF8.GetString(s_input) == "Assets/Prefabs/Test.prefab"
            && s_scopeSuspended && s_scopeAccessRejected && ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, fixture.Scope),
            "Prefab reconstruction must use the same temporary authority suspension as creation.");

        Reset(BridgeStatus.CAPACITY_LIMIT);
        Program.Check(fixture.Script.Scene.Create() is null && s_dispatches == 1
            && ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, fixture.Scope), "Capacity failure must restore scope without retry.");
        Reset(BridgeStatus.NOT_FOUND);
        Program.Check(fixture.Script.Scene.Instantiate("missing.prefab") is null && s_dispatches == 1
            && ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, fixture.Scope), "A missing prefab must restore scope without retry.");
        Reset(); s_scene = 99;
        Program.Throws<InvalidDataException>(() => fixture.Script.Scene.Create());
        Program.Check(s_dispatches == 1 && ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, fixture.Scope),
            "Malformed successful creation replies must restore scope before validation fails.");
        Reset();
        ScriptExecutionScope reconstructed = fixture.MakeScope(ScriptCallback.UPDATE);
        Program.Throws<InvalidOperationException>(() => fixture.Bridge.CreateObject(reconstructed, "WrongWitness", Vector3.ZERO, Quaternion.IDENTITY, false));
        Program.Check(s_dispatches == 0 && ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, fixture.Scope),
            "Equal native creation IDs cannot replace the current immutable callback witness.");
        Reset(); ScriptApiRuntime.Current = null;
        Program.Throws<InvalidOperationException>(() => fixture.Bridge.CreateObject(fixture.Scope, "NoScope", Vector3.ZERO, Quaternion.IDENTITY, false));
        Program.Check(s_dispatches == 0, "Creation cannot borrow a scope that is not currently active.");
    }

    static void MutationsNeverReplay()
    {
        using ApiFixture fixture = new();
        Action[] mutations =
        [
            () => fixture.Bridge.CreateObject(fixture.Scope, "A", Vector3.ZERO, Quaternion.IDENTITY, false),
            () => fixture.Bridge.InstantiateObject(fixture.Scope, "A.prefab", Vector3.ZERO, Quaternion.IDENTITY, false),
            () => fixture.Bridge.DestroyObject(fixture.Scope, 202, 0f),
            () => fixture.Bridge.PhysicsBodyCommand(fixture.Scope, 202, (uint)PhysicsBodyCommand.FORCE_AT_POINT,
                Vector3.RIGHT, new Vector3(2f, 3f, 4f), 0),
            () => fixture.Bridge.WritePhysicsVector(fixture.Scope, 202, (uint)PhysicsBodyValue.VELOCITY, Vector3.RIGHT),
            () => fixture.Bridge.WritePhysicsScalar(fixture.Scope, 202, (uint)PhysicsBodyValue.MASS, 2f),
            () => fixture.Bridge.WritePhysicsFlag(fixture.Scope, 202, (uint)PhysicsBodyValue.IS_STATIC, true)
        ];
        foreach (Action mutation in mutations)
        {
            Reset(BridgeStatus.BUFFER_TOO_SMALL); s_outputBytes = 1024;
            Program.Throws<InvalidOperationException>(mutation);
            Program.Check(s_dispatches == 1 && s_wireValid && ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, fixture.Scope),
                "BUFFER_TOO_SMALL must never replay creation, destruction or a physics mutation.");
        }
        Reset(BridgeStatus.BUFFER_TOO_SMALL); s_outputBytes = 1024;
        GameObject?[] objects = [fixture.Target, fixture.Target, null];
        BatchResult result = fixture.Script.Scene.DestroyMany(objects, 0.5f);
        Program.Check(result.Status == BatchStatus.BUFFER_TOO_SMALL && result.AppliedCount == 0 && result.SkippedCount == 0
            && s_dispatches == 1 && s_request.Op == BridgeOp.OBJECTS_DESTROY && s_request.Index == 3
            && s_request.X == 0.5f && s_input.Length == 3 * BridgeAbi.BATCH_REFERENCE_BYTES
            && BinaryPrimitives.ReadUInt64LittleEndian(s_input) == 202
            && BinaryPrimitives.ReadUInt64LittleEndian(s_input.AsSpan(8)) == 42
            && BinaryPrimitives.ReadUInt64LittleEndian(s_input.AsSpan(32)) == 0,
            "DestroyMany must return the original failure after one borrowed request, preserving null and duplicate slots.");
    }

    static void QueryWireAndRetry()
    {
        using ApiFixture fixture = new();
        Reset(); Query(Hits(1), 1);
        bool found = fixture.Script.Physics.Raycast(new Vector3(3f, 4f, 5f), Vector3.RIGHT, 10f, out RaycastHit hit);
        Program.Check(found && hit.GameObject.ObjectId == 202 && hit.GameObject.SceneLifetime == 42
            && hit.GameObject.Guid == TARGET_GUID && hit.Point.X == 1f && hit.Point.Y == 2f && hit.Point.Z == 3f
            && hit.Normal.X == 0f && hit.Normal.Y == 1f && hit.Normal.Z == 0f && hit.Distance == 1f
            && s_dispatches == 1 && s_wireValid && s_request.Op == BridgeOp.PHYSICS_RAYCAST
            && s_request.X == 3f && s_request.Y == 4f && s_request.Z == 5f && s_request.W == 10f
            && s_input.Length == 12 && BinaryPrimitives.ReadSingleLittleEndian(s_input) == 1f,
            "Raycast must retain world values, variable-length GUID and the caller's lifetime in one dispatch.");

        Reset(); Query(Hits(1), 1);
        Program.Check(fixture.Script.Physics.SphereCast(Vector3.ZERO, 2f, Vector3.UP, 10f, out _)
            && s_request.Op == BridgeOp.PHYSICS_SPHERE_CAST && s_input.Length == 16
            && BinaryPrimitives.ReadSingleLittleEndian(s_input.AsSpan(4)) == 1f
            && BinaryPrimitives.ReadSingleLittleEndian(s_input.AsSpan(12)) == 2f,
            "SphereCast must pass direction and radius through the explicit 16-byte borrowed payload.");

        Reset(); Query(Hits(4), 4);
        RaycastHit[] hits = fixture.Script.Physics.RaycastAll(Vector3.ZERO, Vector3.RIGHT, 10f);
        Program.Check(hits.Length == 4 && hits[0].GameObject.ObjectId == 202 && hits[3].GameObject.ObjectId == 205
            && hits[3].Distance == 4f && s_dispatches == 3 && s_wireValid,
            "Readonly capacity detection must page one captured query and close it, preserving every ordered hit.");
        Reset(); Query(Overlaps(2), 2);
        GameObject[] overlaps = fixture.Script.Physics.OverlapSphere(new Vector3(1f, 2f, 3f), 4f);
        Program.Check(overlaps.Length == 2 && overlaps[1].ObjectId == 203 && overlaps[1].Guid == TARGET_GUID
            && s_request.Op == BridgeOp.PHYSICS_OVERLAP_SPHERE && s_request.W == 4f && s_dispatches == 1,
            "OverlapSphere must decode lifetime references without per-object resolution calls.");
        Reset(); Query(Hits(0), 0);
        Program.Check(!fixture.Script.Physics.Raycast(Vector3.ZERO, Vector3.RIGHT, 10f, out _)
            && s_dispatches == 1, "A valid zero-count reply must produce no hit.");
    }

    static void InvalidQueryReplies()
    {
        using ApiFixture fixture = new();
        RejectCast(fixture, Hits(1)[..12], 1);
        RejectCast(fixture, Hits(1), 2);
        RejectCast(fixture, Hits(1), 1, 99);
        byte[] countLimit = new byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(countLimit, uint.MaxValue);
        RejectCast(fixture, countLimit, uint.MaxValue);
        foreach ((int offset, float value) in new (int, float)[]
        { (12, float.NaN), (24, float.PositiveInfinity), (36, float.NaN), (36, -1f), (36, 11f) })
        {
            byte[] malformed = Hits(1); BinaryPrimitives.WriteSingleLittleEndian(malformed.AsSpan(offset), value);
            RejectCast(fixture, malformed, 1);
        }
        byte[] truncatedGuid = Hits(1); BinaryPrimitives.WriteUInt32LittleEndian(truncatedGuid.AsSpan(40), 999);
        RejectCast(fixture, truncatedGuid, 1);
        byte[] trailing = new byte[Hits(1).Length + 1]; Hits(1).CopyTo(trailing, 0);
        RejectCast(fixture, trailing, 1);
        Reset(); Query(Hits(2), 2);
        Program.Throws<InvalidDataException>(() => fixture.Script.Physics.Raycast(Vector3.ZERO, Vector3.RIGHT, 10f, out _));
        Program.Check(s_dispatches == 1, "A single-hit cast must reject multiple hits.");
        byte[] invalidUtf8 = Hits(1); invalidUtf8[44] = 0xFF;
        Reset(); Query(invalidUtf8, 1);
        Program.Throws<DecoderFallbackException>(() => fixture.Script.Physics.RaycastAll(Vector3.ZERO, Vector3.RIGHT, 10f));
        Program.Check(s_dispatches == 1, "Invalid UTF-8 must not become a lossy GUID reference.");

        Reset(); Query(Overlaps(1)[..8], 1);
        Program.Throws<InvalidDataException>(() => fixture.Script.Physics.OverlapSphere(Vector3.ZERO, 1f));
        Reset(); Query(Overlaps(1), 2);
        Program.Throws<InvalidDataException>(() => fixture.Script.Physics.OverlapSphere(Vector3.ZERO, 1f));
        Reset(BridgeStatus.BUFFER_TOO_SMALL); s_outputBytes = uint.MaxValue;
        Program.Throws<InvalidOperationException>(() => fixture.Script.Physics.RaycastAll(Vector3.ZERO, Vector3.RIGHT, 10f));
        Program.Check(s_dispatches == 1, "An unbounded output request must be rejected before allocation or resend.");
        Reset(); s_outputBytes = 257;
        Program.Throws<InvalidDataException>(() => fixture.Script.Physics.OverlapSphere(Vector3.ZERO, 1f));
        Program.Check(s_dispatches == 1, "A success reply cannot exceed the caller's borrowed buffer.");
    }
    static void LargeQueryBuffers()
    {
        using ApiFixture fixture = new(); const int count = 32769;
        Reset(); Query(Hits(count), count);
        RaycastHit[] result = fixture.Script.Physics.RaycastAll(Vector3.ZERO, Vector3.RIGHT, count + 1);
        Program.Check(result.Length == count && result[^1].Distance == count && s_dispatches > 3,
            "A physics result beyond the former hit cap must page one full snapshot without clipping.");
        RaycastHit sentinel = result[0]; RaycastHit[] shortBuffer = new RaycastHit[count - 1]; Array.Fill(shortBuffer, sentinel);
        Reset(); Query(Hits(count), count);
        CollectionReadResult capacity = fixture.Script.Physics.RaycastAll(Vector3.ZERO, Vector3.RIGHT, count + 1, shortBuffer);
        Program.Check(capacity.Required == count && capacity.Written == 0 && shortBuffer[^1].Distance == sentinel.Distance,
            "Short physics buffers return required/written0 without publishing a prefix.");
        RaycastHit[] full = new RaycastHit[count + 2]; Array.Fill(full, sentinel);
        Reset(); Query(Hits(count), count);
        capacity = fixture.Script.Physics.RaycastAll(Vector3.ZERO, Vector3.RIGHT, count + 1, full);
        Program.Check(capacity.Written == count && full[count - 1].Distance == count && full[^1].Distance == sentinel.Distance,
            "Successful physics caller buffers preserve unused tail.");
        byte[] malformed = Hits(4); BinaryPrimitives.WriteSingleLittleEndian(malformed.AsSpan(4 + 3 * (40 + s_guid.Length) + 32), float.NaN);
        Reset(); Query(malformed, 4); RaycastHit[] untouched = new RaycastHit[4]; Array.Fill(untouched, sentinel);
        Program.Throws<InvalidDataException>(() => fixture.Script.Physics.RaycastAll(Vector3.ZERO, Vector3.RIGHT, 10, untouched));
        Program.Check(untouched[0].Distance == sentinel.Distance && untouched[^1].Distance == sentinel.Distance,
            "A malformed late hit must leave caller-owned output unchanged.");
    }

    static void FixedUpdateOnly()
    {
        using ApiFixture fixture = new();
        RigidBody body = new(fixture.Target);
        Action[] changes =
        [
            () => body.Velocity = Vector3.RIGHT,
            () => body.Mass = 2f,
            () => body.IsStatic = true,
            () => body.AddForceAtPoint(Vector3.UP, Vector3.RIGHT),
            () => body.Teleport(Vector3.ZERO),
            () => body.SetFreezePosition(true, false, true)
        ];
        foreach (ScriptCallback callback in new[] { ScriptCallback.UPDATE, ScriptCallback.LATE_UPDATE, ScriptCallback.ON_COLLISION_ENTER })
        {
            ScriptApiRuntime.Current = fixture.MakeScope(callback);
            foreach (Action change in changes)
            {
                Reset(); Program.Throws<InvalidOperationException>(change);
                Program.Check(s_dispatches == 0, "Body mutations outside FixedUpdate must fail before crossing the native boundary.");
            }
        }
        ScriptApiRuntime.Current = fixture.MakeScope(ScriptCallback.ON_COLLISION_ENTER);
        Reset(); Query(Hits(1), 1);
        Program.Check(fixture.Script.Physics.Raycast(Vector3.ZERO, Vector3.RIGHT, 10f, out _)
            && s_dispatches == 1, "Physics callbacks may query the world while deferring body changes to FixedUpdate.");
        ScriptApiRuntime.Current = fixture.MakeScope(ScriptCallback.FIXED_UPDATE);
        foreach (Action change in changes)
        {
            Reset(); change();
            Program.Check(s_dispatches == 1 && s_wireValid, "Valid FixedUpdate changes must dispatch exactly once.");
        }
        Reset();
        Program.Throws<ArgumentOutOfRangeException>(() => body.Velocity = new Vector3(float.NaN, 0f, 0f));
        Program.Throws<ArgumentOutOfRangeException>(() => body.Mass = float.Epsilon);
        Program.Check(s_dispatches == 0, "Non-finite velocity and an overflowing inverse mass must fail before dispatch.");
        ScriptApiRuntime.Current = null;
        Program.Throws<InvalidOperationException>(() => body.AddForce(Vector3.UP));
        Program.Check(s_dispatches == 0, "A cached rigid-body facade cannot retain callback authority.");
    }

    static void RejectCast(ApiFixture fixture, byte[] bytes, uint count, ulong scene = 42)
    {
        Reset(); Query(bytes, count); s_scene = scene;
        Program.Throws<InvalidDataException>(() => fixture.Script.Physics.RaycastAll(Vector3.ZERO, Vector3.RIGHT, 10f));
        Program.Check(s_dispatches == 1 && ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, fixture.Scope),
            "Invalid physics replies must not expose partial results or replace the current scope.");
    }

    static void Reset(BridgeStatus status = BridgeStatus.OK)
    {
        s_status = status; s_response = []; s_input = []; s_count = 0; s_scene = 42; s_outputBytes = null;
        s_dispatches = 0; s_request = default; s_wireValid = true; s_scopeSuspended = false; s_scopeAccessRejected = false;
        s_expectedScope = ScriptApiRuntime.Current;
    }
    static void Query(byte[] bytes, uint count) { s_response = bytes; s_count = count; }

    static byte[] Hits(int count)
    {
        byte[] bytes = new byte[checked(4 + count * ((int)BridgeAbi.PHYSICS_HIT_BYTES + s_guid.Length))];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, (uint)count);
        int offset = 4;
        for (int index = 0; index < count; index++)
        {
            BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(offset), (ulong)(202 + index));
            WriteVector(bytes.AsSpan(offset + 8), new Vector3(1f + index, 2f, 3f));
            WriteVector(bytes.AsSpan(offset + 20), Vector3.UP);
            BinaryPrimitives.WriteSingleLittleEndian(bytes.AsSpan(offset + 32), 1f + index);
            BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(offset + 36), (uint)s_guid.Length);
            s_guid.CopyTo(bytes, offset + 40); offset += 40 + s_guid.Length;
        }
        return bytes;
    }
    static byte[] Overlaps(int count)
    {
        byte[] bytes = new byte[checked(4 + count * (12 + s_guid.Length))];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, (uint)count);
        int offset = 4;
        for (int index = 0; index < count; index++)
        {
            BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(offset), (ulong)(202 + index));
            BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(offset + 8), (uint)s_guid.Length);
            s_guid.CopyTo(bytes, offset + 12); offset += 12 + s_guid.Length;
        }
        return bytes;
    }
    static void WriteVector(Span<byte> bytes, Vector3 value)
    {
        BinaryPrimitives.WriteSingleLittleEndian(bytes, value.X);
        BinaryPrimitives.WriteSingleLittleEndian(bytes[4..], value.Y);
        BinaryPrimitives.WriteSingleLittleEndian(bytes[8..], value.Z);
    }

    /// @note fake から managed 例外を unmanaged 境界へ出さず、観測値を戻った側で検証する。
    /// @see https://learn.microsoft.com/dotnet/api/system.runtime.interopservices.unmanagedcallersonlyattribute UnmanagedCallersOnly
    [UnmanagedCallersOnly(CallConvs = new Type[] { typeof(CallConvCdecl) })]
    static int Dispatch(BridgeRequest* request, BridgeReply* reply)
    {
        if (request->Op == BridgeOp.COMPONENT_READ && request->Flags == 1)
        { *reply = new BridgeReply { Status = BridgeStatus.OK, Integer = 91 }; return 0; }
        s_dispatches++; s_request = *request;
        s_input = new ReadOnlySpan<byte>(request->Input, (int)request->InputBytes).ToArray();
        s_wireValid &= request->AbiVersion == BridgeAbi.VERSION && request->Generation == 7 && request->InstanceId == 9
            && request->OwnerId == 101 && request->CallId == 13;
        bool creation = request->Op == BridgeOp.SCENE_CREATE || request->Op == BridgeOp.SCENE_INSTANTIATE;
        if (creation)
        {
            s_scopeSuspended = ScriptApiRuntime.Current is null;
            try { ScriptApiRuntime.RequireScope(); }
            catch (InvalidOperationException) { s_scopeAccessRejected = true; }
        }
        else s_wireValid &= ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, s_expectedScope);
        *reply = new BridgeReply { Status = s_status, CallId = s_scene, Index = s_count };
        if (request->Flags == 768) { reply->OutputBytes = 0; return 0; }
        if (request->Flags == 512) {
            ulong offset = ((ulong)request->Reserved1 << 32) | request->Reserved0;
            int bytes = Math.Min((int)request->OutputCapacity, s_response.Length - (int)offset);
            s_response.AsSpan((int)offset, bytes).CopyTo(new Span<byte>(request->Output, bytes));
            reply->OutputBytes = (uint)bytes; reply->Generation = (ulong)s_response.Length; reply->Integer = (long)offset + bytes;
            reply->Flags = offset + (uint)bytes == (ulong)s_response.Length ? 1U : 0U; return 0;
        }
        if (s_status != BridgeStatus.OK)
        {
            reply->OutputBytes = s_outputBytes ?? 0;
            return (int)s_status;
        }
        if (creation)
        {
            reply->ValueKind = ScriptFieldKind.GAME_OBJECT; reply->Integer = 202;
            reply->OutputBytes = (uint)s_guid.Length;
            if (request->OutputCapacity >= s_guid.Length) s_guid.CopyTo(new Span<byte>(request->Output, s_guid.Length));
            return 0;
        }
        reply->OutputBytes = s_outputBytes ?? (uint)s_response.Length;
        if (s_outputBytes is null && s_response.Length > request->OutputCapacity)
        {
            reply->Status = BridgeStatus.BUFFER_TOO_SMALL;
            reply->Integer = 77; reply->Generation = (ulong)s_response.Length;
            return (int)BridgeStatus.BUFFER_TOO_SMALL;
        }
        if (s_response.Length <= request->OutputCapacity)
            s_response.CopyTo(new Span<byte>(request->Output, s_response.Length));
        return 0;
    }

    sealed class OwnerScript : Script { }
    sealed class ApiFixture : IDisposable, IScriptComponentLookup
    {
        readonly ScriptExecutionScope? m_previous = ScriptApiRuntime.Current;
        internal ApiFixture()
        {
            Type type = typeof(EntryPoint).GetNestedType("NativeBridge", BindingFlags.NonPublic)
                ?? throw new InvalidOperationException("NativeBridge type was not found.");
            ConstructorInfo constructor = type.GetConstructor(BindingFlags.Instance | BindingFlags.NonPublic, null, [typeof(nint)], null)
                ?? throw new InvalidOperationException("NativeBridge constructor was not found.");
            nint address = (nint)(delegate* unmanaged[Cdecl]<BridgeRequest*, BridgeReply*, int>)&Dispatch;
            Bridge = (IScriptNativeBridge)constructor.Invoke([address]);
            ScriptBinding binding = new(7, 9, 101) { GameObject = new GameObject(101, 42, "owner") };
            Script = new OwnerScript { Binding = binding };
            Scope = MakeScope(ScriptCallback.UPDATE); ScriptApiRuntime.Current = Scope;
        }
        internal IScriptNativeBridge Bridge { get; }
        internal Script Script { get; }
        internal ScriptExecutionScope Scope { get; }
        internal GameObject Target { get; } = new(202, 42, TARGET_GUID);
        internal ScriptExecutionScope MakeScope(ScriptCallback callback) => new(Bridge, this, 7, 9, 101, 13, 42, callback);
        public Component[] GetComponents(ScriptExecutionScope scope, GameObject target, Type type) => [];
        public void Dispose() { ScriptApiRuntime.Current = m_previous; s_expectedScope = null; }
    }
}
