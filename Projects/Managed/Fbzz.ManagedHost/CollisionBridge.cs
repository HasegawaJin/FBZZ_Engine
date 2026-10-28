/// @file    CollisionBridge.cs
/// @brief   native の物理通知を寿命付き参照と値スナップショットへ復元する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Buffers.Binary;
using System.IO;
using Fbzz;

namespace Fbzz.ManagedHost;

public static unsafe partial class EntryPoint
{
    /// @note 相手は現在の scene に属する識別子。Host runtime が受信個体と同じ寿命か検証する。
    static Collision DecodeCollision(BridgeRequest request)
    {
        ReadOnlySpan<byte> input = new(request.Input, checked((int)request.InputBytes));
        if (input.Length < BridgeAbi.COLLISION_HEADER_BYTES) throw new InvalidDataException("Collision notification is truncated.");
        ulong objectId = BinaryPrimitives.ReadUInt64LittleEndian(input);
        ulong lifetime = BinaryPrimitives.ReadUInt64LittleEndian(input[8..]);
        uint length = BinaryPrimitives.ReadUInt32LittleEndian(input[64..]);
        if (length != input.Length - BridgeAbi.COLLISION_HEADER_BYTES) throw new InvalidDataException("Collision GUID length is invalid.");
        GameObject? other = null;
        if (objectId == 0)
        {
            if (lifetime != 0 || length != 0) throw new InvalidDataException("A missing collision object must have no identity.");
        }
        else
        {
            if (lifetime == 0) throw new InvalidDataException("Collision scene lifetime is missing.");
            string guid = ScriptFieldCodec.ValidateGuid(s_utf8.GetString(input[(int)BridgeAbi.COLLISION_HEADER_BYTES..]));
            other = new GameObject(objectId, lifetime, guid);
        }
        Span<float> values = stackalloc float[12];
        for (int index = 0; index < values.Length; index++)
        {
            values[index] = BinaryPrimitives.ReadSingleLittleEndian(input[(16 + index * 4)..]);
            if (!float.IsFinite(values[index])) throw new InvalidDataException("Collision notification contains non-finite data.");
        }
        return new Collision(other, new Vector3(values[0], values[1], values[2]), new Vector3(values[3], values[4], values[5]),
            values[6], new Vector3(values[7], values[8], values[9]), values[10], values[11]);
    }
}
