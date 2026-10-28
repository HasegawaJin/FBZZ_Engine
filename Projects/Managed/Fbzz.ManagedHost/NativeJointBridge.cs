/// @file    NativeJointBridge.cs
/// @brief   Chain の全 targets を検証してから同期候補を一度だけ publish する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
using System.Buffers;
using System.Buffers.Binary;
using System.Globalization;
using System.IO;
using Fbzz;
namespace Fbzz.ManagedHost;
public static unsafe partial class EntryPoint
{
    sealed partial class NativeBridge
    {
        public bool ConnectJointChain(ScriptExecutionScope scope, ulong objectId, ReadOnlySpan<GameObject> targets, float distance, int iterations)
        {
            if (scope.Callback != ScriptCallback.FIXED_UPDATE) throw new InvalidOperationException("Joint changes require FixedUpdate.");
            if (!float.IsFinite(distance) || iterations < 1) throw new ArgumentOutOfRangeException(nameof(iterations));
            if (targets.IsEmpty) return false;
            foreach (GameObject target in targets) if (target is null || target.SceneLifetime != scope.SceneLifetime) throw new ArgumentException("Every chain target must belong to this Scene.", nameof(targets));
            long handle = InvokeService(scope, objectId, 200, 19, targets.Length, [distance], [iterations.ToString(CultureInfo.InvariantCulture)], null, false).Integer;
            if (handle <= 0) throw new InvalidDataException("Invalid Joint candidate handle.");
            ulong token = (ulong)handle; byte[] rented = ArrayPool<byte>.Shared.Rent(checked((int)BridgeAbi.MAX_BATCH_PACKET_BYTES));
            try {
                Span<byte> packet = rented.AsSpan(0, checked((int)BridgeAbi.MAX_BATCH_PACKET_BYTES)); int offset = 0;
                while (offset < targets.Length) {
                    int count = Math.Min((packet.Length - 24) / 16, targets.Length - offset);
                    BinaryPrimitives.WriteUInt64LittleEndian(packet, token); BinaryPrimitives.WriteUInt64LittleEndian(packet[8..], (ulong)offset);
                    BinaryPrimitives.WriteUInt32LittleEndian(packet[16..], (uint)count); BinaryPrimitives.WriteUInt32LittleEndian(packet[20..], 0);
                    for (int index = 0; index < count; index++) {
                        GameObject target = targets[offset + index]; Span<byte> row = packet[(24 + index * 16)..];
                        BinaryPrimitives.WriteUInt64LittleEndian(row, target.ObjectId); BinaryPrimitives.WriteUInt64LittleEndian(row[8..], target.SceneLifetime);
                    }
                    BridgeReply reply = StorePacket(scope, objectId, 200, 20, packet[..(24 + count * 16)]); offset += count;
                    if (reply.Integer != offset) throw new InvalidDataException("Joint candidate acknowledged another offset.");
                }
                BinaryPrimitives.WriteUInt64LittleEndian(packet, token); BridgeReply committed = StorePacket(scope, objectId, 200, 22, packet[..8]); token = 0;
                if (committed.Integer is not (0 or 1)) throw new InvalidDataException("Invalid Joint commit acknowledgment.");
                return committed.Integer != 0;
            } finally {
                if (token != 0) { try { BinaryPrimitives.WriteUInt64LittleEndian(rented, token); _ = StorePacket(scope, objectId, 200, 21, rented.AsSpan(0, 8)); } catch { } }
                ArrayPool<byte>.Shared.Return(rented, true);
            }
        }
    }
}
