/// @file    NativeServicesBridge.cs
/// @brief   型付きサービスの値 packet を固定 ABI に転送する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Buffers;
using System.Buffers.Binary;
using System.IO;
using System.Text;
using Fbzz;

namespace Fbzz.ManagedHost;

public static unsafe partial class EntryPoint
{
    sealed partial class NativeBridge
    {
        static readonly UTF8Encoding s_storeUtf8 = new(false, true);
        /// @note Count/encode は immutable caller string と同じ同期 scope。packet ごとに Engine/user callback を配送しない。
        public void WriteStoreString(ScriptExecutionScope scope, ulong objectId, uint service, string key, string value)
        {
            int keyBytes = s_storeUtf8.GetByteCount(key);
            if (value.Length <= (1048576 - 32) / 3 && 32L + keyBytes + s_storeUtf8.GetByteCount(value) <= 1048576) {
                InvokeService(scope, objectId, service, 15, 0, [], [key, value], null, false);
                return;
            }
            byte[] rented = ArrayPool<byte>.Shared.Rent(65536);
            ulong token = 0;
            try {
                Span<byte> packet = rented.AsSpan(0, 65536);
                Encoder counter = s_storeUtf8.GetEncoder();
                int characters = 0; ulong total = 0;
                bool completed = false;
                do {
                    counter.Convert(value.AsSpan(characters), packet, true, out int usedChars, out int usedBytes, out completed);
                    characters += usedChars; total = checked(total + (uint)usedBytes);
                    if (!completed && usedChars == 0 && usedBytes == 0) throw new InvalidDataException("Store UTF8 counting made no progress.");
                } while (!completed);
                long smallBytes = checked(32L + keyBytes + (long)total);
                if (smallBytes <= 1048576) {
                    InvokeService(scope, objectId, service, 15, 0, [], [key, value], null, false);
                    return;
                }
                long handle = InvokeService(scope, objectId, service, 22, checked((long)total), [], [key], null, false).Integer;
                if (handle <= 0) throw new InvalidDataException("Store reception returned an invalid token.");
                token = (ulong)handle;
                Encoder encoder = s_storeUtf8.GetEncoder();
                characters = 0; ulong offset = 0; completed = false;
                while (!completed) {
                    encoder.Convert(value.AsSpan(characters), packet[24..], true, out int usedChars, out int usedBytes, out completed);
                    if (usedBytes == 0) throw new InvalidDataException("Store UTF8 transfer made no progress.");
                    BinaryPrimitives.WriteUInt64LittleEndian(packet, token);
                    BinaryPrimitives.WriteUInt64LittleEndian(packet[8..], offset);
                    BinaryPrimitives.WriteUInt32LittleEndian(packet[16..], (uint)usedBytes);
                    BinaryPrimitives.WriteUInt32LittleEndian(packet[20..], 0);
                    BridgeReply reply = StorePacket(scope, objectId, service, 23, packet[..(24 + usedBytes)]);
                    characters += usedChars; offset += (uint)usedBytes;
                    if (reply.Integer < 0 || (ulong)reply.Integer != offset) throw new InvalidDataException("Store reception acknowledged a different byte offset.");
                }
                if (offset != total) throw new InvalidDataException("Store UTF8 transfer total changed.");
                BinaryPrimitives.WriteUInt64LittleEndian(packet, token);
                _ = StorePacket(scope, objectId, service, 24, packet[..8]);
                token = 0;
            } finally {
                if (token != 0) {
                    try {
                        BinaryPrimitives.WriteUInt64LittleEndian(rented, token);
                        _ = StorePacket(scope, objectId, service, 25, rented.AsSpan(0, 8));
                    } catch { }
                }
                ArrayPool<byte>.Shared.Return(rented, clearArray: true);
            }
        }
        BridgeReply StorePacket(ScriptExecutionScope scope, ulong objectId, uint service, uint method, ReadOnlySpan<byte> input)
        {
            Span<byte> output = stackalloc byte[1024];
            fixed (byte* source = input) fixed (byte* destination = output) {
                BridgeRequest request = Request(scope, objectId, (BridgeOp)service);
                request.Index = method; request.Input = source; request.InputBytes = (uint)input.Length;
                request.Output = destination; request.OutputCapacity = (uint)output.Length;
                return Call(ref request);
            }
        }
        public void WriteStoreDocument(ScriptExecutionScope scope, ulong objectId, uint service, string key, Type type, object? value)
        {
            using ImmutableValueSnapshot.SnapshotStream document = PortableStoreDocument.Capture(type, value);
            string declared = PortableStoreDocument.TypeIdentity(type), schema = PortableStoreDocument.SchemaIdentity(type);
            long handle = InvokeService(scope, objectId, service, 26, document.Length, [], [key, declared, schema], null, false).Integer;
            if (handle <= 0) throw new InvalidDataException("Store document returned an invalid reception token.");
            ulong token = (ulong)handle; byte[] rented = ArrayPool<byte>.Shared.Rent(65536);
            try {
                Span<byte> packet = rented.AsSpan(0, 65536); ulong offset = 0;
                while (document.Position < document.Length) {
                    int count = document.Read(packet[24..]);
                    if (count == 0) throw new InvalidDataException("Store document transfer made no progress.");
                    BinaryPrimitives.WriteUInt64LittleEndian(packet, token); BinaryPrimitives.WriteUInt64LittleEndian(packet[8..], offset);
                    BinaryPrimitives.WriteUInt32LittleEndian(packet[16..], (uint)count); BinaryPrimitives.WriteUInt32LittleEndian(packet[20..], 0);
                    BridgeReply reply = StorePacket(scope, objectId, service, 23, packet[..(24 + count)]);
                    offset += (uint)count;
                    if (reply.Integer < 0 || (ulong)reply.Integer != offset) throw new InvalidDataException("Store document acknowledged a different offset.");
                }
                BinaryPrimitives.WriteUInt64LittleEndian(packet, token); _ = StorePacket(scope, objectId, service, 24, packet[..8]); token = 0;
            } finally {
                if (token != 0) { try { BinaryPrimitives.WriteUInt64LittleEndian(rented, token); _ = StorePacket(scope, objectId, service, 25, rented.AsSpan(0, 8)); } catch { } }
                ArrayPool<byte>.Shared.Return(rented, clearArray: true);
            }
        }
        public bool ReadStoreDocument(ScriptExecutionScope scope, ulong objectId, uint service, string key, Type type, object? defaults, out object? value)
        {
            value = null; PortableStoreDocument.ValidateSchema(type);
            NativeServiceResult result = InvokeService(scope, objectId, service, 27, 0, [], [key], null, true);
            if (!result.Flag) return false;
            if (result.Text.Length != 3) throw new InvalidDataException("Malformed portable store document response.");
            return PortableStoreDocument.TryRead(type, defaults, result.Text[0], result.Text[1], result.Text[2], out value);
        }
        public NativeServiceResult InvokeService(ScriptExecutionScope scope, ulong objectId, uint service, uint method,
            long integer, ReadOnlySpan<float> values, ReadOnlySpan<string> text, GameObject? related, bool readOnly)
        {
            if (values.Length > 128 || text.Length > 128) throw new ArgumentException("Service packet exceeds its declared capacity.");
            if (related is not null && (related.SceneLifetime != scope.SceneLifetime || related.ObjectId == 0))
                throw new InvalidOperationException("Service reference is unresolved or belongs to another scene.");
            int size = checked(24 + values.Length * 4);
            foreach (float value in values) if (!float.IsFinite(value)) throw new ArgumentException("Nonfinite service value.");
            foreach (string value in text) { ArgumentNullException.ThrowIfNull(value); size = checked(size + 4 + s_utf8.GetByteCount(value)); }
            if (size > 1048576) throw new ArgumentException("Service packet is too large.");
            Span<byte> localInput = stackalloc byte[1024];
            byte[]? rentedInput = size > localInput.Length ? ArrayPool<byte>.Shared.Rent(size) : null;
            byte[]? rentedOutput = null;
            try
            {
                Span<byte> input = rentedInput is null ? localInput[..size] : rentedInput.AsSpan(0, size);
                BinaryPrimitives.WriteUInt64LittleEndian(input, related?.ObjectId ?? 0);
                BinaryPrimitives.WriteInt64LittleEndian(input[8..], integer);
                BinaryPrimitives.WriteUInt32LittleEndian(input[16..], (uint)values.Length);
                BinaryPrimitives.WriteUInt32LittleEndian(input[20..], (uint)text.Length);
                int offset = 24;
                foreach (float value in values) { BinaryPrimitives.WriteSingleLittleEndian(input[offset..], value); offset += 4; }
                foreach (string value in text)
                {
                    int length = s_utf8.GetByteCount(value);
                    BinaryPrimitives.WriteUInt32LittleEndian(input[offset..], (uint)length); offset += 4;
                    offset += s_utf8.GetBytes(value, input.Slice(offset, length));
                }
                fixed (byte* source = input)
                {
                    BridgeRequest request = Request(scope, objectId, (BridgeOp)service);
                    request.Index = method; request.Flags = readOnly ? 1u : 0u; request.Input = source; request.InputBytes = (uint)input.Length;
                    Span<byte> output = stackalloc byte[1024];
                    BridgeReply reply;
                    int length;
                    if (readOnly)
                    {
                        fixed (byte* destination = output)
                        {
                            request.Output = destination; request.OutputCapacity = (uint)output.Length;
                            reply = default; int status = m_dispatch(&request, &reply);
                            if (status == (int)BridgeStatus.BUFFER_TOO_SMALL && reply.Status == BridgeStatus.BUFFER_TOO_SMALL) {
                                rentedOutput = ArrayPool<byte>.Shared.Rent(65536);
                                return ReadPagedService(scope, request, rentedOutput.AsSpan(0, 65536));
                            }
                            RequireSuccess(status, reply.Status);
                        }
                        if (reply.OutputBytes > output.Length) throw new InvalidDataException("Service reply exceeds capacity.");
                        length = (int)reply.OutputBytes;
                    }
                    else
                    {
                        /// @note 変更要求は容量不足でも再送しない。借用出力は同期復号で所有値へ変換する。
                        fixed (byte* destination = output)
                        {
                            request.Output = destination; request.OutputCapacity = (uint)output.Length;
                            reply = Call(ref request);
                        }
                        if (reply.OutputBytes > output.Length) throw new InvalidDataException("Mutating service reply exceeds capacity.");
                        length = (int)reply.OutputBytes;
                    }
                    return ServiceResult(scope, output[..length], reply);
                }
            }
            finally
            {
                if (rentedInput is not null) ArrayPool<byte>.Shared.Return(rentedInput, clearArray: true);
                if (rentedOutput is not null) ArrayPool<byte>.Shared.Return(rentedOutput, clearArray: true);
            }
        }

        NativeServiceResult ReadPagedService(ScriptExecutionScope scope, BridgeRequest request, Span<byte> page)
        {
            ServiceReader reader = new(this, request, page, scope.SceneLifetime);
            int floats = checked((int)reader.UInt32());
            int strings = checked((int)reader.UInt32());
            if ((ulong)floats * 4 + (ulong)strings * 4 + 12 > reader.Remaining)
                throw new InvalidDataException("Invalid paged service counts.");
            float[]? ownedValues = floats > 8 ? new float[floats] : null;
            Span<float> values = ownedValues is null ? stackalloc float[floats] : ownedValues;
            string[] text = strings == 0 ? [] : new string[strings];
            for (int index = 0; index < values.Length; index++) values[index] = reader.Single();
            for (int index = 0; index < text.Length; index++) text[index] = reader.String();
            ulong objectId = reader.UInt64();
            string guid = reader.String();
            if (reader.Remaining != 0 || (objectId == 0) != (guid.Length == 0)) throw new InvalidDataException("Invalid paged service object identity.");
            GameObject? gameObject = objectId == 0 ? null : new GameObject(objectId, scope.SceneLifetime, ScriptFieldCodec.ValidateGuid(guid));
            return new NativeServiceResult(values, text, reader.Integer, gameObject, ownedValues);
        }
        public CollectionReadResult ReadBlendWeights(ScriptExecutionScope scope, ulong objectId, Span<BlendWeight> output)
        {
            Span<byte> input = stackalloc byte[24]; input.Clear();
            Span<byte> page = stackalloc byte[1024];
            byte[]? rented = null;
            BlendWeight[]? staged = null;
            try
            {
              fixed (byte* source = input) {
                BridgeRequest request = Request(scope, objectId, BridgeOp.SERVICE_ANIMATOR);
                request.Index = 24; request.Flags = 1; request.Input = source; request.InputBytes = 24;
                ServiceReader reader = new(this, request, page, scope.SceneLifetime);
                int count = checked((int)reader.UInt32());
                if (reader.UInt32() != count || (ulong)count * 8 + 12 > reader.Remaining)
                    throw new InvalidDataException("Invalid blend weight counts.");
                if (output.Length < count) return new(count, 0);
                if (count > 64) {
                    rented = ArrayPool<byte>.Shared.Rent(65536);
                    reader = new(this, request, rented.AsSpan(0, 65536), scope.SceneLifetime);
                    if (reader.UInt32() != count || reader.UInt32() != count) throw new InvalidDataException("Blend weights changed during a synchronous read.");
                }
                /// @note 数値・UTF8・全後続 packet を検証してから caller へ一度だけ公開する。
                staged = ArrayPool<BlendWeight>.Shared.Rent(count);
                for (int index = 0; index < count; index++) staged[index] = new("", reader.Single());
                for (int index = 0; index < count; index++) staged[index] = new(reader.String(), staged[index].Weight);
                if (reader.UInt64() != 0 || reader.String().Length != 0 || reader.Remaining != 0)
                    throw new InvalidDataException("Unexpected blend weight object result.");
                staged.AsSpan(0, count).CopyTo(output);
                return new(count, count);
              }
            }
            finally {
                if (rented is not null) ArrayPool<byte>.Shared.Return(rented, clearArray: true);
                if (staged is not null) ArrayPool<BlendWeight>.Shared.Return(staged, clearArray: true);
            }
        }
        /// @note read-only 結果を固定 packet で復号する。total は通知値で、pool・全 packet 配列・ゲーム callback を保持しない。
        ref struct ServiceReader
        {
            readonly NativeBridge m_bridge;
            BridgeRequest m_request;
            readonly Span<byte> m_page;
            readonly ulong m_scene;
            ulong m_total;
            ulong m_loaded;
            int m_position;
            int m_length;
            bool m_initialized;
            long m_integer;
            internal ServiceReader(NativeBridge bridge, BridgeRequest request, Span<byte> page, ulong scene)
            {
                m_bridge = bridge; m_request = request; m_page = page; m_scene = scene;
                m_total = 0; m_loaded = 0; m_position = 0; m_length = 0; m_initialized = false; m_integer = 0;
                Fill();
            }
            internal ulong Remaining => m_total - (m_loaded - (ulong)(m_length - m_position));
            internal long Integer => m_integer;
            void Fill()
            {
                m_request.Flags = 257; m_request.Reserved0 = (uint)m_loaded; m_request.Reserved1 = (uint)(m_loaded >> 32);
                BridgeReply reply;
                fixed (byte* output = m_page) {
                    m_request.Output = output; m_request.OutputCapacity = (uint)m_page.Length;
                    reply = m_bridge.Call(ref m_request);
                }
                if (reply.CallId != m_scene || reply.OutputBytes > m_page.Length || reply.Flags > 1
                    || reply.Generation < m_loaded || reply.OutputBytes > reply.Generation - m_loaded
                    || (reply.Flags == 1) != (m_loaded + reply.OutputBytes == reply.Generation)
                    || (reply.OutputBytes == 0 && reply.Flags == 0)) throw new InvalidDataException("Invalid service page progress.");
                if (m_initialized && (reply.Generation != m_total || reply.Integer != m_integer))
                    throw new InvalidDataException("Service result changed during a synchronous read.");
                m_initialized = true; m_total = reply.Generation; m_integer = reply.Integer;
                m_length = (int)reply.OutputBytes; m_position = 0; m_loaded += reply.OutputBytes;
            }
            void Bytes(scoped Span<byte> output)
            {
                if ((ulong)output.Length > Remaining) throw new InvalidDataException("Truncated paged service response.");
                int written = 0;
                while (written < output.Length) {
                    if (m_position == m_length) Fill();
                    int count = Math.Min(output.Length - written, m_length - m_position);
                    m_page.Slice(m_position, count).CopyTo(output[written..]); m_position += count; written += count;
                }
            }
            internal uint UInt32() { Span<byte> bytes = stackalloc byte[4]; Bytes(bytes); return BinaryPrimitives.ReadUInt32LittleEndian(bytes); }
            internal ulong UInt64() { Span<byte> bytes = stackalloc byte[8]; Bytes(bytes); return BinaryPrimitives.ReadUInt64LittleEndian(bytes); }
            internal float Single()
            {
                float value = BitConverter.UInt32BitsToSingle(UInt32());
                if (!float.IsFinite(value)) throw new InvalidDataException("Nonfinite paged service value.");
                return value;
            }
            internal string String()
            {
                uint length = UInt32();
                if (length > Remaining || length > int.MaxValue) throw new InvalidDataException("Invalid service string length for a CLR string.");
                if (length == 0) return "";
                if (length <= m_length - m_position) {
                    string result = s_utf8.GetString(m_page.Slice(m_position, (int)length)); m_position += (int)length; return result;
                }
                Decoder decoder = s_utf8.GetDecoder();
                StringBuilder builder = new();
                Span<char> chars = stackalloc char[1024];
                uint remaining = length;
                while (remaining != 0) {
                    if (m_position == m_length) Fill();
                    int count = (int)Math.Min(remaining, (uint)(m_length - m_position));
                    decoder.Convert(m_page.Slice(m_position, count), chars, remaining == count, out int usedBytes, out int usedChars, out _);
                    if (usedBytes == 0 && usedChars == 0) throw new InvalidDataException("Service text decoder made no progress.");
                    builder.Append(chars[..usedChars]); m_position += usedBytes; remaining -= (uint)usedBytes;
                }
                return builder.ToString();
            }
        }

        static NativeServiceResult ServiceResult(ScriptExecutionScope scope, ReadOnlySpan<byte> bytes, BridgeReply reply)
        {
            if (bytes.Length < 20 || reply.CallId != scope.SceneLifetime) throw new InvalidDataException("Invalid service reply lifetime.");
            uint floats = BinaryPrimitives.ReadUInt32LittleEndian(bytes);
            uint strings = BinaryPrimitives.ReadUInt32LittleEndian(bytes[4..]);
            if (floats > int.MaxValue || strings > int.MaxValue || floats > (uint)(bytes.Length - 20) / 4
                || strings > (uint)(bytes.Length - 20 - (int)floats * 4) / 4) throw new InvalidDataException("Invalid service reply counts.");
            float[]? ownedValues = floats > 8 ? new float[(int)floats] : null;
            Span<float> values = ownedValues is null ? stackalloc float[(int)floats] : ownedValues;
            string[] text = strings == 0 ? [] : new string[(int)strings];
            int offset = 8;
            for (int index = 0; index < values.Length; index++)
            {
                values[index] = BinaryPrimitives.ReadSingleLittleEndian(bytes[offset..]); offset += 4;
                if (!float.IsFinite(values[index])) throw new InvalidDataException("Nonfinite service reply.");
            }
            for (int index = 0; index < text.Length; index++) text[index] = ServiceString(bytes, ref offset);
            if (bytes.Length - offset < 12) throw new InvalidDataException("Truncated service object.");
            ulong objectId = BinaryPrimitives.ReadUInt64LittleEndian(bytes[offset..]); offset += 8;
            string guid = ServiceString(bytes, ref offset);
            if (offset != bytes.Length || (objectId == 0) != (guid.Length == 0)) throw new InvalidDataException("Invalid service object identity.");
            GameObject? gameObject = objectId == 0 ? null : new GameObject(objectId, reply.CallId, ScriptFieldCodec.ValidateGuid(guid));
            return new NativeServiceResult(values, text, reply.Integer, gameObject, ownedValues);
        }
        static string ServiceString(ReadOnlySpan<byte> bytes, ref int offset)
        {
            if (bytes.Length - offset < 4) throw new InvalidDataException("Truncated service text.");
            uint length = BinaryPrimitives.ReadUInt32LittleEndian(bytes[offset..]); offset += 4;
            if (length > (uint)(bytes.Length - offset)) throw new InvalidDataException("Invalid service text length.");
            string value = s_utf8.GetString(bytes.Slice(offset, (int)length)); offset += (int)length; return value;
        }
    }
}
