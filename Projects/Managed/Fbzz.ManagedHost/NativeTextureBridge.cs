/// @file    NativeTextureBridge.cs
/// @brief   Transfers owned runtime texture rectangles through bounded byte packets.
/// @author  Hasegawa Jin
/// @date    2026-10-26
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
        public long CreateRuntimeTexture(ScriptExecutionScope scope, int width, int height, RuntimeTextureFormat format, out ulong ownerObject)
        {
            BridgeRequest self = Request(scope, 0, BridgeOp.OBJECT_SELF);
            Span<byte> selfName = stackalloc byte[128];
            fixed (byte* destination = selfName)
            {
                self.Output = destination; self.OutputCapacity = (uint)selfName.Length;
                ownerObject = unchecked((ulong)Call(ref self).Integer);
            }
            return InvokeService(scope, ownerObject, (uint)BridgeOp.SERVICE_ASSETS, 10, 0, [width, height, (float)format], [], null, false).Integer;
        }

        public bool UpdateRuntimeTexture(ScriptExecutionScope scope, ulong ownerObject, long texture,
            int x, int y, int width, int height, int rowPitch, ReadOnlySpan<byte> pixels)
        {
            uint service = (uint)BridgeOp.SERVICE_ASSETS;
            long token = InvokeService(scope, ownerObject, service, 11, texture, [],
                [x.ToString(CultureInfo.InvariantCulture), y.ToString(CultureInfo.InvariantCulture),
                 width.ToString(CultureInfo.InvariantCulture), height.ToString(CultureInfo.InvariantCulture), rowPitch.ToString(CultureInfo.InvariantCulture)], null, false).Integer;
            if (token == 0) return false;
            if (token < 0) throw new InvalidDataException("Invalid texture reception token.");
            byte[] rented = ArrayPool<byte>.Shared.Rent(65536);
            bool committed = false;
            try
            {
                Span<byte> packet = rented.AsSpan(0, 65536);
                int offset = 0;
                while (offset < pixels.Length)
                {
                    int count = Math.Min(packet.Length - 24, pixels.Length - offset);
                    BinaryPrimitives.WriteUInt64LittleEndian(packet, (ulong)token);
                    BinaryPrimitives.WriteUInt64LittleEndian(packet[8..], (ulong)offset);
                    BinaryPrimitives.WriteUInt32LittleEndian(packet[16..], (uint)count);
                    BinaryPrimitives.WriteUInt32LittleEndian(packet[20..], 0);
                    pixels.Slice(offset, count).CopyTo(packet[24..]);
                    BridgeReply reply = StorePacket(scope, ownerObject, service, 12, packet[..(24 + count)]);
                    offset += count;
                    if (reply.Integer != offset) throw new InvalidDataException("Texture upload acknowledged a different byte offset.");
                }
                BinaryPrimitives.WriteUInt64LittleEndian(packet, (ulong)token);
                BridgeReply result = StorePacket(scope, ownerObject, service, 13, packet[..8]);
                committed = true;
                return result.Integer == 1;
            }
            finally
            {
                if (!committed)
                {
                    try { BinaryPrimitives.WriteUInt64LittleEndian(rented, (ulong)token); StorePacket(scope, ownerObject, service, 14, rented.AsSpan(0, 8)); }
                    catch { }
                }
                ArrayPool<byte>.Shared.Return(rented, clearArray: true);
            }
        }
    }
}
