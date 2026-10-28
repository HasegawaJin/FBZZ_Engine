/// @file    StructuredFieldCodec.cs
/// @brief   ユーザー property を評価せず、有限な設定値だけを版付き JSON として保存する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;
using System.Text.Json;
using Fbzz;
using Member = Fbzz.ManagedHost.ManagedFieldSchema.Member;

namespace Fbzz.ManagedHost;

/// @note 共有 CLR identity は保存しない。循環を拒否し、共有した非循環オブジェクトは値として複製する。
internal static class StructuredFieldCodec
{
    internal const int VERSION = 1;
    internal const int MAX_DEPTH = 32;

    internal static bool IsSupported(Type type) => ManagedFieldSchema.IsSupported(type);

    internal static bool Validate(object? value, Type type)
    {
        try { using Utf8JsonWriter writer = new(Stream.Null); Write(writer, value, type, new Budget(), 0); return true; }
        catch (Exception) { return false; }
    }

    /// @note Scene references are checked after binding; the save codec also accepts unconnected defaults and snapshots.
    internal static bool ValidateRequired(object? value, Type type, bool required, Func<GameObject, bool> isAlive)
        => ValidateRequired(value, type, required, isAlive, out _);

    internal static bool ValidateRequired(object? value, Type type, bool required, Func<GameObject, bool> isAlive, out string path,
        Func<ComponentReferenceIdentity, bool>? componentAlive = null)
    {
        try { return Required(value, type, required, isAlive, componentAlive, new Budget(), 0, "", out path); }
        catch (Exception) { path = ""; return false; }
    }

    internal static bool IsReferenceType(Type type) => type == typeof(GameObject) || ComponentReferenceCodec.IsReference(type)
        || Element(type) is Type element && (element == typeof(GameObject) || ComponentReferenceCodec.IsReference(element));

    static bool Required(object? value, Type type, bool required, Func<GameObject, bool> isAlive, Func<ComponentReferenceIdentity, bool>? componentAlive,
        Budget budget, int depth, string current, out string path)
    {
        path = current;
        budget.Visit(depth);
        if (required)
        {
            if (type == typeof(GameObject)) return value is GameObject single && isAlive(single);
            if (ComponentReferenceCodec.IsReference(type)) return value is IComponentReference reference && reference.Identity is ComponentReferenceIdentity identity
                && componentAlive is not null && componentAlive(identity);
            if (Element(type) is not Type referenceType || !IsReferenceType(referenceType) || value is not IEnumerable references) return false;
            int count = 0;
            foreach (object? item in references)
            {
                budget.Visit(depth + 1);
                if (!Required(item, referenceType, true, isAlive, componentAlive, budget, depth + 1, current + "[" + count + "]", out path)) return false;
                count++;
            }
            return count != 0;
        }
        if (value is null || ManagedFieldSchema.Kind(type) is not ("object" or "list")) return true;
        Type? element = Element(type);
        if (element is not null && ManagedFieldSchema.Kind(element) is not ("object" or "list")) return true;
        if (!type.IsValueType && !budget.Path.Add(value)) return false;
        try
        {
            if (element is not null)
            {
                int index = 0;
                foreach (object? item in (IEnumerable)value)
                {
                    if (!Required(item, element, false, isAlive, componentAlive, budget, depth + 1, current + "[" + index + "]", out path)) return false;
                    index++;
                }
            }
            else
                foreach (Member member in Fields(type))
                    if (!Required(member.Field.GetValue(value), member.Field.FieldType,
                        member.Field.IsDefined(typeof(RequiredReferenceAttribute), false), isAlive, componentAlive, budget, depth + 1,
                        current + "." + member.Key, out path)) return false;
            return true;
        }
        finally { if (!type.IsValueType) budget.Path.Remove(value); }
    }

    internal static string Encode(object? value, Type type)
    {
        using MemoryStream stream = new();
        using (Utf8JsonWriter writer = new(stream))
        {
            writer.WriteStartObject(); writer.WriteNumber("$version", VERSION); writer.WritePropertyName("value");
            Write(writer, value, type, new Budget(), 0);
            writer.WriteEndObject();
        }
        return Encoding.UTF8.GetString(stream.ToArray());
    }

    /// @note JSON の非 null GO token と同じ順序で参照 identity を返す。GUID の再解決は行わない。
    internal static ulong[] ReferenceIdentities(object? value, Type type, ulong sceneLifetime)
    {
        List<GameObject> references = new();
        _ = EncodeValue(value, type, references);
        return references.Select((GameObject reference) => reference.SceneLifetime == sceneLifetime ? reference.ObjectId : 0UL).ToArray();
    }

    internal static string ChildPath(string parent, string key)
        => parent + "/" + key.Replace("~", "~0", StringComparison.Ordinal).Replace("/", "~1", StringComparison.Ordinal);

    /// @note 編集前の参照 token を保存する。GUID を解決せず、他の値の修正を既存の数値不正で妨げない。
    internal static Dictionary<string, GameObject> ReferencePaths(object? value, Type type)
    {
        Dictionary<string, GameObject> result = new(StringComparer.Ordinal);
        CollectReferencePaths(value, type, "", new Budget(), 0, result);
        return result;
    }

    static void CollectReferencePaths(object? value, Type type, string path, Budget budget, int depth, Dictionary<string, GameObject> result)
    {
        budget.Visit(depth);
        if (value is null) return;
        if (type == typeof(GameObject)) { result.Add(path, (GameObject)value); return; }
        if (ManagedFieldSchema.Kind(type) is not ("object" or "list")) return;
        if (!type.IsValueType && !budget.Path.Add(value)) throw new InvalidDataException("Structured references must not contain a cycle.");
        try {
            Type? element = Element(type);
            if (element is not null) {
                int index = 0;
                foreach (object? item in (IEnumerable)value)
                    CollectReferencePaths(item, element, ChildPath(path, (index++).ToString(System.Globalization.CultureInfo.InvariantCulture)), budget, depth + 1, result);
            } else
                foreach (Member member in Fields(type))
                    CollectReferencePaths(member.Field.GetValue(value), member.Field.FieldType, ChildPath(path, member.Key), budget, depth + 1, result);
        } finally { if (!type.IsValueType) budget.Path.Remove(value); }
    }

    internal static string EncodeValue(object? value, Type type, List<GameObject>? references = null)
    {
        using MemoryStream stream = new();
        using (Utf8JsonWriter writer = new(stream)) Write(writer, value, type, new Budget(), 0, references);
        return Encoding.UTF8.GetString(stream.ToArray());
    }

    internal static object? Decode(string json, Type type, Func<string, GameObject> resolve)
    {
        using JsonDocument document = JsonDocument.Parse(json, new JsonDocumentOptions { MaxDepth = MAX_DEPTH + 4 });
        JsonElement root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object) throw new InvalidDataException("Structured field envelope is missing.");
        HashSet<string> keys = new(StringComparer.Ordinal);
        foreach (JsonProperty property in root.EnumerateObject())
            if (!keys.Add(property.Name) || property.Name is not ("$version" or "value"))
                throw new InvalidDataException("Structured field envelope contains unknown or duplicate keys.");
        if (keys.Count != 2 || !root.GetProperty("$version").TryGetInt32(out int version) || version != VERSION)
            throw new InvalidDataException("Structured field codec version is unsupported.");
        return DecodeValue(root.GetProperty("value"), type, resolve);
    }

    internal static object? DecodeValue(JsonElement json, Type type, Func<string, GameObject> resolve,
        Func<string, string, GameObject>? resolveReference = null, bool validate = true,
        Func<string, ComponentReferenceIdentity, Type, ComponentReferenceIdentity>? resolveComponent = null)
        => Read(json, type, resolve, new Budget(), 0, resolveReference, "", validate, resolveComponent);

    internal static object Schema(Type type) => ManagedFieldSchema.Build(type);
    /// @note Copies saved fields only, without user constructors or private mutable state; immutable reference tokens retain their exact lifetime.
    internal static object? CloneValue(object? value, Type type) => Clone(value, type, new Budget(), 0);
    static object? Clone(object? value, Type type, Budget budget, int depth)
    {
        budget.Visit(depth);
        if (value is null || ManagedFieldSchema.Kind(type) is not ("object" or "list")) return value;
        if (!type.IsValueType && !budget.Path.Add(value)) throw new InvalidDataException("Settings must not contain a cycle.");
        try {
            if (Element(type) is Type element) {
                if (value is Array source) {
                    Array result = Array.CreateInstance(element, source.Length);
                    for (int index = 0; index < source.Length; index++) result.SetValue(Clone(source.GetValue(index), element, budget, depth + 1), index);
                    return result;
                }
                IList list = (IList)Activator.CreateInstance(type)!;
                foreach (object? item in (IEnumerable)value) list.Add(Clone(item, element, budget, depth + 1));
                return list;
            }
            object copy = RuntimeHelpers.GetUninitializedObject(type);
            foreach (Member member in Fields(type)) member.Field.SetValue(copy, Clone(member.Field.GetValue(value), member.Field.FieldType, budget, depth + 1));
            return copy;
        } finally { if (!type.IsValueType) budget.Path.Remove(value); }
    }

    static void Write(Utf8JsonWriter writer, object? value, Type type, Budget budget, int depth, List<GameObject>? references = null)
    {
        budget.Visit(depth);
        if (!IsSupported(type)) throw new InvalidDataException($"Unsupported structured type: {type.FullName}.");
        if (ComponentReferenceCodec.IsReference(type)) {
            ComponentReferenceIdentity? identity = value is IComponentReference reference ? reference.Identity : null;
            if (identity is null) { writer.WriteNullValue(); return; }
            ComponentReferenceCodec.Validate(identity, ComponentReferenceCodec.TargetType(type));
            writer.WriteStartObject(); writer.WritePropertyName("$component"); JsonSerializer.Serialize(writer, ComponentReferenceCodec.Address(identity)); writer.WriteEndObject();
            return;
        }
        if (value is null)
        {
            if (ManagedFieldSchema.MayBeNull(type))
            { writer.WriteNullValue(); return; }
            throw new InvalidDataException("Strings and collections must not be null.");
        }
        if (value.GetType() != type && type != typeof(GameObject)) throw new InvalidDataException("Polymorphic structured values are unsupported.");
        if (ManagedPrimitiveCodec.IsNumeric(type)) { ManagedPrimitiveCodec.Write(writer, value, type); return; }
        if (type == typeof(bool)) { writer.WriteBooleanValue((bool)value); return; }
        if (type == typeof(string)) { budget.Text((string)value); writer.WriteStringValue((string)value); return; }
        if (type == typeof(GameObject))
        { references?.Add((GameObject)value); writer.WriteStartObject(); writer.WriteString("$go", ScriptFieldCodec.ValidateGuid(((GameObject)value).Guid)); writer.WriteEndObject(); return; }
        if (value is IAssetReference asset) { budget.Text(asset.Reference ?? ""); writer.WriteStringValue(asset.Reference ?? ""); return; }
        if (type == typeof(Vector3))
        { Vector3 vector = (Vector3)value; writer.WriteStartArray(); writer.WriteNumberValue(Finite(vector.X)); writer.WriteNumberValue(Finite(vector.Y)); writer.WriteNumberValue(Finite(vector.Z)); writer.WriteEndArray(); return; }
        if (type == typeof(Vector2))
        { Vector2 vector = (Vector2)value; writer.WriteStartArray(); writer.WriteNumberValue(Finite(vector.X)); writer.WriteNumberValue(Finite(vector.Y)); writer.WriteEndArray(); return; }
        if (type == typeof(Vector4))
        { Vector4 vector = (Vector4)value; writer.WriteStartArray(); writer.WriteNumberValue(Finite(vector.X)); writer.WriteNumberValue(Finite(vector.Y)); writer.WriteNumberValue(Finite(vector.Z)); writer.WriteNumberValue(Finite(vector.W)); writer.WriteEndArray(); return; }
        if (type == typeof(Quaternion))
        { Quaternion rotation = (Quaternion)value; writer.WriteStartArray(); writer.WriteNumberValue(Finite(rotation.X)); writer.WriteNumberValue(Finite(rotation.Y)); writer.WriteNumberValue(Finite(rotation.Z)); writer.WriteNumberValue(Finite(rotation.W)); writer.WriteEndArray(); return; }
        if (!type.IsValueType && !budget.Path.Add(value)) throw new InvalidDataException("Structured values must not contain a cycle.");
        try
        {
            Type? element = Element(type);
            if (element is not null)
            {
                writer.WriteStartArray();
                foreach (object? item in (IEnumerable)value) Write(writer, item, element, budget, depth + 1, references);
                writer.WriteEndArray();
            }
            else
            {
                writer.WriteStartObject();
                foreach (Member member in Fields(type))
                {
                    object? fieldValue = member.Field.GetValue(value);
                    ValidateRange(member, fieldValue);
                    writer.WritePropertyName(member.Key); Write(writer, fieldValue, member.Field.FieldType, budget, depth + 1, references);
                }
                writer.WriteEndObject();
            }
        }
        finally { if (!type.IsValueType) budget.Path.Remove(value); }
    }

    static object? Read(JsonElement json, Type type, Func<string, GameObject> resolve, Budget budget, int depth,
        Func<string, string, GameObject>? resolveReference, string path, bool validate = true,
        Func<string, ComponentReferenceIdentity, Type, ComponentReferenceIdentity>? resolveComponent = null)
    {
        budget.Visit(depth);
        if (ComponentReferenceCodec.IsReference(type)) {
            if (json.ValueKind == JsonValueKind.Null) return ComponentReferenceCodec.Create(type, null);
            ComponentReferenceIdentity identity = ComponentReferenceCodec.ReadAddress(json, type);
            if (resolveComponent is not null) identity = resolveComponent(path, identity, type);
            return ComponentReferenceCodec.Create(type, identity);
        }
        if (!IsSupported(type)) throw new InvalidDataException($"Unsupported structured type: {type.FullName}.");
        if (json.ValueKind == JsonValueKind.Null)
        {
            if (ManagedFieldSchema.MayBeNull(type)) return null;
            throw new InvalidDataException("Strings and collections must not be null.");
        }
        if (ManagedPrimitiveCodec.IsNumeric(type)) return ManagedPrimitiveCodec.Read(json, type);
        if (type == typeof(bool)) return json.GetBoolean();
        if (type == typeof(string)) return json.GetString() ?? throw new InvalidDataException("Null string.");
        if (type == typeof(GameObject))
        {
            if (json.ValueKind == JsonValueKind.String) {
                string guid = ScriptFieldCodec.ValidateGuid(json.GetString() ?? "");
                return resolveReference is null ? resolve(guid) : resolveReference(path, guid);
            }
            if (json.ValueKind == JsonValueKind.Object && json.EnumerateObject().Count() == 1 && json.TryGetProperty("$go", out JsonElement reference))
            {
                string guid = ScriptFieldCodec.ValidateGuid(reference.GetString() ?? "");
                return resolveReference is null ? resolve(guid) : resolveReference(path, guid);
            }
            throw new InvalidDataException("A GameObject reference requires a GUID or a typed GO token.");
        }
        if (type == typeof(MaterialAssetRef) || type == typeof(TextureAssetRef) || type == typeof(ModelAssetRef)) {
            string reference = json.GetString() ?? throw new InvalidDataException("Typed asset reference requires a GUID string.");
            if (reference.Length == 0) return Activator.CreateInstance(type);
            return type == typeof(MaterialAssetRef) ? new MaterialAssetRef(reference)
                : type == typeof(TextureAssetRef) ? (object)new TextureAssetRef(reference) : new ModelAssetRef(reference);
        }
        if (type.IsGenericType && type.GetGenericTypeDefinition() == typeof(DataAssetRef<>))
        {
            string reference = json.GetString() ?? throw new InvalidDataException("Asset reference requires a string.");
            if (reference.Contains('\0')) throw new InvalidDataException("Asset reference contains NUL.");
            return reference.Length == 0 ? Activator.CreateInstance(type) : Activator.CreateInstance(type, reference);
        }
        if (type == typeof(Vector2) || type == typeof(Vector3) || type == typeof(Vector4) || type == typeof(Quaternion))
        {
            int count = type == typeof(Vector2) ? 2 : type == typeof(Vector3) ? 3 : 4;
            if (json.ValueKind != JsonValueKind.Array || json.GetArrayLength() != count) throw new InvalidDataException("Invalid math value shape.");
            float x = Finite(json[0].GetSingle()), y = Finite(json[1].GetSingle());
            if (count == 2) return new Vector2(x, y);
            float z = Finite(json[2].GetSingle());
            if (count == 3) return new Vector3(x, y, z);
            float w = Finite(json[3].GetSingle());
            return type == typeof(Vector4) ? (object)new Vector4(x, y, z, w) : new Quaternion(x, y, z, w);
        }
        Type? element = Element(type);
        if (element is not null)
        {
            if (json.ValueKind != JsonValueKind.Array) throw new InvalidDataException("Invalid collection shape.");
            int length = json.GetArrayLength();
            if (type.IsArray)
            { Array array = Array.CreateInstance(element, length); for (int index = 0; index < length; index++) array.SetValue(Read(json[index], element, resolve, budget, depth + 1,
                resolveReference, ChildPath(path, index.ToString(System.Globalization.CultureInfo.InvariantCulture)), validate, resolveComponent), index); return array; }
            IList list = (IList)(Activator.CreateInstance(type) ?? throw new InvalidDataException("Collection construction failed."));
            int itemIndex = 0;
            foreach (JsonElement item in json.EnumerateArray()) list.Add(Read(item, element, resolve, budget, depth + 1,
                resolveReference, ChildPath(path, (itemIndex++).ToString(System.Globalization.CultureInfo.InvariantCulture)), validate, resolveComponent));
            return list;
        }
        if (json.ValueKind != JsonValueKind.Object) throw new InvalidDataException("A serializable value requires an object.");
        Member[] fields = Fields(type);
        Dictionary<Member, object?> prepared = new();
        HashSet<string> keys = new(StringComparer.Ordinal);
        foreach (JsonProperty property in json.EnumerateObject())
        {
            if (!keys.Add(property.Name)) throw new InvalidDataException("Duplicate nested field key.");
            Member? member = fields.FirstOrDefault((Member field) => field.Key == property.Name || field.Previous.Contains(property.Name));
            if (member is null || prepared.ContainsKey(member)) throw new InvalidDataException("Unknown or ambiguous nested field key.");
            object? value = Read(property.Value, member.Field.FieldType, resolve, budget, depth + 1,
                resolveReference, ChildPath(path, member.Key), validate, resolveComponent);
            if (validate) ValidateRange(member, value); prepared.Add(member, value);
        }
        /// @note Decode never evaluates user constructors; missing members use the same inert schema defaults as authoring.
        object result = RuntimeHelpers.GetUninitializedObject(type);
        foreach (Member field in fields) {
            object? value;
            if (!prepared.TryGetValue(field, out value)) {
                Dictionary<string, object?> node = ManagedFieldSchema.Build(field.Field.FieldType);
                if (field.Field.GetCustomAttribute<ValueRangeAttribute>(false) is ValueRangeAttribute range) ManagedFieldSchema.RangeDefault(node, range);
                JsonElement initial = JsonSerializer.SerializeToElement(node.GetValueOrDefault("defaultValue"));
                value = Read(initial, field.Field.FieldType, resolve, budget, depth + 1, resolveReference, ChildPath(path, field.Key), validate, resolveComponent);
            }
            field.Field.SetValue(result, value);
        }
        if (validate) {
            foreach (Member field in fields) ValidateRange(field, field.Field.GetValue(result));
            if (!Validate(result, type)) throw new InvalidDataException("Restored defaults contain invalid nested values.");
        }
        return result;
    }

    static float Finite(float value) => float.IsFinite(value) ? value : throw new InvalidDataException("Stored floats must be finite.");
    static Type? Element(Type type) => ManagedFieldSchema.Element(type);
    static Member[] Fields(Type type) => ManagedFieldSchema.Fields(type);
    static void ValidateRange(Member member, object? value)
    {
        if (member.Field.GetCustomAttribute<ValueRangeAttribute>(false) is not ValueRangeAttribute range) return;
        if (!ManagedFieldSchema.WithinRange(value, range))
            throw new InvalidDataException($"Nested field range rejected {member.Key}.");
    }
    sealed class Budget
    {
        internal readonly HashSet<object> Path = new(ReferenceEqualityComparer.Instance);
        internal void Visit(int depth)
        { if (depth > MAX_DEPTH) throw new InvalidDataException("Structured field nesting exceeds its supported schema depth."); }
        internal void Text(string text)
        {
            _ = new UTF8Encoding(false, true).GetByteCount(text);
        }
    }
}
