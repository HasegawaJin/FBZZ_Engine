/// @file    ScriptFieldCodec.cs
/// @brief   一次元配列の固定 LE codec と副作用なしの JSON 設定値検証。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using Fbzz;

namespace Fbzz.ManagedHost;

/// @note binary 配列は count32LE、固定値列または UTF8 length32LE + bytes。GO null slot は length=0xFFFFFFFF、配列自体の null は拒否する。
/// @see https://learn.microsoft.com/en-us/dotnet/api/system.io.binarywriter numeric writes are little-endian。
internal static class ScriptFieldCodec
{
    static readonly UTF8Encoding s_utf8 = new(false, true);
    internal static byte[] EncodeArray(Array array, ScriptFieldKind element)
    {
        using MemoryStream stream = new();
        using BinaryWriter writer = new(stream, s_utf8, leaveOpen: true);
        writer.Write((uint)array.Length);
        foreach (object? value in array)
        {
            switch (element)
            {
                case ScriptFieldKind.FLOAT: writer.Write((float)value!); break;
                case ScriptFieldKind.INT: writer.Write(Convert.ToInt32(value)); break;
                case ScriptFieldKind.BOOL: writer.Write((byte)((bool)value! ? 1 : 0)); break;
                case ScriptFieldKind.VECTOR3:
                    Vector3 vector = (Vector3)value!; writer.Write(vector.X); writer.Write(vector.Y); writer.Write(vector.Z); break;
                case ScriptFieldKind.STRING: WriteString(writer, (string)value!); break;
                case ScriptFieldKind.GAME_OBJECT: WriteString(writer, (value as GameObject)?.Guid); break;
                default: throw new InvalidDataException("Unsupported array element kind.");
            }
        }
        return stream.ToArray();
    }

    internal static Array DecodeArray(ReadOnlySpan<byte> bytes, ScriptFieldDescriptor field, Func<string, GameObject> resolve)
    {
        using MemoryStream stream = new(bytes.ToArray(), writable: false);
        using BinaryReader reader = new(stream, s_utf8, leaveOpen: true);
        uint count = reader.ReadUInt32();
        int minimum = field.ElementKind switch { ScriptFieldKind.BOOL => 1, ScriptFieldKind.VECTOR3 => 12, _ => 4 };
        if (count > int.MaxValue || (ulong)count * (uint)minimum > (ulong)(stream.Length - stream.Position))
            throw new InvalidDataException("Array count exceeds its payload.");
        Array result = Array.CreateInstance(field.ElementType, (int)count);
        for (int index = 0; index < count; index++)
        {
            object? value = field.ElementKind switch
            {
                ScriptFieldKind.FLOAT => reader.ReadSingle(),
                ScriptFieldKind.INT => DecodeInteger(reader.ReadInt32(), field.ElementType),
                ScriptFieldKind.BOOL => ReadBoolean(reader),
                ScriptFieldKind.VECTOR3 => new Vector3(reader.ReadSingle(), reader.ReadSingle(), reader.ReadSingle()),
                ScriptFieldKind.STRING => ReadString(reader) ?? throw new InvalidDataException("String array elements cannot be null."),
                ScriptFieldKind.GAME_OBJECT => ResolveNullable(ReadString(reader), resolve),
                _ => throw new InvalidDataException("Unsupported array kind.")
            };
            result.SetValue(value, index);
        }
        if (stream.Position != stream.Length || !field.Validate(result)) throw new InvalidDataException("Array values or payload length are invalid.");
        return result;
    }

    internal static object? DecodeJson(JsonElement json, ScriptFieldDescriptor field, Func<string, GameObject> resolve,
        Func<string, string, GameObject>? resolveReference = null,
        Func<string, ComponentReferenceIdentity, Type, ComponentReferenceIdentity>? resolveComponent = null)
    {
        object? value = StructuredFieldCodec.DecodeValue(json, field.ValueType, resolve, resolveReference, resolveComponent: resolveComponent);
        if (!field.Validate(value)) throw new InvalidDataException("Setting value violates its declared constraints.");
        return value;
    }

    internal static string SchemaJson(ScriptFieldDescriptor field)
    {
        Dictionary<string, object?> value = ManagedFieldSchema.Build(field.ValueType);
        if (field.Range is ValueRangeAttribute range) ManagedFieldSchema.RangeDefault(value, range, field.ValueType);
        Dictionary<string, object?> schema = new(value)
        {
            ["name"] = field.Key, ["kind"] = (uint)field.Kind, ["isArray"] = field.IsArray,
            ["elementKind"] = (uint)field.ElementKind, ["requiredReference"] = field.RequiredReference,
            ["previousKeys"] = field.PreviousKeys, ["label"] = field.Label, ["help"] = field.Metadata?.Help,
            ["group"] = field.Metadata?.Group, ["unit"] = field.Metadata?.Unit, ["visible"] = field.Visible,
            ["minimum"] = ManagedFieldSchema.RangeBound(field.ValueType, field.Range, false), ["maximum"] = ManagedFieldSchema.RangeBound(field.ValueType, field.Range, true),
            ["value"] = value, ["nested"] = value, ["enumValues"] = EnumValues(field.ElementType),
            ["flags"] = field.ElementType.IsEnum && field.ElementType.IsDefined(typeof(FlagsAttribute), false)
        };
        return JsonSerializer.Serialize(schema);
    }

    internal static object? EnumValues(Type type) => type.IsEnum
        ? Enum.GetNames(type).Select(name => new { name, value = ManagedPrimitiveCodec.WireValue(Enum.Parse(type, name), type) }).ToArray() : null;

    /// @note Flags accepts combinations of declared bits, including unnamed combinations, while rejecting unknown bits.
    /// @see https://learn.microsoft.com/en-us/dotnet/api/system.flagsattribute FlagsAttribute bitwise combinations.
    internal static bool ValidEnum(Type type, object value)
    {
        if (!type.IsDefined(typeof(FlagsAttribute), false)) return Enum.IsDefined(type, value);
        ulong allowed = 0;
        foreach (object candidate in Enum.GetValues(type)) allowed |= ManagedPrimitiveCodec.Bits(candidate, type);
        return (ManagedPrimitiveCodec.Bits(value, type) & ~allowed) == 0;
    }

    internal static string ValidateGuid(string guid)
    {
        if (string.IsNullOrWhiteSpace(guid) || guid.Contains('\0')) throw new InvalidDataException("A GameObject GUID must be nonempty.");
        return guid;
    }
    static object DecodeInteger(int value, Type type) => type.IsEnum ? Enum.ToObject(type, value) : value;
    static GameObject? ResolveNullable(string? guid, Func<string, GameObject> resolve) => guid is null ? null : resolve(ValidateGuid(guid));
    static bool ReadBoolean(BinaryReader reader)
    {
        byte value = reader.ReadByte();
        if (value > 1) throw new InvalidDataException("Boolean elements must be zero or one.");
        return value != 0;
    }
    static void WriteString(BinaryWriter writer, string? value)
    {
        if (value is null) { writer.Write(uint.MaxValue); return; }
        byte[] bytes = s_utf8.GetBytes(value);
        writer.Write((uint)bytes.Length); writer.Write(bytes);
    }
    static string? ReadString(BinaryReader reader)
    {
        uint length = reader.ReadUInt32();
        if (length == uint.MaxValue) return null;
        if (length > int.MaxValue || length > reader.BaseStream.Length - reader.BaseStream.Position) throw new InvalidDataException("UTF8 element length exceeds its payload.");
        return s_utf8.GetString(reader.ReadBytes((int)length));
    }
}
