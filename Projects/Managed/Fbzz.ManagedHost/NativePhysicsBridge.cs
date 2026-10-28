/// @file    NativePhysicsBridge.cs
/// @brief   物理の借用引数と寿命付き query 応答を固定 ABI へ接続する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Buffers.Binary;
using System.IO;
using Fbzz;

namespace Fbzz.ManagedHost;

public static unsafe partial class EntryPoint
{
    sealed partial class NativeBridge
    {
        public void PhysicsBodyCommand(ScriptExecutionScope scope, ulong objectId, uint command, Vector3 value, Vector3 point, uint axes)
        {
            byte* input = stackalloc byte[12];
            BinaryPrimitives.WriteSingleLittleEndian(new Span<byte>(input, 4), point.X);
            BinaryPrimitives.WriteSingleLittleEndian(new Span<byte>(input + 4, 4), point.Y);
            BinaryPrimitives.WriteSingleLittleEndian(new Span<byte>(input + 8, 4), point.Z);
            BridgeRequest request = Request(scope, objectId, BridgeOp.PHYSICS_BODY_COMMAND);
            request.Index = command; request.Flags = axes;
            request.X = value.X; request.Y = value.Y; request.Z = value.Z;
            if (command == (uint)Fbzz.PhysicsBodyCommand.FORCE_AT_POINT || command == (uint)Fbzz.PhysicsBodyCommand.POINT_IMPULSE)
            { request.Input = input; request.InputBytes = 12; }
            Call(ref request);
        }

        public bool Raycast(ScriptExecutionScope scope, Vector3 origin, Vector3 direction, float maxDistance, out RaycastHit hit)
        {
            RaycastHit[] hits = Cast(scope, BridgeOp.PHYSICS_RAYCAST, origin, direction, maxDistance, 0f);
            hit = hits.Length == 0 ? default : hits[0];
            return hits.Length != 0;
        }

        public bool SphereCast(ScriptExecutionScope scope, Vector3 center, float radius, Vector3 direction, float maxDistance, out RaycastHit hit)
        {
            RaycastHit[] hits = Cast(scope, BridgeOp.PHYSICS_SPHERE_CAST, center, direction, maxDistance, radius);
            hit = hits.Length == 0 ? default : hits[0];
            return hits.Length != 0;
        }

        public RaycastHit[] RaycastAll(ScriptExecutionScope scope, Vector3 origin, Vector3 direction, float maxDistance)
            => Cast(scope, BridgeOp.PHYSICS_RAYCAST_ALL, origin, direction, maxDistance, 0f);

        public bool RaycastFiltered(ScriptExecutionScope scope, Vector3 origin, Vector3 direction, float maxDistance, PhysicsQueryOptions options, out RaycastHit hit)
        {
            RaycastHit[] hits = Cast(scope, BridgeOp.PHYSICS_RAYCAST_FILTERED, origin, direction, maxDistance, 0f, options);
            hit = hits.Length == 0 ? default : hits[0]; return hits.Length != 0;
        }
        public bool SphereCastFiltered(ScriptExecutionScope scope, Vector3 center, float radius, Vector3 direction, float maxDistance, PhysicsQueryOptions options, out RaycastHit hit)
        {
            RaycastHit[] hits = Cast(scope, BridgeOp.PHYSICS_SPHERE_CAST_FILTERED, center, direction, maxDistance, radius, options);
            hit = hits.Length == 0 ? default : hits[0]; return hits.Length != 0;
        }
        public RaycastHit[] RaycastAllFiltered(ScriptExecutionScope scope, Vector3 origin, Vector3 direction, float maxDistance, PhysicsQueryOptions options)
            => Cast(scope, BridgeOp.PHYSICS_RAYCAST_ALL_FILTERED, origin, direction, maxDistance, 0f, options);

        RaycastHit[] Cast(ScriptExecutionScope scope, BridgeOp operation, Vector3 origin, Vector3 direction, float distance, float radius,
            PhysicsQueryOptions? options = null)
        {
            bool sphere = operation == BridgeOp.PHYSICS_SPHERE_CAST || operation == BridgeOp.PHYSICS_SPHERE_CAST_FILTERED;
            bool all = operation == BridgeOp.PHYSICS_RAYCAST_ALL || operation == BridgeOp.PHYSICS_RAYCAST_ALL_FILTERED;
            int baseBytes = sphere ? 16 : 12;
            byte[] input = new byte[baseBytes + (options.HasValue ? 16 : 0)];
            BinaryPrimitives.WriteSingleLittleEndian(input, direction.X);
            BinaryPrimitives.WriteSingleLittleEndian(input.AsSpan(4), direction.Y);
            BinaryPrimitives.WriteSingleLittleEndian(input.AsSpan(8), direction.Z);
            if (sphere) BinaryPrimitives.WriteSingleLittleEndian(input.AsSpan(12), radius);
            if (options.HasValue) WriteOptions(scope, input.AsSpan(baseBytes), options.Value);
            fixed (byte* source = input)
            {
                BridgeRequest request = Request(scope, 0, operation);
                request.Input = source; request.InputBytes = (uint)input.Length;
                request.X = origin.X; request.Y = origin.Y; request.Z = origin.Z; request.W = distance;
                Span<byte> local = stackalloc byte[256]; NativeReadSnapshot snapshot = new(this, scope, ref request, local);
                try {
                    int count = PhysicsCount(ref snapshot, true);
                    if (!all && count > 1) throw new InvalidDataException("A single cast returned multiple hits.");
                    RaycastHit[] result = new RaycastHit[count];
                    for (int index = 0; index < result.Length; index++) result[index] = PhysicsHit(ref snapshot, scope, distance);
                    if (snapshot.Remaining != 0) throw new InvalidDataException("Physics reply has trailing bytes.");
                    return result;
                } finally { snapshot.Dispose(); }
            }
        }

        public GameObject[] OverlapSphere(ScriptExecutionScope scope, Vector3 center, float radius)
            => Overlap(scope, center, radius, null);

        public GameObject[] OverlapSphereFiltered(ScriptExecutionScope scope, Vector3 center, float radius, PhysicsQueryOptions options)
            => Overlap(scope, center, radius, options);

        static void WriteOptions(ScriptExecutionScope scope, Span<byte> bytes, PhysicsQueryOptions options)
        {
            if ((uint)options.Triggers > 2 || (options.Ignore is not null && options.Ignore.SceneLifetime != scope.SceneLifetime))
                throw new ArgumentException("Invalid physics query options.", nameof(options));
            BinaryPrimitives.WriteUInt32LittleEndian(bytes, options.EffectiveLayerMask);
            BinaryPrimitives.WriteUInt32LittleEndian(bytes[4..], (uint)options.Triggers);
            BinaryPrimitives.WriteUInt64LittleEndian(bytes[8..], options.Ignore?.ObjectId ?? 0);
        }

        GameObject[] Overlap(ScriptExecutionScope scope, Vector3 center, float radius, PhysicsQueryOptions? options)
        {
            byte* input = stackalloc byte[16];
            BridgeRequest request = Request(scope, 0, options.HasValue ? BridgeOp.PHYSICS_OVERLAP_SPHERE_FILTERED : BridgeOp.PHYSICS_OVERLAP_SPHERE);
            if (options.HasValue) { WriteOptions(scope, new Span<byte>(input, 16), options.Value); request.Input = input; request.InputBytes = 16; }
            request.X = center.X; request.Y = center.Y; request.Z = center.Z; request.W = radius;
            Span<byte> local = stackalloc byte[256]; NativeReadSnapshot snapshot = new(this, scope, ref request, local);
            try {
                int count = PhysicsCount(ref snapshot, false); GameObject[] result = new GameObject[count];
                for (int index = 0; index < count; index++) result[index] = PhysicsObject(ref snapshot, snapshot.UInt64(), scope.SceneLifetime);
                if (snapshot.Remaining != 0) throw new InvalidDataException("Overlap reply has trailing bytes.");
                return result;
            } finally { snapshot.Dispose(); }
        }

        static int PhysicsCount(ref NativeReadSnapshot snapshot, bool hits)
        {
            uint count = snapshot.UInt32();
            if (count != snapshot.Count || count > int.MaxValue || (ulong)count * (hits ? 40U : 12U) > snapshot.Remaining)
                throw new InvalidDataException("Physics query count disagrees with its full snapshot.");
            return (int)count;
        }
        static RaycastHit PhysicsHit(ref NativeReadSnapshot snapshot, ScriptExecutionScope scope, float distance)
        {
            ulong id = snapshot.UInt64(); Vector3 point = new(snapshot.Single(), snapshot.Single(), snapshot.Single());
            Vector3 normal = new(snapshot.Single(), snapshot.Single(), snapshot.Single()); float hitDistance = snapshot.Single();
            GameObject target = PhysicsObject(ref snapshot, id, scope.SceneLifetime);
            if (!point.IsFinite || !normal.IsFinite || !float.IsFinite(hitDistance) || hitDistance < 0 || hitDistance > distance)
                throw new InvalidDataException("Physics hit contains invalid values.");
            return new(target, point, normal, hitDistance);
        }
        static GameObject PhysicsObject(ref NativeReadSnapshot snapshot, ulong objectId, ulong lifetime)
        {
            if (objectId == 0) throw new InvalidDataException("Physics object identity is invalid.");
            return new(objectId, lifetime, ScriptFieldCodec.ValidateGuid(snapshot.Text()));
        }
    }
}
