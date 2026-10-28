/// @file    PrimitiveCodecTests.cs
/// @brief   Exact primitive values, enum widths and typed range boundaries across recursive settings snapshots.
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class PrimitiveCodecTests
{
    internal static (string Name, Action Test)[] Cases => [
        ("ExactPrimitiveScalarAndSnapshotRoundtrip", Scalars),
        ("ExactPrimitiveEnumsAndRecursiveValues", Enums),
        ("ExactPrimitiveRangesAndInertDefaults", Ranges)
    ];
    enum Signed8 : sbyte { LOW = sbyte.MinValue, HIGH = sbyte.MaxValue }
    enum Unsigned8 : byte { HIGH = byte.MaxValue }
    enum Signed16 : short { LOW = short.MinValue }
    enum Unsigned16 : ushort { HIGH = ushort.MaxValue }
    enum Signed32 : int { LOW = int.MinValue }
    enum Unsigned32 : uint { HIGH = uint.MaxValue }
    enum Signed64 : long { LOW = long.MinValue, ABOVE_JS_PRECISION = 9007199254740993 }
    [Flags] enum Unsigned64 : ulong { NONE = 0, LOW = 1, HIGH = 1UL << 63 }
    [Serializable] sealed class Values
    {
        public long signed = long.MinValue;
        public ulong unsigned = ulong.MaxValue;
        public double real = double.MaxValue;
        public decimal money = decimal.MaxValue;
        public char code = '\ud800';
        public List<long> list = [long.MinValue, 9007199254740993L, long.MaxValue];
        public Unsigned64[] flags = [Unsigned64.HIGH | Unsigned64.LOW];
    }
    sealed class Bounds : Script
    {
        [ValueRange(long.MaxValue - 1, long.MaxValue)] public long signed = long.MaxValue;
        [ValueRange(ulong.MaxValue - 1, ulong.MaxValue)] public ulong unsigned = ulong.MaxValue;
        [ValueRange("0.1", "0.5")] public decimal money = 0.3m;
        [ValueRange(9007199254740993L, 9007199254740995L)] public double precise = 9007199254740994d;
        [ValueRange(16777217, 16777219)] public float single = 16777218f;
    }
    static object? Decode(string json, Type type)
    { using JsonDocument document = JsonDocument.Parse(json); return StructuredFieldCodec.DecodeValue(document.RootElement, type, _ => throw new InvalidDataException("Unexpected reference.")); }
    static void Scalars()
    {
        object[] values = [sbyte.MinValue, byte.MaxValue, short.MinValue, ushort.MaxValue, int.MinValue, uint.MaxValue,
            long.MinValue, long.MaxValue, ulong.MaxValue, float.MaxValue, double.MaxValue, double.Epsilon, decimal.MaxValue,
            decimal.MinValue, 0.0000000000000000000000000001m, '\ud800', '\udfff', '\0'];
        foreach (object value in values) {
            Type type = value.GetType(); string json = StructuredFieldCodec.EncodeValue(value, type);
            Program.Check(Equals(value, Decode(json, type)), $"Exact scalar roundtrip failed: {type.Name}.");
            using JsonDocument wire = JsonDocument.Parse(json);
            Program.Check(wire.RootElement.ValueKind == (type == typeof(long) || type == typeof(ulong) || type == typeof(decimal) ? JsonValueKind.String : JsonValueKind.Number), "Wire encoding must follow the declared primitive schema.");
            using ImmutableValueSnapshot snapshot = ImmutableValueSnapshot.Capture(value, type);
            Program.Check(Equals(value, snapshot.RestoreValue(type)), $"Immutable scalar restore failed: {type.Name}.");
            using JsonDocument page = JsonDocument.Parse(JsonSerializer.Serialize(snapshot.ReadValue("", 4096)));
            Program.Check(Equals(value, Decode(page.RootElement.GetProperty("value").GetRawText(), type)), "Paged immutable scalars must retain exact primitive values.");
        }
        foreach ((Type type, string json) in new (Type, string)[] { (typeof(long), "9007199254740993"), (typeof(ulong), "18446744073709551615"),
            (typeof(long), "\"+1\""), (typeof(long), "\"01\""), (typeof(ulong), "\"-1\""), (typeof(decimal), "\"1.00\""),
            (typeof(decimal), "\"0.00000000000000000000000000001\""), (typeof(byte), "256"), (typeof(sbyte), "-129"),
            (typeof(char), "65536"), (typeof(char), "\"x\""), (typeof(int), "true"), (typeof(double), "1e999") }) {
            bool rejected = false; try { _ = Decode(json, type); } catch (Exception) { rejected = true; }
            Program.Check(rejected, $"Malformed or inexact {type.Name} input was accepted.");
        }
        Program.Check(!StructuredFieldCodec.Validate(double.PositiveInfinity, typeof(double)) && !StructuredFieldCodec.Validate(float.NaN, typeof(float)), "Only finite binary floating point settings are persisted.");
        foreach ((decimal value, string canonical) in new (decimal, string)[] { (1.2300m, "1.23"), (1.00m, "1"), (new decimal(0, 0, 0, true, 28), "0"),
            (decimal.MaxValue, "79228162514264337593543950335"), (decimal.MinValue, "-79228162514264337593543950335") }) {
            string encoded = StructuredFieldCodec.EncodeValue(value, typeof(decimal));
            Program.Check(encoded == JsonSerializer.Serialize(canonical) && Equals(value, Decode(encoded, typeof(decimal))), "Decimal scale/zero normalization must retain the exact value and canonical extremal strings.");
        }
    }
    static void Enums()
    {
        object[] values = [Signed8.LOW, Signed8.HIGH, Unsigned8.HIGH, Signed16.LOW, Unsigned16.HIGH, Signed32.LOW, Unsigned32.HIGH,
            Signed64.LOW, Signed64.ABOVE_JS_PRECISION, Unsigned64.HIGH | Unsigned64.LOW];
        foreach (object value in values) {
            Type type = value.GetType(); Program.Check(ManagedFieldSchema.IsSupported(type), "All legal integral enum widths belong to the common schema.");
            Program.Check(Equals(value, Decode(StructuredFieldCodec.EncodeValue(value, type), type)), "Enum values must retain their exact underlying type.");
            using ImmutableValueSnapshot snapshot = ImmutableValueSnapshot.Capture(value, type); Program.Check(Equals(value, snapshot.RestoreValue(type)), "Snapshot enum values must retain signedness and width.");
            using JsonDocument schema = JsonDocument.Parse(JsonSerializer.Serialize(ManagedFieldSchema.Build(type)));
            Program.Check(schema.RootElement.GetProperty("valueKind").GetString() == "enum" && schema.RootElement.GetProperty("enumValues").GetArrayLength() != 0, "Sparse enum candidates remain editable.");
        }
        Program.Throws<InvalidDataException>(() => Decode("\"9223372036854775810\"", typeof(Unsigned64)));
        Values original = new(); string encoded = StructuredFieldCodec.EncodeValue(original, typeof(Values)); Values restored = (Values)Decode(encoded, typeof(Values))!;
        Program.Check(restored.signed == original.signed && restored.unsigned == original.unsigned && restored.real == original.real && restored.money == original.money
            && restored.code == original.code && restored.list.SequenceEqual(original.list) && restored.flags.SequenceEqual(original.flags), "Recursive helper/List/array paths use the same lossless primitive codec.");
    }
    static void Ranges()
    {
        foreach (string name in new[] { "signed", "unsigned", "money", "precise", "single" }) {
            FieldInfo member = typeof(Bounds).GetField(name)!;
            ScriptFieldDescriptor field = new(member, name, ScriptCatalogBuilder.GetKind(member.FieldType), true, false, []);
            using JsonDocument schema = JsonDocument.Parse(ScriptFieldCodec.SchemaJson(field)); JsonElement node = schema.RootElement.GetProperty("value");
            object? initial = Decode(node.GetProperty("defaultValue").GetRawText(), member.FieldType);
            Program.Check(field.Validate(initial), "Hidden/inert defaults must satisfy exact typed ranges independently of rendering.");
            if (name == "signed") Program.Check((long)initial! == long.MaxValue - 1 && node.GetProperty("minimum").GetString() == "9223372036854775806", "Int64 range bounds must not pass through double.");
            if (name == "unsigned") Program.Check((ulong)initial! == ulong.MaxValue - 1 && node.GetProperty("maximum").GetString() == "18446744073709551615", "UInt64 range bounds must remain exact.");
            if (name == "money") Program.Check((decimal)initial! == 0.1m && node.GetProperty("minimum").GetString() == "0.1", "Decimal ranges use exact invariant strings.");
            if (name == "precise") Program.Check((double)initial! == 9007199254740994d && !field.Validate(9007199254740992d), "Binary64 must compare exact integral bounds beyond JavaScript precision.");
            if (name == "single") Program.Check((float)initial! == 16777218f && !field.Validate(16777216f), "Binary32 defaults select a representable in-range value.");
        }
    }
}
