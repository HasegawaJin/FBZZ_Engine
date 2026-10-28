/// @file    InspectorSchemaTests.cs
/// @brief   Inspector と AI に共通の列挙候補・入れ子 metadata と既存保存契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class InspectorSchemaTests
{
    internal static (string Name, Action Test)[] Cases =>
    [
        ("InspectorSparseEnumAndFlagsContract", EnumContract),
        ("InspectorNestedMetadataAndNullability", Metadata),
        ("InspectorStructuredListOrderRoundtrip", ListOrder),
        ("InspectorAuthoringMathAndUnsignedValues", AuthoringValues),
        ("InspectorSnapshotRetainsExactReferencePaths", SnapshotReferences),
        ("InspectorSchemaDiscoveryIsInert", DiscoveryIsInert),
        ("InspectorRepeatedNestedSchemaAndCodecRemainConsistent", RepeatedNestedSchema)
    ];

    enum Mode { NEGATIVE = -3, ZERO = 0, SPARSE = 12 }
    [Flags] enum Options { NONE = 0, FIRST = 1, FOURTH = 8, HIGH = int.MinValue }
    sealed class Settings : Script
    {
        public Mode mode = Mode.ZERO;
        public Options options = Options.NONE;
        public Mode[] modes = [];
        public Options[] optionArray = [];
        public uint mask = uint.MaxValue;
        public GameObject[] targets = [];
        [ValueRange(0, 10)] public List<uint> counts = [];
    }
    [Serializable] sealed class Nested
    {
        [Setting(Label = "速度", Help = "秒速", Group = "移動", Unit = "m/s", Visible = false)]
        [ValueRange(1, 5)] public float speed = 2;
        public Mode mode = Mode.NEGATIVE;
        public Options options = Options.FIRST | Options.FOURTH;
    }
    [Serializable] struct ValueSettings { public float speed; }

    static ScriptFieldDescriptor Field(string name)
    {
        FieldInfo field = typeof(Settings).GetField(name)!;
        return new(field, name, ScriptCatalogBuilder.GetKind(field.FieldType), true, false, []);
    }
    static object? Decode(string value, ScriptFieldDescriptor field)
    {
        using JsonDocument json = JsonDocument.Parse(value);
        return ScriptFieldCodec.DecodeJson(json.RootElement, field, _ => throw new InvalidDataException("Unexpected reference."));
    }
    static void EnumContract()
    {
        ScriptFieldDescriptor mode = Field("mode"), options = Field("options");
        Program.Check(Equals(Decode("-3", mode), Mode.NEGATIVE) && Equals(Decode("12", mode), Mode.SPARSE),
            "Sparse and negative enum values must retain their actual persisted integers.");
        Program.Throws<InvalidDataException>(() => Decode("1", mode));
        Program.Check(Equals(Decode("9", options), Options.FIRST | Options.FOURTH)
            && options.Validate(Options.HIGH | Options.FIRST) && options.Validate(Options.NONE),
            "Flags must accept unnamed combinations, zero, and a declared signed high bit.");
        Program.Throws<InvalidDataException>(() => Decode("2", options));
        Program.Check(((Options[])Decode("[9,-2147483647]", Field("optionArray"))!).Length == 2,
            "Flag array validation must share the scalar combination contract.");
        using JsonDocument schema = JsonDocument.Parse(ScriptFieldCodec.SchemaJson(Field("modes")));
        JsonElement[] candidates = schema.RootElement.GetProperty("enumValues").EnumerateArray().ToArray();
        Program.Check(candidates.Any(candidate => candidate.GetProperty("name").GetString() == "NEGATIVE" && candidate.GetProperty("value").GetInt32() == -3)
            && schema.RootElement.GetProperty("element").GetProperty("enumValues").GetArrayLength() == 3,
            "Enum arrays must expose candidate names and real integers to Inspector and AI.");
        Program.Check(StructuredFieldCodec.Validate(new Nested(), typeof(Nested)), "Nested unnamed Flags combinations must encode.");
        Nested bad = new() { options = (Options)2 };
        Program.Check(!StructuredFieldCodec.Validate(bad, typeof(Nested)), "Nested Flags must reject undeclared bits.");
    }
    static void Metadata()
    {
        using JsonDocument schema = JsonDocument.Parse(JsonSerializer.Serialize(StructuredFieldCodec.Schema(typeof(Nested))));
        JsonElement speed = schema.RootElement.GetProperty("fields").EnumerateArray().First(field => field.GetProperty("name").GetString() == "speed");
        Program.Check(!speed.GetProperty("visible").GetBoolean() && speed.GetProperty("label").GetString() == "速度"
            && speed.GetProperty("help").GetString() == "秒速" && speed.GetProperty("group").GetString() == "移動"
            && speed.GetProperty("unit").GetString() == "m/s" && speed.GetProperty("minimum").GetSingle() == 1
            && speed.GetProperty("maximum").GetSingle() == 5,
            "Nested settings metadata must expose visibility, range, help, group, and unit independently of persistence.");
        string encoded = StructuredFieldCodec.Encode(new Nested(), typeof(Nested));
        Program.Check(encoded.Contains("\"speed\":2", StringComparison.Ordinal), "Hidden fields must still be saved.");
        using JsonDocument valueSchema = JsonDocument.Parse(JsonSerializer.Serialize(StructuredFieldCodec.Schema(typeof(ValueSettings))));
        using JsonDocument listSchema = JsonDocument.Parse(JsonSerializer.Serialize(StructuredFieldCodec.Schema(typeof(List<ValueSettings>))));
        Program.Check(schema.RootElement.GetProperty("mayBeNull").GetBoolean()
            && !valueSchema.RootElement.GetProperty("mayBeNull").GetBoolean()
            && !listSchema.RootElement.GetProperty("mayBeNull").GetBoolean(),
            "Only supported reference values may expose a null action; structs and collections must remain nonnull.");
        using JsonDocument invalid = JsonDocument.Parse("{\"speed\":9}");
        Program.Throws<InvalidDataException>(() => StructuredFieldCodec.DecodeValue(invalid.RootElement, typeof(Nested), _ => throw new InvalidDataException()));
    }
    static void ListOrder()
    {
        List<Nested> items = [new() { speed = 1 }, new() { speed = 4 }, new() { speed = 2 }];
        (items[0], items[2]) = (items[2], items[0]);
        string encoded = StructuredFieldCodec.Encode(items, typeof(List<Nested>));
        List<Nested> restored = (List<Nested>)StructuredFieldCodec.Decode(encoded, typeof(List<Nested>), _ => throw new InvalidDataException())!;
        Program.Check(restored.Select(item => item.speed).SequenceEqual(new float[] { 2, 4, 1 })
            && restored.All(item => item.options == (Options.FIRST | Options.FOURTH)),
            "Reordered structured lists must preserve sequence, hidden values, and unnamed Flags combinations through saving.");
        string value = StructuredFieldCodec.Encode(new ValueSettings { speed = 3 }, typeof(ValueSettings));
        Program.Check(((ValueSettings)StructuredFieldCodec.Decode(value, typeof(ValueSettings), _ => throw new InvalidDataException())!).speed == 3,
            "A value-type setting must restore as a value, preserving its nonnull contract.");
    }
    [Serializable] sealed class AuthoringSettings
    {
        public Vector2 point = new(1.25f, -2);
        public Vector4 color = new(0.25f, 1, 2, 0.5f);
        public uint mask = uint.MaxValue;
        public Vector2[] path = [new(3, 4), new(-5, 6)];
        public List<Vector4> colors = [new(1, 2, 3, 4)];
        public List<uint> counts = [0, 2147483648U, uint.MaxValue];
    }
    static void AuthoringValues()
    {
        AuthoringSettings original = new();
        string encoded = StructuredFieldCodec.Encode(original, typeof(AuthoringSettings));
        AuthoringSettings restored = (AuthoringSettings)StructuredFieldCodec.Decode(encoded, typeof(AuthoringSettings), _ => throw new InvalidDataException())!;
        Program.Check(restored.mask == uint.MaxValue && restored.counts.SequenceEqual(original.counts)
            && restored.point.X == 1.25f && restored.point.Y == -2 && restored.color.Z == 2 && restored.color.W == 0.5f
            && restored.path.Length == 2 && restored.path[1].X == -5 && restored.colors[0].W == 4,
            "Vector2/4, uint and their arrays/Lists must preserve exact values through the shared codec.");
        using JsonDocument schema = JsonDocument.Parse(JsonSerializer.Serialize(ManagedFieldSchema.Build(typeof(AuthoringSettings))));
        Program.Check(schema.RootElement.GetProperty("createValue").GetProperty("mask").GetUInt32() == 0,
            "Schema defaults must be constructor-free type defaults; live snapshots retain declared initializers.");
        using JsonDocument negative = JsonDocument.Parse("-1");
        using JsonDocument overflow = JsonDocument.Parse("4294967296");
        Program.Throws<InvalidDataException>(() => StructuredFieldCodec.DecodeValue(negative.RootElement, typeof(uint), _ => throw new InvalidDataException()));
        Program.Throws<InvalidDataException>(() => StructuredFieldCodec.DecodeValue(overflow.RootElement, typeof(uint), _ => throw new InvalidDataException()));
        Program.Check(!StructuredFieldCodec.Validate(new Vector2(float.NaN, 0), typeof(Vector2))
            && !StructuredFieldCodec.Validate(new Vector4(0, 0, 0, float.PositiveInfinity), typeof(Vector4)),
            "Every saved math component must be finite.");
        Program.Check(((List<uint>)Decode("[0,10]", Field("counts"))!).SequenceEqual(new uint[] { 0, 10 }),
            "Range constraints must apply to uint List elements through the same validator as scalar fields.");
        Program.Throws<InvalidDataException>(() => Decode("[11]", Field("counts")));
    }
    static void SnapshotReferences()
    {
        Settings settings = new() { targets = [new GameObject(202, 1, "same-guid"), new GameObject(303, 1, "same-guid")] };
        ScriptFieldDescriptor[] fields = typeof(Settings).GetFields(BindingFlags.Instance | BindingFlags.Public).Select(field => Field(field.Name)).ToArray();
        ConstructorInfo constructor = typeof(Settings).GetConstructor(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic, null, Type.EmptyTypes, null)!;
        ScriptTypeDescriptor type = new(typeof(Settings), constructor, fields, []);
        ManagedScriptRuntime.ScriptInstance instance = new(1, 101, type, settings, new ScriptBinding(1, 9, 101), new());
        using JsonDocument before = JsonDocument.Parse(ManagedSettingsSnapshot.Read(instance, 1, 9));
        JsonElement refs = before.RootElement.GetProperty("references");
        Program.Check(refs.GetProperty("/targets/0").GetProperty("objectId").GetString() == "202"
            && refs.GetProperty("/targets/1").GetProperty("objectId").GetString() == "303"
            && before.RootElement.GetProperty("settings").GetProperty("mask").GetUInt32() == uint.MaxValue,
            "Snapshot paths must distinguish same-GUID tokens and preserve uint values without float conversion.");
        (settings.targets[0], settings.targets[1]) = (settings.targets[1], settings.targets[0]);
        using JsonDocument after = JsonDocument.Parse(ManagedSettingsSnapshot.Read(instance, 1, 9));
        Program.Check(after.RootElement.GetProperty("references").GetProperty("/targets/0").GetProperty("objectId").GetString() == "303"
            && before.RootElement.GetProperty("revision").GetString() != after.RootElement.GetProperty("revision").GetString(),
            "Changing exact token order must change snapshot revision even when all saved GUIDs remain identical.");
    }
    enum Wide : ulong { HIGH = ulong.MaxValue }
    [Serializable] sealed class ConstructorProbe
    {
        internal static int Calls;
        public ConstructorProbe() { Calls++; throw new InvalidOperationException("Schema must never construct this helper."); }
        [ValueRange(1, 5)] public int count = 3;
        public ConstructorProbe? next = null;
    }
    static void DiscoveryIsInert()
    {
        ConstructorProbe.Calls = 0;
        using JsonDocument schema = JsonDocument.Parse(JsonSerializer.Serialize(ManagedFieldSchema.Build(typeof(ConstructorProbe))));
        Program.Check(ConstructorProbe.Calls == 0 && schema.RootElement.GetProperty("createValue").GetProperty("count").GetInt32() == 1
            && schema.RootElement.GetProperty("createValue").GetProperty("next").ValueKind == JsonValueKind.Null,
            "Schema discovery must not run throwing constructors and must produce range-valid defaults for hidden or recursive fields.");
        using JsonDocument empty = JsonDocument.Parse("{}");
        ConstructorProbe restored = (ConstructorProbe)StructuredFieldCodec.DecodeValue(empty.RootElement, typeof(ConstructorProbe), _ => throw new InvalidOperationException())!;
        Program.Check(ConstructorProbe.Calls == 0 && restored.count == 1 && restored.next is null,
            "Decoding missing nested fields must use inert schema defaults without executing throwing or recursive constructors.");
        using JsonDocument wide = JsonDocument.Parse(JsonSerializer.Serialize(ManagedFieldSchema.Build(typeof(Wide))));
        Program.Check(wide.RootElement.GetProperty("valueKind").GetString() == "enum" && wide.RootElement.GetProperty("numericKind").GetString() == "uint64"
            && wide.RootElement.GetProperty("enumValues")[0].GetProperty("value").GetString() == "18446744073709551615",
            "Wide enum metadata must remain inert and preserve its exact candidates without integer overflow.");
        using JsonDocument unsupported = JsonDocument.Parse(JsonSerializer.Serialize(ManagedFieldSchema.Build(typeof(DateTime))));
        Program.Check(!unsupported.RootElement.GetProperty("supported").GetBoolean() && unsupported.RootElement.GetProperty("enumValues").ValueKind == JsonValueKind.Null,
            "Unrecognized framework values must retain an explicit inert unsupported schema.");
    }
    static void RepeatedNestedSchema()
    {
        string first = JsonSerializer.Serialize(ManagedFieldSchema.Build(typeof(Nested)));
        string second = JsonSerializer.Serialize(ManagedFieldSchema.Build(typeof(Nested)));
        Nested original = new() { speed = 4, mode = Mode.SPARSE, options = Options.HIGH | Options.FIRST };
        string encoded = StructuredFieldCodec.Encode(original, typeof(Nested));
        Nested restored = (Nested)StructuredFieldCodec.Decode(encoded, typeof(Nested), _ => throw new InvalidDataException())!;
        Program.Check(first == second && restored.speed == original.speed && restored.mode == original.mode && restored.options == original.options,
            "Repeated schema discovery and codec use must preserve deterministic metadata, exact enum bits, and nested values.");
    }
}
