/// @file    NativeReadSnapshot.cs
/// @brief   同期 query 一回の immutable snapshot を固定 page で読み、総 byte 上限を置かない。
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
        ref struct NativeReadSnapshot
        {
            readonly NativeBridge m_bridge;
            readonly ScriptExecutionScope m_scope;
            readonly BridgeOp m_operation;
            readonly uint m_options;
            Span<byte> m_packet;
            byte[]? m_rented;
            ulong m_token, m_total, m_consumed, m_fetched;
            int m_position, m_length;
            internal readonly uint Count;
            internal readonly ScriptFieldKind ValueKind;
            internal NativeReadSnapshot(NativeBridge bridge, ScriptExecutionScope scope, scoped ref BridgeRequest request, Span<byte> local)
            {
                m_bridge = bridge; m_scope = scope; m_operation = request.Op; m_options = request.Flags;
                m_packet = local; m_rented = null; m_token = m_total = m_consumed = m_fetched = 0; m_position = m_length = 0; Count = 0; ValueKind = ScriptFieldKind.NONE;
                BridgeReply reply = default; int code;
                fixed (byte* output = local) fixed (BridgeRequest* pointer = &request) {
                    request.Output = output; request.OutputCapacity = (uint)local.Length; code = bridge.m_dispatch(pointer, &reply);
                }
                if (!(code == (int)BridgeStatus.BUFFER_TOO_SMALL && reply.Status == BridgeStatus.BUFFER_TOO_SMALL)) RequireSuccess(code, reply.Status);
                if (reply.CallId != scope.SceneLifetime) throw new InvalidDataException("Query returned another Scene lifetime.");
                Count = reply.Index;
                ValueKind = reply.ValueKind;
                if (code == (int)BridgeStatus.BUFFER_TOO_SMALL && reply.Status == BridgeStatus.BUFFER_TOO_SMALL) {
                    if (reply.Integer <= 0) throw new InvalidOperationException("Query did not provide a stable paged snapshot.");
                    m_token = (ulong)reply.Integer;
                    try {
                        if (reply.Generation <= (ulong)local.Length) throw new InvalidDataException("Invalid query snapshot capacity.");
                        m_total = reply.Generation; m_rented = ArrayPool<byte>.Shared.Rent(checked((int)BridgeAbi.MAX_BATCH_PACKET_BYTES));
                        m_packet = m_rented.AsSpan(0, checked((int)BridgeAbi.MAX_BATCH_PACKET_BYTES));
                    } catch { Dispose(); throw; }
                } else {
                    RequireSuccess(code, reply.Status);
                    if (reply.OutputBytes > local.Length) throw new InvalidDataException("Query exceeds its borrowed packet.");
                    m_length = checked((int)reply.OutputBytes); m_total = m_fetched = reply.OutputBytes;
                }
            }
            internal readonly ulong Remaining => m_total - m_consumed;
            internal readonly ulong FetchedRemaining => m_fetched - m_consumed;
            internal void Read(scoped Span<byte> target)
            {
                if ((ulong)target.Length > Remaining) throw new InvalidDataException("Query snapshot is truncated.");
                while (!target.IsEmpty) {
                    if (m_position == m_length) Fetch();
                    int count = Math.Min(target.Length, m_length - m_position);
                    m_packet.Slice(m_position, count).CopyTo(target); m_position += count; m_consumed += (uint)count; target = target[count..];
                }
            }
            void Fetch()
            {
                if (m_token == 0 || m_fetched == m_total) throw new InvalidDataException("Query snapshot ended early.");
                BridgeRequest request = Request(m_scope, 0, m_operation); request.Flags = 512U | m_options; request.Integer = checked((long)m_token);
                request.Reserved0 = (uint)m_fetched; request.Reserved1 = (uint)(m_fetched >> 32);
                int expected = (int)Math.Min((ulong)m_packet.Length, m_total - m_fetched);
                fixed (byte* output = m_packet) { request.Output = output; request.OutputCapacity = (uint)expected; BridgeReply reply = m_bridge.Call(ref request);
                    if (reply.CallId != m_scope.SceneLifetime || reply.Index != Count || reply.Generation != m_total || reply.OutputBytes != expected
                        || reply.Integer != (long)(m_fetched + (uint)expected) || reply.Flags != (m_fetched + (uint)expected == m_total ? 1U : 0U)) throw new InvalidDataException("Query snapshot page changed.");
                }
                m_fetched += (uint)expected; m_position = 0; m_length = expected;
            }
            internal uint UInt32() { Span<byte> bytes = stackalloc byte[4]; Read(bytes); return BinaryPrimitives.ReadUInt32LittleEndian(bytes); }
            internal ulong UInt64() { Span<byte> bytes = stackalloc byte[8]; Read(bytes); return BinaryPrimitives.ReadUInt64LittleEndian(bytes); }
            internal float Single() { Span<byte> bytes = stackalloc byte[4]; Read(bytes); return BinaryPrimitives.ReadSingleLittleEndian(bytes); }
            internal string Text()
            {
                uint length = UInt32(); if (length > int.MaxValue || length > Remaining) throw new InvalidDataException("Query UTF8 string is truncated.");
                byte[] rented = ArrayPool<byte>.Shared.Rent(Math.Max(1, (int)length));
                try { Span<byte> bytes = rented.AsSpan(0, (int)length); Read(bytes); return s_utf8.GetString(bytes); }
                finally { ArrayPool<byte>.Shared.Return(rented, true); }
            }
            internal void Dispose()
            {
                if (m_token != 0) { try { BridgeRequest close = Request(m_scope, 0, m_operation); close.Flags = 768U | m_options; close.Integer = checked((long)m_token); _ = m_bridge.Call(ref close); } catch { } m_token = 0; }
                if (m_rented is not null) { ArrayPool<byte>.Shared.Return(m_rented, true); m_rented = null; }
            }
        }
    }
}
