/// @file    NativeObjectBatchTransfers.cs
/// @brief   全件候補を bounded packet で送り、検証済み結果だけを caller Span へ公開する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
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
        readonly List<byte[]> m_batchResultPages = [];
        BatchResult PhasedObjectBatch(ScriptExecutionScope scope, ReadOnlySpan<GameObject?> objects,
            ReadOnlySpan<Vector3> inputPositions, Span<Vector3> outputPositions, Span<ObjectStatus> statuses,
            BridgeOp operation, Vector3 delta, bool activeOnly)
        {
            int count = objects.Length, packetBytes = checked((int)BridgeAbi.MAX_BATCH_PACKET_BYTES);
            bool read = operation == BridgeOp.OBJECTS_READ_WORLD_POSITIONS, set = operation == BridgeOp.OBJECTS_SET_WORLD_POSITIONS;
            int stride = checked((int)(set ? BridgeAbi.BATCH_SET_ITEM_BYTES : BridgeAbi.BATCH_REFERENCE_BYTES));
            int outputStride = read ? 16 : set ? 4 : 0;
            ulong outputBytes = (ulong)count * (uint)outputStride;
            if (m_batchInput.Length < packetBytes) m_batchInput = new byte[packetBytes];
            int pageCount = checked((int)((outputBytes + (uint)packetBytes - 1) / (uint)packetBytes));
            while (m_batchResultPages.Count < pageCount) m_batchResultPages.Add(new byte[packetBytes]);
            m_batchInFlight = true; ulong token = 0;
            try {
                BridgeRequest begin = Request(scope, 0, operation); begin.Flags = 256U | (activeOnly ? 1U : 0U); begin.Index = (uint)count;
                begin.X = delta.X; begin.Y = delta.Y; begin.Z = delta.Z;
                BatchStatus status = DispatchBatchPhase(ref begin, out BridgeReply reply);
                if (status != BatchStatus.OK) return BatchFailure(status, count);
                if (reply.Integer <= 0 || reply.OutputBytes != 0 || reply.CallId != scope.SceneLifetime) throw new InvalidDataException("Invalid batch candidate token.");
                token = (ulong)reply.Integer;
                int offset = 0;
                while (offset < count) {
                    int rows = Math.Min(packetBytes / stride, count - offset);
                    Span<byte> packet = m_batchInput.AsSpan(0, rows * stride);
                    for (int index = 0; index < rows; index++) {
                        GameObject? item = objects[offset + index]; Span<byte> row = packet[(index * stride)..];
                        BinaryPrimitives.WriteUInt64LittleEndian(row, item?.ObjectId ?? 0);
                        BinaryPrimitives.WriteUInt64LittleEndian(row[8..], item?.SceneLifetime ?? 0);
                        if (set) { Vector3 point = inputPositions[offset + index]; BinaryPrimitives.WriteSingleLittleEndian(row[16..], point.X); BinaryPrimitives.WriteSingleLittleEndian(row[20..], point.Y); BinaryPrimitives.WriteSingleLittleEndian(row[24..], point.Z); }
                    }
                    fixed (byte* input = packet) {
                        BridgeRequest stage = BatchPhase(scope, operation, token, 2, activeOnly); stage.Index = (uint)offset; stage.Input = input; stage.InputBytes = (uint)packet.Length;
                        status = DispatchBatchPhase(ref stage, out reply);
                    }
                    if (status != BatchStatus.OK) return BatchFailure(status, count);
                    offset += rows;
                    if (reply.Integer != offset || reply.OutputBytes != 0 || reply.CallId != scope.SceneLifetime) throw new InvalidDataException("Batch candidate acknowledged a different offset.");
                }
                BridgeRequest prepare = BatchPhase(scope, operation, token, 3, activeOnly);
                status = DispatchBatchPhase(ref prepare, out reply);
                if (status != BatchStatus.OK) return BatchFailure(status, count);
                if (reply.OutputBytes != 0 || reply.Generation != outputBytes || reply.Index > (uint)count || reply.Flags > (uint)count
                    || (ulong)reply.Index + reply.Flags != (uint)count || reply.CallId != scope.SceneLifetime) throw new InvalidDataException("Invalid aggregate batch result metadata.");
                int applied = (int)reply.Index, skipped = (int)reply.Flags;
                for (int page = 0; page < pageCount; page++) {
                    ulong start = (ulong)page * (uint)packetBytes; int bytes = (int)Math.Min((ulong)packetBytes, outputBytes - start);
                    fixed (byte* output = m_batchResultPages[page]) {
                        BridgeRequest query = BatchPhase(scope, operation, token, 4, activeOnly); query.Output = output; query.OutputCapacity = (uint)bytes;
                        query.Reserved0 = (uint)start; query.Reserved1 = (uint)(start >> 32);
                        status = DispatchBatchPhase(ref query, out reply);
                    }
                    if (status != BatchStatus.OK) return BatchFailure(status, count);
                    if (reply.Generation != outputBytes || reply.OutputBytes != bytes || reply.Integer != (long)(start + (uint)bytes)
                        || reply.Flags != (start + (uint)bytes == outputBytes ? 1U : 0U) || reply.CallId != scope.SceneLifetime) throw new InvalidDataException("Batch result snapshot page changed.");
                }
                if (outputStride != 0) {
                    int valid = 0;
                    for (int index = 0; index < count; index++) {
                        ReadOnlySpan<byte> row = BatchResultRow(index, outputStride, packetBytes);
                        uint slot = BinaryPrimitives.ReadUInt32LittleEndian(row);
                        if (slot > (uint)ObjectStatus.EXPIRED) throw new InvalidDataException("Unknown aggregate batch slot status.");
                        if (slot == (uint)ObjectStatus.VALID) {
                            if (read) ValidateBatchPosition(row);
                            valid++;
                        }
                    }
                    if (valid != applied || count - valid != skipped) throw new InvalidDataException("Aggregate batch slot counts disagree.");
                }
                /// @note 全結果を staging へ受信・検証してから固定長 commit。変更後に失敗し得る page を要求しない。
                BridgeRequest publish = BatchPhase(scope, operation, token, 6, activeOnly);
                status = DispatchBatchPhase(ref publish, out reply);
                if (status != BatchStatus.OK) return BatchFailure(status, count);
                if (reply.Index != (uint)applied || reply.Flags != (uint)skipped || reply.OutputBytes != 0 || reply.Generation != outputBytes || reply.CallId != scope.SceneLifetime)
                    throw new InvalidDataException("Final batch publication acknowledgment changed.");
                if (outputStride != 0) {
                    for (int index = 0; index < count; index++) {
                        ReadOnlySpan<byte> row = BatchResultRow(index, outputStride, packetBytes); ObjectStatus slot = (ObjectStatus)BinaryPrimitives.ReadUInt32LittleEndian(row);
                        statuses[index] = slot;
                        if (read && slot == ObjectStatus.VALID) outputPositions[index] = new(BinaryPrimitives.ReadSingleLittleEndian(row[4..]), BinaryPrimitives.ReadSingleLittleEndian(row[8..]), BinaryPrimitives.ReadSingleLittleEndian(row[12..]));
                    }
                }
                return new(BatchStatus.OK, count, applied, skipped);
            } finally {
                if (token != 0) { try { BridgeRequest close = BatchPhase(scope, operation, token, 5, activeOnly); _ = DispatchBatchPhase(ref close, out _); } catch { } }
                m_batchInFlight = false;
            }
        }
        ReadOnlySpan<byte> BatchResultRow(int index, int stride, int packetBytes)
        {
            ulong offset = (ulong)index * (uint)stride;
            return m_batchResultPages[(int)(offset / (uint)packetBytes)].AsSpan((int)(offset % (uint)packetBytes), stride);
        }
        static void ValidateBatchPosition(ReadOnlySpan<byte> row)
        {
            if (!float.IsFinite(BinaryPrimitives.ReadSingleLittleEndian(row[4..]))
                || !float.IsFinite(BinaryPrimitives.ReadSingleLittleEndian(row[8..]))
                || !float.IsFinite(BinaryPrimitives.ReadSingleLittleEndian(row[12..]))) throw new InvalidDataException("Batch reply contains a nonfinite valid world position.");
        }
        static BridgeRequest BatchPhase(ScriptExecutionScope scope, BridgeOp operation, ulong token, uint phase, bool activeOnly)
        {
            BridgeRequest request = Request(scope, 0, operation); request.Integer = checked((long)token); request.Flags = (phase << 8) | (activeOnly ? 1U : 0U); return request;
        }
        BatchStatus DispatchBatchPhase(ref BridgeRequest request, out BridgeReply reply)
        {
            BridgeReply result = default; int code;
            fixed (BridgeRequest* pointer = &request) code = m_dispatch(pointer, &result);
            reply = result;
            if (result.Status != BridgeStatus.OK && (result.Index != 0 || result.Flags != 0 || result.OutputBytes != 0))
                throw new InvalidDataException("Failed batch phase returned partial result metadata.");
            if (result.Status == BridgeStatus.NOT_FOUND && code == (int)result.Status) return BatchStatus.INVALID_SCOPE;
            return BatchStatusFor(code, result.Status);
        }
    }
}
