/// @file    NativeObjectQueryBuffer.cs
/// @brief   caller の配列と Host の転送・参照 cache を再利用する全件検索。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using Fbzz;

namespace Fbzz.ManagedHost;
public static unsafe partial class EntryPoint
{
    sealed partial class NativeBridge
    {
        byte[] m_objectQueryInput = [];
        byte[] m_objectQueryOutput = new byte[4];
        GameObject?[] m_objectQueryResults = [];
        readonly Dictionary<ulong, GameObject> m_objectQueryCache = new();
        readonly HashSet<ulong> m_objectQuerySeen = new();
        readonly List<ulong> m_objectQueryStale = [];
        ulong m_objectQueryLifetime;
        bool m_objectQueryInFlight;
        /// @note cache は直近の成功結果だけを保持する。別の絞込結果から戻ると wrapper は再作成され得るが、寿命 ID の等値性は保つ。
        public ObjectQueryResult QueryObjectsInto(ScriptExecutionScope scope, ulong root, uint scopeKind,
            string name, string tag, string prefabPart, bool includeInactive, Span<GameObject?> output)
        {
            if (m_objectQueryInFlight) return new ObjectQueryResult(BatchStatus.INVALID_ARGUMENT, 0);
            int inputBytes = checked(12 + s_utf8.GetByteCount(name) + s_utf8.GetByteCount(tag) + s_utf8.GetByteCount(prefabPart));
            if (m_objectQueryInput.Length < inputBytes) m_objectQueryInput = new byte[inputBytes];
            int offset = 0;
            WriteQueryText(name, ref offset); WriteQueryText(tag, ref offset); WriteQueryText(prefabPart, ref offset);
            bool sameLifetime = m_objectQueryLifetime == scope.SceneLifetime;
            m_objectQueryInFlight = true;
            try
            {
                for (int attempt = 0; attempt < 2; attempt++)
                {
                    fixed (byte* input = m_objectQueryInput)
                    fixed (byte* destination = m_objectQueryOutput)
                    {
                        BridgeRequest request = Request(scope, root, BridgeOp.SCENE_QUERY);
                        request.Input = input; request.InputBytes = (uint)inputBytes; request.Index = scopeKind; request.Flags = includeInactive ? 1U : 0U;
                        request.Output = destination; request.OutputCapacity = (uint)m_objectQueryOutput.Length;
                        BridgeReply reply = default; int status = m_dispatch(&request, &reply);
                        if (status != (int)reply.Status || reply.Index > int.MaxValue || reply.OutputBytes > int.MaxValue)
                            throw new InvalidDataException("Object query returned an invalid status or count.");
                        if ((reply.Status == BridgeStatus.BUFFER_TOO_SMALL || reply.Status == BridgeStatus.OK) && reply.Index > output.Length)
                            return new ObjectQueryResult(BatchStatus.BUFFER_TOO_SMALL, (int)reply.Index);
                        if (reply.Status == BridgeStatus.BUFFER_TOO_SMALL)
                        {
                            if (attempt != 0 || reply.OutputBytes <= m_objectQueryOutput.Length)
                                throw new InvalidDataException("Object query reported an inconsistent capacity.");
                            m_objectQueryOutput = new byte[(int)reply.OutputBytes];
                            continue;
                        }
                        RequireSuccess(status, reply.Status);
                        if (reply.CallId != scope.SceneLifetime || reply.OutputBytes > m_objectQueryOutput.Length || reply.OutputBytes < 4)
                            throw new InvalidDataException("Object query returned an invalid buffer.");
                        ReadOnlySpan<byte> bytes = m_objectQueryOutput.AsSpan(0, (int)reply.OutputBytes);
                        uint count = BinaryPrimitives.ReadUInt32LittleEndian(bytes); offset = 4;
                        if (count != reply.Index || count > (bytes.Length - 4) / 12)
                            throw new InvalidDataException("Object query returned an invalid count.");
                        if (m_objectQueryResults.Length < (int)count) m_objectQueryResults = new GameObject?[(int)count];
                        for (int index = 0; index < count; index++)
                        {
                            if (bytes.Length - offset < 12) throw new InvalidDataException("Object query is truncated.");
                            ulong id = BinaryPrimitives.ReadUInt64LittleEndian(bytes[offset..]); offset += 8;
                            uint length = BinaryPrimitives.ReadUInt32LittleEndian(bytes[offset..]); offset += 4;
                            if (id == 0 || length > bytes.Length - offset) throw new InvalidDataException("Object query identity is invalid.");
                            ReadOnlySpan<byte> guid = bytes.Slice(offset, (int)length); offset += (int)length;
                            if (!m_objectQuerySeen.Add(id)) throw new InvalidDataException("Object query repeated an identity.");
                            if (!sameLifetime || !m_objectQueryCache.TryGetValue(id, out GameObject? item))
                            {
                                item = new GameObject(id, scope.SceneLifetime, ScriptFieldCodec.ValidateGuid(s_utf8.GetString(guid)));
                            }
                            m_objectQueryResults[index] = item;
                        }
                        if (offset != bytes.Length) throw new InvalidDataException("Object query has trailing bytes.");
                        if (!sameLifetime) { m_objectQueryCache.Clear(); m_objectQueryLifetime = scope.SceneLifetime; }
                        foreach (ulong id in m_objectQueryCache.Keys)
                            if (!m_objectQuerySeen.Contains(id)) m_objectQueryStale.Add(id);
                        foreach (ulong id in m_objectQueryStale) m_objectQueryCache.Remove(id);
                        for (int index = 0; index < count; index++)
                        {
                            GameObject item = m_objectQueryResults[index]!;
                            m_objectQueryCache.TryAdd(item.ObjectId, item);
                        }
                        m_objectQueryResults.AsSpan(0, (int)count).CopyTo(output);
                        return new ObjectQueryResult(BatchStatus.OK, (int)count);
                    }
                }
                throw new InvalidDataException("Object query changed its capacity twice.");
            }
            finally
            {
                Array.Clear(m_objectQueryResults);
                m_objectQuerySeen.Clear();
                m_objectQueryStale.Clear();
                m_objectQueryInFlight = false;
            }
        }
        void WriteQueryText(string value, ref int offset)
        {
            int length = s_utf8.GetByteCount(value);
            BinaryPrimitives.WriteUInt32LittleEndian(m_objectQueryInput.AsSpan(offset), (uint)length); offset += 4;
            s_utf8.GetBytes(value.AsSpan(), m_objectQueryInput.AsSpan(offset, length)); offset += length;
        }
    }
}
