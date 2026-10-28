/// @file    NativeAiPoseBridge.cs
/// @brief   Marshal AI world poses with reused borrowed buffers and validate replies before publishing statuses.
/// @author  Hasegawa Jin
/// @date    2026-10-11
using System;
using System.Buffers.Binary;
using System.IO;
using Fbzz;
using Fbzz.AI;

namespace Fbzz.ManagedHost;

public static unsafe partial class EntryPoint
{
    sealed partial class NativeBridge : IScriptAiPoseNativeBridge
    {
        byte[] m_aiPoseInput = [], m_aiPoseOutput = [];
        bool m_aiPoseInFlight;

        /// @note Native borrows buffers only during synchronous dispatch; reentry cannot overwrite an in-flight packet.
        public BatchResult CommitWorldPoses(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects,
            ReadOnlySpan<Vector3> positions, ReadOnlySpan<Quaternion> rotations, Span<ObjectStatus> statuses)
        {
            int count = objects.Length;
            if (!ScriptApiRuntime.IsSameScope(scope, ScriptApiRuntime.Current) || scope.CallId == 0
                || scope.ThreadId != Environment.CurrentManagedThreadId || m_aiPoseInFlight)
                return AiPoseFailure(BatchStatus.INVALID_SCOPE, count);
            if (count > AiPoseWire.MAX_AGENTS || positions.Length < count || rotations.Length < count)
                return AiPoseFailure(BatchStatus.INVALID_ARGUMENT, count);
            if (statuses.Length < count) return AiPoseFailure(BatchStatus.BUFFER_TOO_SMALL, count);
            foreach (GameObject item in objects)
                if (item is not null && item.SceneLifetime != scope.SceneLifetime)
                    return AiPoseFailure(BatchStatus.SCENE_MISMATCH, count);
            int inputBytes = AiPoseWire.HEADER_BYTES + count * AiPoseWire.ROW_BYTES;
            int outputBytes = count * AiPoseWire.STATUS_BYTES;
            if (m_aiPoseInput.Length < inputBytes) m_aiPoseInput = new byte[inputBytes];
            if (m_aiPoseOutput.Length < outputBytes) m_aiPoseOutput = new byte[outputBytes];
            Span<byte> input = m_aiPoseInput.AsSpan(0, inputBytes);
            BinaryPrimitives.WriteUInt32LittleEndian(input, (uint)count);
            for (int index = 0; index < count; index++) {
                GameObject? item = objects[index];
                Span<byte> row = input.Slice(AiPoseWire.HEADER_BYTES + index * AiPoseWire.ROW_BYTES, AiPoseWire.ROW_BYTES);
                BinaryPrimitives.WriteUInt64LittleEndian(row, item?.ObjectId ?? 0);
                BinaryPrimitives.WriteUInt64LittleEndian(row[8..], item?.SceneLifetime ?? 0);
                Vector3 position = positions[index]; Quaternion rotation = rotations[index];
                BinaryPrimitives.WriteSingleLittleEndian(row[16..], position.X);
                BinaryPrimitives.WriteSingleLittleEndian(row[20..], position.Y);
                BinaryPrimitives.WriteSingleLittleEndian(row[24..], position.Z);
                BinaryPrimitives.WriteSingleLittleEndian(row[28..], rotation.X);
                BinaryPrimitives.WriteSingleLittleEndian(row[32..], rotation.Y);
                BinaryPrimitives.WriteSingleLittleEndian(row[36..], rotation.Z);
                BinaryPrimitives.WriteSingleLittleEndian(row[40..], rotation.W);
            }
            m_aiPoseInFlight = true;
            try {
                fixed (byte* inputPointer = m_aiPoseInput)
                fixed (byte* outputPointer = m_aiPoseOutput) {
                    BridgeRequest request = Request(scope, 0, BridgeOp.AI_CROWD_STEP);
                    request.Index = 1; request.Integer = count;
                    request.Input = inputPointer; request.InputBytes = (uint)inputBytes;
                    request.Output = outputBytes == 0 ? null : outputPointer; request.OutputCapacity = (uint)outputBytes;
                    BridgeReply reply = default;
                    int returned = m_dispatch(&request, &reply);
                    BatchStatus status = AiPoseStatusFor(returned, reply.Status);
                    if (status != BatchStatus.OK) {
                        if (reply.Index != 0 || reply.Flags != 0 || (status == BatchStatus.BUFFER_TOO_SMALL
                            ? reply.OutputBytes != (uint)outputBytes : reply.OutputBytes != 0 && reply.OutputBytes != (uint)outputBytes))
                            throw new InvalidDataException("Native AI pose failure returned partial or invalid result metadata.");
                        return AiPoseFailure(status, count);
                    }
                    if (reply.Index > (uint)count || reply.Flags > (uint)count || (ulong)reply.Index + reply.Flags != (uint)count
                        || reply.OutputBytes != (uint)outputBytes)
                        throw new InvalidDataException("Native AI pose reply returned invalid counts or output length.");
                    ReadOnlySpan<byte> output = m_aiPoseOutput.AsSpan(0, outputBytes);
                    int valid = 0;
                    for (int index = 0; index < count; index++) {
                        uint slot = BinaryPrimitives.ReadUInt32LittleEndian(output[(index * AiPoseWire.STATUS_BYTES)..]);
                        if (slot > (uint)ObjectStatus.EXPIRED) throw new InvalidDataException("Native AI pose reply returned an unknown slot status.");
                        if (slot == (uint)ObjectStatus.VALID) valid++;
                    }
                    if ((uint)valid != reply.Index || count - valid != (int)reply.Flags)
                        throw new InvalidDataException("Native AI pose slot statuses disagree with reply counts.");
                    for (int index = 0; index < count; index++)
                        statuses[index] = (ObjectStatus)BinaryPrimitives.ReadUInt32LittleEndian(output[(index * AiPoseWire.STATUS_BYTES)..]);
                    return new(BatchStatus.OK, count, valid, count - valid);
                }
            } finally { m_aiPoseInFlight = false; }
        }

        static BatchResult AiPoseFailure(BatchStatus status, int count) => new(status, count, 0, 0);
        static BatchStatus AiPoseStatusFor(int returned, BridgeStatus status)
        {
            if (returned != (int)status) throw new InvalidDataException("Native AI pose return code disagrees with reply status.");
            return status switch {
                BridgeStatus.OK => BatchStatus.OK,
                BridgeStatus.INVALID_ARGUMENT => BatchStatus.INVALID_ARGUMENT,
                BridgeStatus.BUFFER_TOO_SMALL => BatchStatus.BUFFER_TOO_SMALL,
                BridgeStatus.INVALID_SCOPE => BatchStatus.INVALID_SCOPE,
                BridgeStatus.NOT_FOUND => BatchStatus.NOT_FOUND,
                BridgeStatus.STALE_GENERATION => BatchStatus.STALE_GENERATION,
                BridgeStatus.SCENE_MISMATCH => BatchStatus.SCENE_MISMATCH,
                BridgeStatus.PHYSICS_BODY => BatchStatus.PHYSICS_BODY,
                _ => throw new InvalidDataException("Native AI pose returned an unexpected status.")
            };
        }
    }
}
