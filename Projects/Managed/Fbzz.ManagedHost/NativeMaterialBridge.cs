/// @file    NativeMaterialBridge.cs
/// @brief   Material 数値列の float64 packet・候補書込・不足時の caller buffer 保護。
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
        (ulong Token, int Count) OpenMaterialValues(ScriptExecutionScope scope, ulong objectId, int slot, string property)
        {
            NativeServiceResult result = InvokeService(scope, objectId, (uint)BridgeOp.SERVICE_MATERIAL, 30, slot, [], [property], null, true);
            if (result.Integer == 0) return (0, 0);
            if (result.Integer < 0 || result.Text.Length != 1 || !int.TryParse(result.Text[0], NumberStyles.None, CultureInfo.InvariantCulture, out int count) || count < 0)
                throw new InvalidDataException("Invalid material numeric snapshot metadata.");
            return ((ulong)result.Integer, count);
        }
        public bool ReadMaterialValues(ScriptExecutionScope scope, ulong objectId, int slot, string property,
            Span<double> output, out CollectionReadResult capacity)
        {
            var snapshot = OpenMaterialValues(scope, objectId, slot, property); capacity = new(snapshot.Count, 0);
            if (snapshot.Token == 0) return false;
            double[]? rented = null;
            try {
                if (output.Length < snapshot.Count) return false;
                rented = ArrayPool<double>.Shared.Rent(Math.Max(1, snapshot.Count));
                ReadMaterialSnapshot(scope, objectId, snapshot.Token, rented.AsSpan(0, snapshot.Count));
                rented.AsSpan(0, snapshot.Count).CopyTo(output); capacity = new(snapshot.Count, snapshot.Count); return true;
            } finally { CloseMaterial(scope, objectId, snapshot.Token, true); if (rented is not null) ArrayPool<double>.Shared.Return(rented, true); }
        }
        public double[]? ReadMaterialValues(ScriptExecutionScope scope, ulong objectId, int slot, string property)
        {
            var snapshot = OpenMaterialValues(scope, objectId, slot, property); if (snapshot.Token == 0) return null;
            try { double[] result = new double[snapshot.Count]; ReadMaterialSnapshot(scope, objectId, snapshot.Token, result); return result; }
            finally { CloseMaterial(scope, objectId, snapshot.Token, true); }
        }
        void ReadMaterialSnapshot(ScriptExecutionScope scope, ulong objectId, ulong token, Span<double> values)
        {
            byte[] rented = ArrayPool<byte>.Shared.Rent(65536);
            try {
                Span<byte> header = stackalloc byte[24]; Span<byte> packet = rented.AsSpan(0, 65536);
                int offset = 0;
                while (offset < values.Length) {
                    int count = Math.Min(8189, values.Length - offset);
                    BinaryPrimitives.WriteUInt64LittleEndian(header, token); BinaryPrimitives.WriteUInt64LittleEndian(header[8..], (ulong)offset);
                    BinaryPrimitives.WriteUInt32LittleEndian(header[16..], (uint)count); BinaryPrimitives.WriteUInt32LittleEndian(header[20..], 0);
                    BridgeReply reply;
                    fixed (byte* input = header) fixed (byte* output = packet) {
                        BridgeRequest request = Request(scope, objectId, BridgeOp.SERVICE_MATERIAL); request.Index = 31; request.Flags = 1;
                        request.Input = input; request.InputBytes = 24; request.Output = output; request.OutputCapacity = (uint)(count * 8);
                        reply = Call(ref request);
                    }
                    if (reply.CallId != scope.SceneLifetime || reply.OutputBytes != count * 8 || reply.Integer != count) throw new InvalidDataException("Material numeric page changed during synchronous read.");
                    for (int index = 0; index < count; index++) {
                        double value = BinaryPrimitives.ReadDoubleLittleEndian(packet[(index * 8)..]);
                        if (!double.IsFinite(value)) throw new InvalidDataException("Nonfinite material numeric reply.");
                        values[offset + index] = value;
                    }
                    offset += count;
                }
            } finally { ArrayPool<byte>.Shared.Return(rented, true); }
        }
        public bool WriteMaterialValues(ScriptExecutionScope scope, ulong objectId, int slot, string property, ReadOnlySpan<double> values)
        {
            foreach (double value in values) if (!double.IsFinite(value)) throw new ArgumentOutOfRangeException(nameof(values));
            long handle = InvokeService(scope, objectId, (uint)BridgeOp.SERVICE_MATERIAL, 32, slot, [], [property, values.Length.ToString(CultureInfo.InvariantCulture)], null, false).Integer;
            if (handle <= 0) throw new InvalidDataException("Material numeric reception returned an invalid token.");
            ulong token = (ulong)handle; byte[] rented = ArrayPool<byte>.Shared.Rent(65536);
            try {
                Span<byte> packet = rented.AsSpan(0, 65536); int offset = 0;
                while (offset < values.Length) {
                    int count = Math.Min(8189, values.Length - offset);
                    BinaryPrimitives.WriteUInt64LittleEndian(packet, token); BinaryPrimitives.WriteUInt64LittleEndian(packet[8..], (ulong)offset);
                    BinaryPrimitives.WriteUInt32LittleEndian(packet[16..], (uint)count); BinaryPrimitives.WriteUInt32LittleEndian(packet[20..], 0);
                    for (int index = 0; index < count; index++) BinaryPrimitives.WriteDoubleLittleEndian(packet[(24 + index * 8)..], values[offset + index]);
                    BridgeReply reply = StorePacket(scope, objectId, (uint)BridgeOp.SERVICE_MATERIAL, 33, packet[..(24 + count * 8)]);
                    offset += count; if (reply.Integer != offset) throw new InvalidDataException("Material numeric reception acknowledged a different offset.");
                }
                BinaryPrimitives.WriteUInt64LittleEndian(packet, token);
                BridgeReply committed = StorePacket(scope, objectId, (uint)BridgeOp.SERVICE_MATERIAL, 34, packet[..8]); token = 0;
                if (committed.Integer is not (0 or 1)) throw new InvalidDataException("Invalid material numeric commit result.");
                return committed.Integer != 0;
            } finally { if (token != 0) CloseMaterial(scope, objectId, token, false); ArrayPool<byte>.Shared.Return(rented, true); }
        }
        void CloseMaterial(ScriptExecutionScope scope, ulong objectId, ulong token, bool read)
        {
            try {
                Span<byte> header = stackalloc byte[8]; BinaryPrimitives.WriteUInt64LittleEndian(header, token);
                Span<byte> output = stackalloc byte[1024];
                fixed (byte* source = header) fixed (byte* destination = output) {
                    BridgeRequest request = Request(scope, objectId, BridgeOp.SERVICE_MATERIAL); request.Index = 35; request.Flags = read ? 1u : 0;
                    request.Input = source; request.InputBytes = 8; request.Output = destination; request.OutputCapacity = 1024; _ = Call(ref request);
                }
            } catch { }
        }
    }
}
