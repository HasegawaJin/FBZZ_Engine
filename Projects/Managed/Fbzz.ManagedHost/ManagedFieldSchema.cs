/// @file    ManagedFieldSchema.cs
/// @brief   設定型・メンバー・既定値・表示 metadata の単一 recursive schema。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using Fbzz;

namespace Fbzz.ManagedHost;

/// @note Typeキーの生存中だけFieldInfoを再利用し、値からTypeへの参照で退役コード世代を固定しない。property getterは評価しない。
internal static class ManagedFieldSchema
{
    internal sealed record Member(FieldInfo Field, string Key, string[] Previous, SettingAttribute? Metadata);
    /// @see https://learn.microsoft.com/en-us/dotnet/api/system.runtime.compilerservices.conditionalweaktable-2 ConditionalWeakTableのキーと値の生存規則。
    static readonly ConditionalWeakTable<Type, Member[]> s_fields = new();
    internal static string Kind(Type type)
    {
        if (ManagedPrimitiveCodec.Kind(type) is string primitive) return primitive;
        if (type == typeof(bool)) return "bool";
        if (type == typeof(string)) return "string";
        if (type == typeof(Vector2)) return "vector2";
        if (type == typeof(Vector3)) return "vector3";
        if (type == typeof(Vector4)) return "vector4";
        if (type == typeof(Quaternion)) return "quaternion";
        if (type == typeof(GameObject)) return "gameObject";
        if (ComponentReferenceCodec.IsReference(type)) return "componentRef";
        if (type == typeof(MaterialAssetRef)) return "materialAssetRef";
        if (type == typeof(TextureAssetRef)) return "textureAssetRef";
        if (type == typeof(ModelAssetRef)) return "modelAssetRef";
        if (type.IsGenericType && type.GetGenericTypeDefinition() == typeof(DataAssetRef<>)) return "dataAssetRef";
        if (Element(type) is not null) return "list";
        return "object";
    }
    internal static bool MayBeNull(Type type) => type == typeof(GameObject)
        || (Kind(type) == "object" && !type.IsValueType);
    internal static Type? Element(Type type) => type.IsArray && type.IsSZArray ? type.GetElementType()
        : type.IsGenericType && type.GetGenericTypeDefinition() == typeof(List<>) ? type.GetGenericArguments()[0] : null;
    internal static ConstructorInfo? Constructor(Type type)
        => type.GetConstructor(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance, null, Type.EmptyTypes, null);

    internal static bool IsSupported(Type type) => Supported(type, new HashSet<Type>(), 0);
    static bool Supported(Type type, HashSet<Type> visiting, int depth)
    {
        if (depth > StructuredFieldCodec.MAX_DEPTH) return false;
        if (Kind(type) is not ("object" or "list")) return true;
        if (type.IsArray) return type.IsSZArray && Supported(type.GetElementType()!, visiting, depth + 1);
        if (Element(type) is Type element) return Supported(element, visiting, depth + 1);
        if (type == typeof(object)) return false;
        for (Type? declaring = type; declaring is not null && declaring != typeof(object); declaring = declaring.BaseType)
        {
            if (type.IsValueType && declaring == typeof(ValueType)) continue;
            if (declaring == typeof(SequenceEvent)) continue;
            string assembly = declaring.Assembly.GetName().Name ?? "";
            if (declaring.Assembly == typeof(object).Assembly || declaring.Assembly == typeof(Script).Assembly
                || assembly == "netstandard" || assembly.StartsWith("System.", StringComparison.Ordinal)
                || assembly.StartsWith("Microsoft.", StringComparison.Ordinal)) return false;
        }
        if (typeof(IEnumerable).IsAssignableFrom(type) || type.IsAbstract || type.IsPointer || type.IsByRef || type.ContainsGenericParameters
            || !type.IsDefined(typeof(SerializableAttribute), false) || typeof(Component).IsAssignableFrom(type)) return false;
        if (!type.IsValueType && Constructor(type) is null) return false;
        if (!visiting.Add(type)) return true;
        bool valid = Fields(type).All(field => Supported(field.Field.FieldType, visiting, depth + 1));
        visiting.Remove(type); return valid;
    }

    internal static Member[] Fields(Type type) => s_fields.GetValue(type, DiscoverFields);

    static Member[] DiscoverFields(Type type)
    {
        List<Member> result = new(); HashSet<string> keys = new(StringComparer.Ordinal);
        for (Type? declaring = type; declaring is not null && declaring != typeof(object); declaring = declaring.BaseType)
            foreach (FieldInfo field in declaring.GetFields(BindingFlags.DeclaredOnly | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static).OrderBy(field => field.MetadataToken))
            {
                SettingAttribute? setting = field.GetCustomAttribute<SettingAttribute>(false);
                bool saved = !field.IsStatic && !field.IsInitOnly && !field.IsLiteral && !field.IsDefined(typeof(NonSerializedAttribute), false)
                    && (field.IsPublic || setting is not null);
                if (field.IsDefined(typeof(RequiredReferenceAttribute), false) && (!saved || !StructuredFieldCodec.IsReferenceType(field.FieldType)))
                    throw new InvalidDataException($"Nested RequiredReference requires a saved reference field: {field.Name}.");
                if (!saved) continue;
                string key = field.GetCustomAttribute<FieldKeyAttribute>(false)?.Key ?? field.Name;
                string[] previous = field.GetCustomAttributes<PreviousFieldKeyAttribute>(false).Select(value => value.Key).ToArray();
                if (key == "$go" || previous.Contains("$go", StringComparer.Ordinal))
                    throw new InvalidDataException("Nested field key '$go' is reserved for stored GameObject references.");
                if (string.IsNullOrWhiteSpace(key) || key.Contains('\0') || !keys.Add(key)
                    || previous.Any(value => string.IsNullOrWhiteSpace(value) || value.Contains('\0') || !keys.Add(value)))
                    throw new InvalidDataException("Nested field keys must be unique and nonempty.");
                if (field.GetCustomAttribute<ValueRangeAttribute>(false) is ValueRangeAttribute range) ValidateRangeContract(field.FieldType, range);
                result.Add(new(field, key, previous, setting));
            }
        return result.OrderBy(field => field.Key, StringComparer.Ordinal).ToArray();
    }

    internal static void ValidateRangeContract(Type type, ValueRangeAttribute range)
    {
        Type numeric = Element(type) ?? type;
        _ = ManagedPrimitiveCodec.Range(numeric, range);
    }
    internal static bool WithinRange(object? value, ValueRangeAttribute range)
    {
        if (value is IEnumerable collection && value is not string) return collection.Cast<object?>().All(item => WithinRange(item, range));
        try { return value is not null && ManagedPrimitiveCodec.WithinRange(value, range); }
        catch (Exception) { return false; }
    }

    internal static Dictionary<string, object?> Build(Type type) => Build(type, new HashSet<Type>(), 0);
    static Dictionary<string, object?> Build(Type type, HashSet<Type> visiting, int depth)
    {
        Dictionary<string, object?> node = new()
        {
            ["type"] = type.FullName, ["valueKind"] = Kind(type), ["mayBeNull"] = MayBeNull(type),
            ["enumValues"] = ScriptFieldCodec.EnumValues(type), ["flags"] = type.IsEnum && type.IsDefined(typeof(FlagsAttribute), false),
            ["codecVersion"] = StructuredFieldCodec.VERSION
        };
        if (ManagedPrimitiveCodec.IsNumeric(type)) {
            Type numeric = ManagedPrimitiveCodec.NumericType(type);
            node["numericKind"] = ManagedPrimitiveCodec.Kind(numeric); node["wireEncoding"] = ManagedPrimitiveCodec.StringEncoded(numeric) ? "decimal_string" : "json_number";
            if (numeric == typeof(char)) node["unit"] = "utf16_code_unit";
        }
        if (ComponentReferenceCodec.IsReference(type)) {
            node["targetType"] = ComponentReferenceCodec.TargetType(type).FullName; node["defaultValue"] = null;
            node["referenceAddress"] = "ownerGuid+nativeComponentKind|savedScriptId+typeKey";
            node["runtimeIdentity"] = "scene_object_lifetime+component_birth|script_instance_generation_birth";
            Type target = ComponentReferenceCodec.TargetType(type);
            node["nativeKinds"] = Enumerable.Range(0, (int)NativeComponentKind.COUNT).Select(index => (Kind: (uint)index, Type: NativeComponentFactory.GetType((uint)index)))
                .Where(value => value.Type is not null && target.IsAssignableFrom(value.Type)).Select(value => new {
                    kind = value.Kind, type = value.Type!.FullName, nativeName = value.Kind == 0 ? "TransformComponent" : ScriptAuthoringMetadata.NativeName(value.Type!) }).ToArray();
            node["statuses"] = new[] { "empty", "missing", "alive" };
        }
        if (typeof(IAssetReference).IsAssignableFrom(type)) {
            node["assetKind"] = type == typeof(MaterialAssetRef) ? "material" : type == typeof(TextureAssetRef) ? "texture"
                : type == typeof(ModelAssetRef) ? "model" : "dataAsset";
            if (type.IsGenericType) node["targetType"] = type.GetGenericArguments()[0].FullName;
        }
        if (!IsSupported(type)) { node["supported"] = false; node["valueKind"] = "unsupported"; return node; }
        if (depth > StructuredFieldCodec.MAX_DEPTH || !visiting.Add(type)) { node["recursive"] = true; return node; }
        try
        {
            if (Element(type) is Type element) {
                node["collection"] = true; node["element"] = Build(element, visiting, depth + 1);
                node["defaultValue"] = Array.Empty<object>(); return node;
            }
            if (Kind(type) == "object") {
                Dictionary<string, object?>[] fields = Fields(type).Select(field => Field(field, Build(field.Field.FieldType, visiting, depth + 1))).ToArray();
                node["fields"] = fields;
                Dictionary<string, object?> created = new(StringComparer.Ordinal);
                foreach (Dictionary<string, object?> field in fields) {
                    Dictionary<string, object?> child = (Dictionary<string, object?>)field["value"]!;
                    created.Add((string)field["name"]!, child.GetValueOrDefault("defaultValue"));
                }
                node["createValue"] = created;
                node["defaultValue"] = type.IsValueType ? created : null;
            } else if (!ComponentReferenceCodec.IsReference(type)) node["defaultValue"] = ScalarDefault(type);
            return node;
        }
        finally { visiting.Remove(type); }
    }
    internal static Dictionary<string, object?> Field(Member member, Dictionary<string, object?> value)
    {
        ValueRangeAttribute? range = member.Field.GetCustomAttribute<ValueRangeAttribute>(false);
        if (range is not null) RangeDefault(value, range, member.Field.FieldType);
        return new()
        {
            ["name"] = member.Key, ["previousKeys"] = member.Previous, ["label"] = member.Metadata?.Label ?? member.Field.Name,
            ["help"] = member.Metadata?.Help, ["group"] = member.Metadata?.Group, ["unit"] = member.Metadata?.Unit,
            ["visible"] = member.Metadata?.Visible ?? true, ["minimum"] = RangeBound(member.Field.FieldType, range, false), ["maximum"] = RangeBound(member.Field.FieldType, range, true),
            ["requiredReference"] = member.Field.IsDefined(typeof(RequiredReferenceAttribute), false), ["requiredAt"] = "Runtime", ["value"] = value
        };
    }
    internal static void RangeDefault(Dictionary<string, object?> node, ValueRangeAttribute range, Type? declaredType = null)
    {
        if (node.TryGetValue("element", out object? element) && element is Dictionary<string, object?> child) { RangeDefault(child, range, declaredType is null ? null : Element(declaredType)); return; }
        Type type = declaredType ?? Type.GetType((string)node["type"]!) ?? throw new InvalidDataException("Primitive range schema type is unavailable.");
        node["defaultValue"] = ManagedPrimitiveCodec.WireValue(ManagedPrimitiveCodec.Default(type, range), type);
        node["minimum"] = RangeBound(type, range, false); node["maximum"] = RangeBound(type, range, true);
    }
    internal static object? RangeBound(Type type, ValueRangeAttribute? range, bool maximum)
    {
        if (range is null) return null; type = Element(type) ?? type;
        var bounds = ManagedPrimitiveCodec.Range(type, range);
        return ManagedPrimitiveCodec.WireValue(maximum ? bounds.Maximum : bounds.Minimum, ManagedPrimitiveCodec.NumericType(type));
    }
    static object? ScalarDefault(Type type)
    {
        if (type == typeof(string) || typeof(IAssetReference).IsAssignableFrom(type)) return "";
        if (type == typeof(GameObject)) return null;
        if (type == typeof(Quaternion)) return new float[] { 0, 0, 0, 1 };
        if (type == typeof(Vector2)) return new float[] { 0, 0 };
        if (type == typeof(Vector3)) return new float[] { 0, 0, 0 };
        if (type == typeof(Vector4)) return new float[] { 0, 0, 0, 0 };
        if (ManagedPrimitiveCodec.IsNumeric(type)) return ManagedPrimitiveCodec.WireValue(ManagedPrimitiveCodec.Default(type), type);
        if (type == typeof(bool)) return false;
        return null;
    }
}
