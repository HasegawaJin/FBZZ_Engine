/// @file    NativeObjectControlsBridge.cs
/// @brief   オブジェクト制御・階層検索と再送しない一括生成の marshaller。
/// @author  Hasegawa Jin
/// @date    2026-10-04
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
        public void OwnObjects(ScriptExecutionScope scope, ulong instanceId, ReadOnlySpan<GameObject> objects)
        {
            using NativeTransferBuffer storage = new(checked(objects.Length * 16), stackalloc byte[256]);
            Span<byte> bytes = storage.Bytes;
            for (int index = 0; index < objects.Length; index++)
            {
                BinaryPrimitives.WriteUInt64LittleEndian(bytes[(index * 16)..], objects[index].ObjectId);
                BinaryPrimitives.WriteUInt64LittleEndian(bytes[(index * 16 + 8)..], objects[index].SceneLifetime);
            }
            fixed (byte* input = bytes)
            {
                BridgeRequest request = Request(scope, instanceId, BridgeOp.OBJECTS_OWN); request.Index = (uint)objects.Length;
                request.Input = input; request.InputBytes = (uint)bytes.Length; Call(ref request);
            }
        }
        public bool ReadComponent(ScriptExecutionScope scope, ulong objectId, uint kind)
        {
            BridgeRequest request = Request(scope, objectId, BridgeOp.COMPONENT_READ); request.Index = kind;
            BridgeReply reply = default; int status = m_dispatch(&request, &reply);
            if (status == (int)BridgeStatus.NOT_FOUND && reply.Status == BridgeStatus.NOT_FOUND) return false;
            RequireSuccess(status, reply.Status); return reply.Integer != 0;
        }
        public ulong ReadComponentLifetime(ScriptExecutionScope scope, ulong objectId, uint kind)
        {
            BridgeRequest request = Request(scope, objectId, BridgeOp.COMPONENT_READ); request.Index = kind; request.Flags = 1;
            BridgeReply reply = default; int status = m_dispatch(&request, &reply);
            if (status == (int)BridgeStatus.NOT_FOUND && reply.Status == BridgeStatus.NOT_FOUND) return 0;
            RequireSuccess(status, reply.Status); return unchecked((ulong)reply.Integer);
        }
        public long ReadObject(ScriptExecutionScope scope, ulong objectId, uint selector)
        {
            BridgeRequest request = Request(scope, objectId, BridgeOp.OBJECT_READ); request.Index = selector;
            return Call(ref request).Integer;
        }
        public string ReadObjectText(ScriptExecutionScope scope, ulong objectId, uint selector)
        {
            BridgeRequest request = Request(scope, objectId, BridgeOp.OBJECT_READ); request.Index = selector;
            return QueryText(ref request, out _);
        }
        public void WriteObject(ScriptExecutionScope scope, ulong objectId, uint selector, long value, string text)
        {
            using NativeTransferBuffer storage = new(text, stackalloc byte[256], prefixBytes: 8);
            Span<byte> bytes = storage.Bytes;
            BinaryPrimitives.WriteInt64LittleEndian(bytes, value);
            fixed (byte* input = bytes)
            {
                BridgeRequest request = Request(scope, objectId, BridgeOp.OBJECT_WRITE); request.Index = selector;
                request.Input = input; request.InputBytes = (uint)bytes.Length; Call(ref request);
            }
        }
        public GameObject? ReadParent(ScriptExecutionScope scope, ulong objectId)
        {
            BridgeRequest request = Request(scope, objectId, BridgeOp.OBJECT_READ); request.Index = 5;
            return QueryObject(ref request);
        }
        public bool SetParent(ScriptExecutionScope scope, ulong objectId, GameObject? parent)
        {
            if (parent is not null && (parent.SceneLifetime != scope.SceneLifetime || parent.ObjectId == 0))
                throw new InvalidOperationException("Parent reference is unresolved or belongs to another scene.");
            byte* input = stackalloc byte[8];
            BinaryPrimitives.WriteUInt64LittleEndian(new Span<byte>(input, 8), parent?.ObjectId ?? 0);
            BridgeRequest request = Request(scope, objectId, BridgeOp.OBJECT_SET_PARENT);
            request.Input = input; request.InputBytes = 8;
            return Call(ref request).Integer != 0;
        }
        public bool ReadScriptEnabled(ScriptExecutionScope scope, ulong instanceId)
        {
            BridgeRequest request = Request(scope, instanceId, BridgeOp.SCRIPT_ENABLED_READ);
            return Call(ref request).Integer != 0;
        }
        public void WriteScriptEnabled(ScriptExecutionScope scope, ulong instanceId, bool value)
        {
            BridgeRequest request = Request(scope, instanceId, BridgeOp.SCRIPT_ENABLED_WRITE); request.Flags = value ? 1U : 0U;
            Call(ref request);
        }
        public ulong AddComponent(ScriptExecutionScope scope, ulong objectId, uint kind, string typeName)
        {
            if (!ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, scope)) throw new InvalidOperationException("Component creation requires the current callback scope.");
            using NativeTransferBuffer storage = new(typeName, stackalloc byte[256]);
            Span<byte> bytes = storage.Bytes;
            fixed (byte* input = bytes)
            {
                BridgeRequest request = Request(scope, objectId, BridgeOp.COMPONENT_ADD); request.Index = kind;
                request.Input = input; request.InputBytes = (uint)bytes.Length;
                byte* diagnostic = stackalloc byte[256]; request.Output = diagnostic; request.OutputCapacity = 256;
                BridgeReply reply = default;
                ScriptExecutionScope? previous = ScriptApiRuntime.Current;
                try
                {
                    ScriptApiRuntime.Current = null;
                    int status = m_dispatch(&request, &reply);
                    if (status != 0 && reply.OutputBytes != 0 && reply.OutputBytes <= request.OutputCapacity)
                        throw new InvalidOperationException(s_utf8.GetString(new ReadOnlySpan<byte>(diagnostic, (int)reply.OutputBytes)));
                    RequireSuccess(status, reply.Status);
                }
                finally { ScriptApiRuntime.Current = previous; }
                return unchecked((ulong)reply.Integer);
            }
        }
        public bool RemoveComponent(ScriptExecutionScope scope, ulong objectId, uint kind, ulong instanceId)
        {
            byte* input = stackalloc byte[8]; BinaryPrimitives.WriteUInt64LittleEndian(new Span<byte>(input, 8), instanceId);
            BridgeRequest request = Request(scope, objectId, BridgeOp.COMPONENT_REMOVE); request.Index = kind;
            if (kind == uint.MaxValue) { request.Input = input; request.InputBytes = 8; }
            return Call(ref request).Integer != 0;
        }
        public GameObject[] QueryObjects(ScriptExecutionScope scope, ulong root, uint scopeKind, string name, string tag, string prefabPart, bool includeInactive)
        {
            int length = checked(12 + s_utf8.GetByteCount(name) + s_utf8.GetByteCount(tag) + s_utf8.GetByteCount(prefabPart));
            using NativeTransferBuffer storage = new(length, stackalloc byte[256]);
            Span<byte> input = storage.Bytes; int offset = 0;
            WriteObjectQueryText(name, input, ref offset); WriteObjectQueryText(tag, input, ref offset); WriteObjectQueryText(prefabPart, input, ref offset);
            fixed (byte* pointer = input)
            {
                BridgeRequest request = Request(scope, root, BridgeOp.SCENE_QUERY);
                request.Input = pointer; request.InputBytes = (uint)input.Length; request.Index = scopeKind; request.Flags = includeInactive ? 1U : 0U;
                Span<byte> local = stackalloc byte[256]; byte[]? rented = null;
                try {
                    int count = QueryObjectBorrowed(ref request, local, ref rented, out BridgeReply reply);
                    return DecodeObjects(rented is null ? local[..count] : rented.AsSpan(0, count), reply, scope);
                } finally { if (rented is not null) ArrayPool<byte>.Shared.Return(rented, clearArray: true); }
            }
        }
        /// @note Keep the length-prefixed UTF8 wire form while writing directly into invocation-owned storage.
        static void WriteObjectQueryText(string value, Span<byte> destination, ref int offset)
        {
            int length = s_utf8.GetByteCount(value);
            BinaryPrimitives.WriteUInt32LittleEndian(destination[offset..], (uint)length); offset += 4;
            s_utf8.GetBytes(value.AsSpan(), destination.Slice(offset, length)); offset += length;
        }
        public BatchResult SpawnMany(ScriptExecutionScope scope, string reference, bool prefab, ReadOnlySpan<SpawnPose> poses, Span<GameObject?> output)
        {
            if (!ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, scope)) throw new InvalidOperationException("Bulk creation requires the current callback scope.");
            if (output.Length < poses.Length) return new BatchResult(BatchStatus.BUFFER_TOO_SMALL, poses.Length, 0, 0);
            byte[] text = s_utf8.GetBytes(reference);
            int inputBytes = checked(4 + text.Length + poses.Length * 28);
            int outputBytes = checked(4 + poses.Length * 76);
            byte[] input = new byte[inputBytes];
            BinaryPrimitives.WriteUInt32LittleEndian(input, (uint)text.Length); text.CopyTo(input, 4);
            int offset = 4 + text.Length;
            foreach (SpawnPose pose in poses)
            {
                BinaryPrimitives.WriteSingleLittleEndian(input.AsSpan(offset), pose.Position.X);
                BinaryPrimitives.WriteSingleLittleEndian(input.AsSpan(offset + 4), pose.Position.Y);
                BinaryPrimitives.WriteSingleLittleEndian(input.AsSpan(offset + 8), pose.Position.Z);
                BinaryPrimitives.WriteSingleLittleEndian(input.AsSpan(offset + 12), pose.Rotation.X);
                BinaryPrimitives.WriteSingleLittleEndian(input.AsSpan(offset + 16), pose.Rotation.Y);
                BinaryPrimitives.WriteSingleLittleEndian(input.AsSpan(offset + 20), pose.Rotation.Z);
                BinaryPrimitives.WriteSingleLittleEndian(input.AsSpan(offset + 24), pose.Rotation.W);
                offset += 28;
            }
            byte[] buffer = new byte[outputBytes];
            fixed (byte* source = input)
            fixed (byte* destination = buffer)
            {
                BridgeRequest request = Request(scope, 0, BridgeOp.SCENE_SPAWN_MANY);
                request.Input = source; request.InputBytes = (uint)input.Length; request.Output = destination; request.OutputCapacity = (uint)buffer.Length;
                request.Index = (uint)poses.Length; request.Flags = prefab ? 1U : 0U;
                BridgeReply reply = default; int status;
                ScriptExecutionScope? previous = ScriptApiRuntime.Current;
                try { ScriptApiRuntime.Current = null; status = m_dispatch(&request, &reply); }
                finally { ScriptApiRuntime.Current = previous; }
                if (status != (int)reply.Status || !Enum.IsDefined((BatchStatus)status)) throw new InvalidDataException("SpawnMany returned an invalid status.");
                if (reply.Status != BridgeStatus.OK) return new BatchResult((BatchStatus)status, poses.Length, 0, 0);
                if (reply.OutputBytes > buffer.Length) throw new InvalidDataException("SpawnMany exceeded its response buffer.");
                GameObject[] created = DecodeObjects(buffer.AsSpan(0, (int)reply.OutputBytes), reply, scope);
                if (created.Length != poses.Length) throw new InvalidDataException("SpawnMany returned an invalid count.");
                for (int index = 0; index < created.Length; index++) output[index] = created[index];
                return new BatchResult(BatchStatus.OK, poses.Length, poses.Length, 0);
            }
        }
        static GameObject[] DecodeObjects(ReadOnlySpan<byte> bytes, BridgeReply reply, ScriptExecutionScope scope)
        {
            if (bytes.Length < 4 || reply.CallId != scope.SceneLifetime) throw new InvalidDataException("Object query returned an invalid scene.");
            uint count = BinaryPrimitives.ReadUInt32LittleEndian(bytes);
            if (count > int.MaxValue || count != reply.Index || count > (bytes.Length - 4) / 12)
                throw new InvalidDataException("Object query returned an invalid count.");
            GameObject[] result = new GameObject[(int)count]; int offset = 4;
            for (int index = 0; index < result.Length; index++)
            {
                if (bytes.Length - offset < 12) throw new InvalidDataException("Object query is truncated.");
                ulong token = BinaryPrimitives.ReadUInt64LittleEndian(bytes[offset..]); offset += 8;
                uint guidBytes = BinaryPrimitives.ReadUInt32LittleEndian(bytes[offset..]); offset += 4;
                if (token == 0 || guidBytes > bytes.Length - offset) throw new InvalidDataException("Object query has an invalid identity.");
                string guid = ScriptFieldCodec.ValidateGuid(s_utf8.GetString(bytes.Slice(offset, (int)guidBytes)));
                offset += (int)guidBytes;
                result[index] = new GameObject(token, reply.CallId, guid);
            }
            if (offset != bytes.Length) throw new InvalidDataException("Object query has trailing bytes.");
            return result;
        }
        /// @note 読取専用の Scene query だけを再試行し、全返信の容量を managed 配列の表現範囲で検証する。
        int QueryObjectBorrowed(ref BridgeRequest request, Span<byte> local, ref byte[]? rented, out BridgeReply reply)
        {
            for (int attempt = 0; attempt < 2; attempt++)
            {
                Span<byte> buffer = rented is null ? local : rented.AsSpan();
                fixed (byte* output = buffer)
                {
                    request.Output = output; request.OutputCapacity = (uint)buffer.Length;
                    BridgeReply response = default;
                    int status;
                    fixed (BridgeRequest* pointer = &request) status = m_dispatch(pointer, &response);
                    reply = response;
                    if (status != (int)reply.Status || reply.Index > int.MaxValue || reply.OutputBytes > int.MaxValue)
                        throw new InvalidDataException("Object query returned an unrepresentable buffer or count.");
                    if (reply.Status == BridgeStatus.BUFFER_TOO_SMALL)
                    {
                        if (attempt != 0 || reply.OutputBytes <= buffer.Length)
                            throw new InvalidDataException("Object query reported an inconsistent capacity.");
                        rented = ArrayPool<byte>.Shared.Rent((int)reply.OutputBytes);
                        continue;
                    }
                    RequireSuccess(status, reply.Status);
                    if (reply.OutputBytes > buffer.Length) throw new InvalidDataException("Object query exceeded its response buffer.");
                    return (int)reply.OutputBytes;
                }
            }
            throw new InvalidDataException("Object query changed its capacity twice.");
        }
    }
}
