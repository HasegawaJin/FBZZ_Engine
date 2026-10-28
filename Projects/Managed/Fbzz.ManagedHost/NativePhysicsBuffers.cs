/// @file    NativePhysicsBuffers.cs
/// @brief   物理 snapshot を候補 buffer に復号し、全体成功時だけ caller に公開する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
using System.Buffers;
using System.Buffers.Binary;
using System.IO;
using Fbzz;
namespace Fbzz.ManagedHost;
public static unsafe partial class EntryPoint
{
    sealed partial class NativeBridge
    {
        public CollectionReadResult ReadRaycastAll(ScriptExecutionScope scope, Vector3 origin, Vector3 direction, float distance,
            Span<RaycastHit> output, PhysicsQueryOptions? options)
        {
            Span<byte> input = stackalloc byte[28]; BinaryPrimitives.WriteSingleLittleEndian(input, direction.X);
            BinaryPrimitives.WriteSingleLittleEndian(input[4..], direction.Y); BinaryPrimitives.WriteSingleLittleEndian(input[8..], direction.Z);
            if (options.HasValue) WriteOptions(scope, input[12..], options.Value);
            fixed (byte* source = input) {
                BridgeRequest request = Request(scope, 0, options.HasValue ? BridgeOp.PHYSICS_RAYCAST_ALL_FILTERED : BridgeOp.PHYSICS_RAYCAST_ALL);
                request.Input = source; request.InputBytes = options.HasValue ? 28U : 12U; request.X = origin.X; request.Y = origin.Y; request.Z = origin.Z; request.W = distance;
                Span<byte> local = stackalloc byte[256]; NativeReadSnapshot snapshot = new(this, scope, ref request, local); RaycastHit[]? rented = null;
                try {
                    int count = PhysicsCount(ref snapshot, true); if (output.Length < count) return new(count, 0);
                    rented = ArrayPool<RaycastHit>.Shared.Rent(Math.Max(1, count));
                    for (int index = 0; index < count; index++) rented[index] = PhysicsHit(ref snapshot, scope, distance);
                    if (snapshot.Remaining != 0) throw new InvalidDataException("Physics snapshot has trailing bytes.");
                    rented.AsSpan(0, count).CopyTo(output); return new(count, count);
                } finally { snapshot.Dispose(); if (rented is not null) ArrayPool<RaycastHit>.Shared.Return(rented, true); }
            }
        }
        public CollectionReadResult ReadOverlapSphere(ScriptExecutionScope scope, Vector3 center, float radius,
            Span<GameObject> output, PhysicsQueryOptions? options)
        {
            Span<byte> input = stackalloc byte[16]; if (options.HasValue) WriteOptions(scope, input, options.Value);
            fixed (byte* source = input) {
                BridgeRequest request = Request(scope, 0, options.HasValue ? BridgeOp.PHYSICS_OVERLAP_SPHERE_FILTERED : BridgeOp.PHYSICS_OVERLAP_SPHERE);
                if (options.HasValue) { request.Input = source; request.InputBytes = 16; }
                request.X = center.X; request.Y = center.Y; request.Z = center.Z; request.W = radius;
                Span<byte> local = stackalloc byte[256]; NativeReadSnapshot snapshot = new(this, scope, ref request, local); GameObject[]? rented = null;
                try {
                    int count = PhysicsCount(ref snapshot, false); if (output.Length < count) return new(count, 0);
                    rented = ArrayPool<GameObject>.Shared.Rent(Math.Max(1, count));
                    for (int index = 0; index < count; index++) rented[index] = PhysicsObject(ref snapshot, snapshot.UInt64(), scope.SceneLifetime);
                    if (snapshot.Remaining != 0) throw new InvalidDataException("Overlap snapshot has trailing bytes.");
                    rented.AsSpan(0, count).CopyTo(output); return new(count, count);
                } finally { snapshot.Dispose(); if (rented is not null) ArrayPool<GameObject>.Shared.Return(rented, true); }
            }
        }
    }
}
