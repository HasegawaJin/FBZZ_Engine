/// @file    NativeObjectLifecycleBridge.cs
/// @brief   生成要求を再送せず、Prefab 復元中だけ外側の managed 実行権限を停止する。
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
        public GameObject? CreateObject(ScriptExecutionScope scope, string name, Vector3 position, Quaternion rotation, bool usePose)
            => CreateNative(scope, name, position, rotation, usePose, BridgeOp.SCENE_CREATE);

        public GameObject? InstantiateObject(ScriptExecutionScope scope, string prefab, Vector3 position, Quaternion rotation, bool usePose)
            => CreateNative(scope, prefab, position, rotation, usePose, BridgeOp.SCENE_INSTANTIATE);

        /// @note 新規 Script の constructor と設定復元に外側の callback scope を漏らさない。native の caller scope はそのまま保持する。
        GameObject? CreateNative(ScriptExecutionScope scope, string text, Vector3 position, Quaternion rotation, bool usePose, BridgeOp operation)
        {
            byte[] name = s_utf8.GetBytes(text);
            byte[] input = new byte[checked(name.Length + (usePose ? 16 : 0))];
            if (usePose)
            {
                BinaryPrimitives.WriteSingleLittleEndian(input, rotation.X);
                BinaryPrimitives.WriteSingleLittleEndian(input.AsSpan(4), rotation.Y);
                BinaryPrimitives.WriteSingleLittleEndian(input.AsSpan(8), rotation.Z);
                BinaryPrimitives.WriteSingleLittleEndian(input.AsSpan(12), rotation.W);
            }
            name.CopyTo(input, usePose ? 16 : 0);
            byte[] output = new byte[256];
            fixed (byte* source = input)
            fixed (byte* destination = output)
            {
                BridgeRequest request = Request(scope, 0, operation);
                request.Input = source; request.InputBytes = (uint)input.Length;
                request.Output = destination; request.OutputCapacity = (uint)output.Length;
                request.Flags = usePose ? 1U : 0U;
                request.X = position.X; request.Y = position.Y; request.Z = position.Z;
                BridgeReply reply = default;
                ScriptExecutionScope? previous = ScriptApiRuntime.Current;
                if (!ScriptApiRuntime.IsSameScope(previous, scope)) throw new InvalidOperationException("Creation requires the current callback scope.");
                int status;
                try
                {
                    ScriptApiRuntime.Current = null;
                    status = m_dispatch(&request, &reply);
                }
                finally { ScriptApiRuntime.Current = previous; }
                if ((status == (int)BridgeStatus.CAPACITY_LIMIT || status == (int)BridgeStatus.NOT_FOUND) && reply.Status == (BridgeStatus)status)
                    return null;
                RequireSuccess(status, reply.Status);
                if (reply.ValueKind != ScriptFieldKind.GAME_OBJECT || reply.Integer == 0 || reply.CallId != scope.SceneLifetime || reply.OutputBytes > output.Length)
                    throw new InvalidDataException("Creation returned an invalid object identity.");
                return new GameObject(unchecked((ulong)reply.Integer), reply.CallId,
                    ScriptFieldCodec.ValidateGuid(s_utf8.GetString(output.AsSpan(0, (int)reply.OutputBytes))));
            }
        }

        public bool CanCreate(ScriptExecutionScope scope, int count)
        {
            BridgeRequest request = Request(scope, 0, BridgeOp.SCENE_CAN_CREATE);
            request.Index = checked((uint)count);
            return Call(ref request).Integer != 0;
        }

        public bool DestroyObject(ScriptExecutionScope scope, ulong objectId, float delay)
        {
            BridgeRequest request = Request(scope, objectId, BridgeOp.OBJECT_DESTROY);
            request.X = delay;
            BridgeReply reply = default;
            int status = m_dispatch(&request, &reply);
            if (status == (int)BridgeStatus.NOT_FOUND && reply.Status == BridgeStatus.NOT_FOUND) return false;
            RequireSuccess(status, reply.Status);
            return true;
        }

        public BatchResult DestroyObjects(ScriptExecutionScope scope, ReadOnlySpan<GameObject?> objects, float delay)
        {
            int count = objects.Length;
            if (!float.IsFinite(delay) || delay < 0f)
                return BatchFailure(BatchStatus.INVALID_ARGUMENT, count);
            if (m_batchInFlight) return BatchFailure(BatchStatus.INVALID_SCOPE, count);
            for (int index = 0; index < count; index++)
                if (objects[index] is GameObject item && item.SceneLifetime != scope.SceneLifetime)
                    return BatchFailure(BatchStatus.SCENE_MISMATCH, count);
            if ((ulong)count * BridgeAbi.BATCH_REFERENCE_BYTES > BridgeAbi.MAX_BATCH_PACKET_BYTES)
                return PhasedObjectBatch(scope, objects, [], [], [], BridgeOp.OBJECTS_DESTROY, new Vector3(delay, 0, 0), false);
            int inputBytes = checked(count * (int)BridgeAbi.BATCH_REFERENCE_BYTES);
            if (m_batchInput.Length < inputBytes) m_batchInput = new byte[inputBytes];
            for (int index = 0; index < count; index++)
            {
                GameObject? item = objects[index];
                Span<byte> row = m_batchInput.AsSpan(index * (int)BridgeAbi.BATCH_REFERENCE_BYTES, (int)BridgeAbi.BATCH_REFERENCE_BYTES);
                BinaryPrimitives.WriteUInt64LittleEndian(row, item?.ObjectId ?? 0);
                BinaryPrimitives.WriteUInt64LittleEndian(row[8..], item?.SceneLifetime ?? 0);
            }
            m_batchInFlight = true;
            try
            {
                fixed (byte* input = m_batchInput)
                {
                    BridgeRequest request = Request(scope, 0, BridgeOp.OBJECTS_DESTROY);
                    request.Input = input; request.InputBytes = (uint)inputBytes; request.Index = (uint)count; request.X = delay;
                    BridgeReply reply = default;
                    int status = m_dispatch(&request, &reply);
                    if (status != (int)reply.Status || !Enum.IsDefined(typeof(BatchStatus), status))
                        throw new InvalidDataException("DestroyMany returned an invalid status.");
                    if (reply.Status != BridgeStatus.OK) return BatchFailure((BatchStatus)status, count);
                    if (reply.OutputBytes != 0 || reply.Index > count || reply.Flags > count || (ulong)reply.Index + reply.Flags != (uint)count)
                        throw new InvalidDataException("DestroyMany returned invalid counts.");
                    return new BatchResult(BatchStatus.OK, count, (int)reply.Index, (int)reply.Flags);
                }
            }
            finally { m_batchInFlight = false; }
        }
    }
}
