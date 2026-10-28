/// @file    EngineNotificationBridge.cs
/// @brief   Engine 通知の bounded packet を所有する型付き値へ復元する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
using System.Buffers.Binary;
using System.IO;
using Fbzz;

namespace Fbzz.ManagedHost;

public static unsafe partial class EntryPoint
{
    const int ENGINE_NOTIFICATION_MAX_BYTES = 1024 * 1024;
    static object DecodeEngineNotification(BridgeRequest request, ScriptCallback callback)
    {
        if (request.InputBytes > ENGINE_NOTIFICATION_MAX_BYTES) throw new InvalidDataException("Engine notification exceeds its byte budget.");
        ReadOnlySpan<byte> input = new(request.Input, checked((int)request.InputBytes));
        switch (callback)
        {
        case ScriptCallback.ON_ANIMATION_EVENT:
            if (input.Length < 16) throw new InvalidDataException("Animation notification is truncated.");
            uint nameBytes = BinaryPrimitives.ReadUInt32LittleEndian(input[12..]);
            if (nameBytes != input.Length - 16) throw new InvalidDataException("Animation notification has an invalid name length.");
            return new AnimationEvent(NotificationText(input[16..]), BinaryPrimitives.ReadInt32LittleEndian(input),
                NotificationFloat(input[4..]), NotificationFloat(input[8..]));
        case ScriptCallback.ON_ANIMATOR_MOVE:
            if (input.Length != 60) throw new InvalidDataException("Root motion notification has an invalid shape.");
            Span<float> values = stackalloc float[14];
            for (int index = 0; index < values.Length; index++) values[index] = NotificationFloat(input[(index * 4)..]);
            uint applied = BinaryPrimitives.ReadUInt32LittleEndian(input[56..]);
            Quaternion rotation = new(values[3], values[4], values[5], values[6]);
            if (applied > 1 || values[13] < 0 || !float.IsFinite(rotation.LengthSquared) || rotation.LengthSquared <= 0)
                throw new InvalidDataException("Root motion notification has invalid time, rotation or applied state.");
            return new RootMotionInfo(new(values[0], values[1], values[2]), rotation, new(values[7], values[8], values[9]),
                new(values[10], values[11], values[12]), values[13], applied != 0);
        case ScriptCallback.ON_SEQUENCE_EVENT:
            if (input.Length < 20) throw new InvalidDataException("Sequence notification is truncated.");
            uint eventBytes = BinaryPrimitives.ReadUInt32LittleEndian(input[12..]);
            uint sequenceBytes = BinaryPrimitives.ReadUInt32LittleEndian(input[16..]);
            if ((ulong)eventBytes + sequenceBytes != (ulong)(input.Length - 20)) throw new InvalidDataException("Sequence notification has invalid text lengths.");
            return new SequenceEvent(NotificationText(input.Slice(20, (int)eventBytes)), BinaryPrimitives.ReadInt32LittleEndian(input),
                NotificationFloat(input[4..]), NotificationFloat(input[8..]), NotificationText(input[(20 + (int)eventBytes)..]));
        case ScriptCallback.ON_SEQUENCE_FINISHED:
            return NotificationText(input);
        default:
            throw new InvalidDataException("The callback does not accept an Engine notification packet.");
        }
    }
    static float NotificationFloat(ReadOnlySpan<byte> input)
    {
        float value = BinaryPrimitives.ReadSingleLittleEndian(input);
        return float.IsFinite(value) ? value : throw new InvalidDataException("Engine notification contains a non-finite value.");
    }
    static string NotificationText(ReadOnlySpan<byte> input)
    {
        string value = s_utf8.GetString(input);
        return value.Contains('\0') ? throw new InvalidDataException("Engine notification contains a NUL name.") : value;
    }
}
