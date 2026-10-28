/// @file    NativeObjectBatchBridge.cs
/// @brief   呼出元の Span と再利用する借用バッファを一回の native 一括操作へ接続する。
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
        byte[] m_batchInput = [];
        byte[] m_batchOutput = [];
        bool m_batchInFlight;

        public BatchResult ReadWorldPositions(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects,
            Span<Vector3> positions, Span<ObjectStatus> statuses)
        {
            if (positions.Length < objects.Length || statuses.Length < objects.Length)
                return BatchFailure(BatchStatus.BUFFER_TOO_SMALL, objects.Length);
            return ObjectBatch(scope, objects, ReadOnlySpan<Vector3>.Empty, positions, statuses,
                BridgeOp.OBJECTS_READ_WORLD_POSITIONS, default, false);
        }

        public BatchResult SetWorldPositions(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects,
            ReadOnlySpan<Vector3> positions, Span<ObjectStatus> statuses)
        {
            if (positions.Length < objects.Length) return BatchFailure(BatchStatus.INVALID_ARGUMENT, objects.Length);
            if (statuses.Length < objects.Length) return BatchFailure(BatchStatus.BUFFER_TOO_SMALL, objects.Length);
            return ObjectBatch(scope, objects, positions, Span<Vector3>.Empty, statuses,
                BridgeOp.OBJECTS_SET_WORLD_POSITIONS, default, false);
        }

        public BatchResult TranslateWorld(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects,
            Vector3 displacement, bool activeOnly)
        {
            if (!displacement.IsFinite) return BatchFailure(BatchStatus.INVALID_ARGUMENT, objects.Length);
            return ObjectBatch(scope, objects, ReadOnlySpan<Vector3>.Empty, Span<Vector3>.Empty, Span<ObjectStatus>.Empty,
                BridgeOp.OBJECTS_TRANSLATE_WORLD, displacement, activeOnly);
        }

        /// @note scratch は同期 dispatch の期間だけ借用し、同じ bridge の再入で現在のバッファを上書きしない。
        BatchResult ObjectBatch(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects,
            ReadOnlySpan<Vector3> inputPositions, Span<Vector3> outputPositions, Span<ObjectStatus> statuses,
            BridgeOp operation, Vector3 displacement, bool activeOnly)
        {
            int count = objects.Length;
            if (scope.Identity == 0 || scope.ThreadId != Environment.CurrentManagedThreadId || m_batchInFlight)
                return BatchFailure(BatchStatus.INVALID_SCOPE, count);
            for (int index = 0; index < count; index++)
                if (objects[index] is GameObject item && item.SceneLifetime != scope.SceneLifetime)
                    return BatchFailure(BatchStatus.SCENE_MISMATCH, count);
            bool read = operation == BridgeOp.OBJECTS_READ_WORLD_POSITIONS;
            bool set = operation == BridgeOp.OBJECTS_SET_WORLD_POSITIONS;
            int inputStride = checked((int)(set ? BridgeAbi.BATCH_SET_ITEM_BYTES : BridgeAbi.BATCH_REFERENCE_BYTES));
            int outputStride = read ? checked((int)BridgeAbi.BATCH_READ_ITEM_BYTES) : set ? checked((int)BridgeAbi.BATCH_STATUS_BYTES) : 0;
            if ((ulong)count * (uint)inputStride > BridgeAbi.MAX_BATCH_PACKET_BYTES)
                return PhasedObjectBatch(scope, objects, inputPositions, outputPositions, statuses, operation, displacement, activeOnly);
            int inputBytes = checked(count * inputStride);
            int outputBytes = checked(count * outputStride);
            if (m_batchInput.Length < inputBytes) m_batchInput = new byte[inputBytes];
            if (m_batchOutput.Length < outputBytes) m_batchOutput = new byte[outputBytes];
            Span<byte> input = m_batchInput.AsSpan(0, inputBytes);
            for (int index = 0; index < count; index++)
            {
                GameObject? item = objects[index];
                Span<byte> row = input.Slice(index * inputStride, inputStride);
                BinaryPrimitives.WriteUInt64LittleEndian(row, item?.ObjectId ?? 0);
                BinaryPrimitives.WriteUInt64LittleEndian(row[8..], item?.SceneLifetime ?? 0);
                if (set)
                {
                    Vector3 position = inputPositions[index];
                    BinaryPrimitives.WriteSingleLittleEndian(row[16..], position.X);
                    BinaryPrimitives.WriteSingleLittleEndian(row[20..], position.Y);
                    BinaryPrimitives.WriteSingleLittleEndian(row[24..], position.Z);
                }
            }
            m_batchInFlight = true;
            try
            {
                fixed (byte* inputPointer = m_batchInput)
                fixed (byte* outputPointer = m_batchOutput)
                {
                    BridgeRequest request = Request(scope, 0, operation);
                    request.Index = (uint)count;
                    request.Flags = activeOnly ? 1U : 0U;
                    request.X = displacement.X; request.Y = displacement.Y; request.Z = displacement.Z;
                    request.Input = inputPointer; request.InputBytes = (uint)inputBytes;
                    request.Output = outputBytes == 0 ? null : outputPointer; request.OutputCapacity = (uint)outputBytes;
                    BridgeReply reply = default;
                    int returned = m_dispatch(&request, &reply);
                    BatchStatus status = BatchStatusFor(returned, reply.Status);
                    if (status != BatchStatus.OK)
                    {
                        if (reply.Index != 0 || reply.Flags != 0 || (status == BatchStatus.BUFFER_TOO_SMALL
                            ? reply.OutputBytes != (uint)outputBytes : reply.OutputBytes != 0 && reply.OutputBytes != (uint)outputBytes))
                            throw new InvalidDataException("Native batch failure returned invalid counts or output length.");
                        return BatchFailure(status, count);
                    }
                    if (reply.Index > (uint)count || reply.Flags > (uint)count || (ulong)reply.Index + reply.Flags != (uint)count
                        || reply.OutputBytes != (uint)outputBytes)
                        throw new InvalidDataException("Native batch success returned invalid counts or output length.");
                    ReadOnlySpan<byte> output = m_batchOutput.AsSpan(0, outputBytes);
                    if (read || set)
                    {
                        int valid = 0;
                        for (int index = 0; index < count; index++)
                        {
                            uint slot = BinaryPrimitives.ReadUInt32LittleEndian(output[(index * outputStride)..]);
                            if (slot > (uint)ObjectStatus.EXPIRED) throw new InvalidDataException("Native batch returned an unknown slot status.");
                            if (slot == (uint)ObjectStatus.VALID) {
                                if (read) ValidateBatchPosition(output[(index * outputStride)..]);
                                valid++;
                            }
                        }
                        if ((uint)valid != reply.Index || count - valid != (int)reply.Flags)
                            throw new InvalidDataException("Native batch slot statuses disagree with its counts.");
                        /// @note 全 reply の検証完了まで呼出元の Span は変更せず、欠損位置と余剰 tail は保持する。
                        for (int index = 0; index < count; index++)
                        {
                            ReadOnlySpan<byte> row = output[(index * outputStride)..];
                            ObjectStatus slot = (ObjectStatus)BinaryPrimitives.ReadUInt32LittleEndian(row);
                            statuses[index] = slot;
                            if (read && slot == ObjectStatus.VALID)
                                outputPositions[index] = new Vector3(BinaryPrimitives.ReadSingleLittleEndian(row[4..]),
                                    BinaryPrimitives.ReadSingleLittleEndian(row[8..]), BinaryPrimitives.ReadSingleLittleEndian(row[12..]));
                        }
                    }
                    return new BatchResult(BatchStatus.OK, count, (int)reply.Index, (int)reply.Flags);
                }
            }
            finally { m_batchInFlight = false; }
        }

        static BatchResult BatchFailure(BatchStatus status, int count) => new(status, count, 0, 0);

        static BatchStatus BatchStatusFor(int returned, BridgeStatus status)
        {
            if (returned != (int)status) throw new InvalidDataException("Native batch return code disagrees with its reply status.");
            return status switch
            {
                BridgeStatus.OK => BatchStatus.OK,
                BridgeStatus.INVALID_ARGUMENT => BatchStatus.INVALID_ARGUMENT,
                BridgeStatus.BUFFER_TOO_SMALL => BatchStatus.BUFFER_TOO_SMALL,
                BridgeStatus.INVALID_SCOPE => BatchStatus.INVALID_SCOPE,
                BridgeStatus.SCENE_MISMATCH => BatchStatus.SCENE_MISMATCH,
                BridgeStatus.PHYSICS_BODY => BatchStatus.PHYSICS_BODY,
                _ => throw new InvalidDataException("Native batch returned an unexpected status.")
            };
        }
    }
}
