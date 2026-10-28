/// @file    NativeTransferBuffer.cs
/// @brief   同期 marshalling の小さい packet を stack、大きい packet を個別 pool lease に置く。
/// @author  Hasegawa Jin
/// @date    2026-10-20
using System;
using System.Buffers;

namespace Fbzz.ManagedHost;

public static unsafe partial class EntryPoint
{
    /// @note Each invocation owns its storage, including during nested native callbacks; no borrowed bytes escape disposal.
    ref struct NativeTransferBuffer
    {
        byte[]? m_rented;
        Span<byte> m_bytes;
        internal NativeTransferBuffer(int length, Span<byte> local)
        {
            if (length < 0) throw new ArgumentOutOfRangeException(nameof(length));
            m_rented = length <= local.Length ? null : ArrayPool<byte>.Shared.Rent(length);
            m_bytes = (m_rented is null ? local : m_rented.AsSpan()).Slice(0, length);
        }
        internal NativeTransferBuffer(string text, Span<byte> local, int prefixBytes = 0)
            : this(checked(prefixBytes + s_utf8.GetByteCount(text)), local)
        {
            try { s_utf8.GetBytes(text.AsSpan(), m_bytes[prefixBytes..]); }
            catch { Dispose(); throw; }
        }
        internal readonly Span<byte> Bytes => m_bytes;
        public void Dispose()
        {
            byte[]? rented = m_rented;
            m_rented = null; m_bytes = default;
            if (rented is not null) ArrayPool<byte>.Shared.Return(rented, clearArray: true);
        }
    }
}
