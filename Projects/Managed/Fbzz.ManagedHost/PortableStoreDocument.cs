/// @file    PortableStoreDocument.cs
/// @brief   Serializable 値の portable schema・passive snapshot・失敗時の defaults 保護。
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
using System.Collections.Generic;
using System.Collections;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Fbzz;

namespace Fbzz.ManagedHost;
internal static class PortableStoreDocument
{
    internal const int VERSION = 1;
    internal static string TypeIdentity(Type type) => (type.Assembly.GetName().Name ?? "") + ":" + type.FullName;
    internal static string SchemaIdentity(Type type)
        => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(ManagedFieldSchema.Build(type)))));
    internal static void ValidateSchema(Type type)
    {
        if (!StructuredFieldCodec.IsSupported(type)) throw new InvalidDataException($"Unsupported saved value type: {type.FullName}.");
        ValidateType(type, "", new());
    }
    static void ValidateType(Type type, string path, HashSet<Type> visited)
    {
        if (type == typeof(GameObject) || ComponentReferenceCodec.IsReference(type) || typeof(Component).IsAssignableFrom(type))
            throw new InvalidDataException($"CS_STORE_SCENE_REFERENCE {path}: saved values are portable; live Scene references cannot be persisted.");
        if (ManagedFieldSchema.Element(type) is Type element) { ValidateType(element, path + "/*", visited); return; }
        if (ManagedFieldSchema.Kind(type) != "object" || !visited.Add(type)) return;
        try { foreach (ManagedFieldSchema.Member member in ManagedFieldSchema.Fields(type)) ValidateType(member.Field.FieldType, StructuredFieldCodec.ChildPath(path, member.Key), visited); }
        finally { visited.Remove(type); }
    }
    internal static ImmutableValueSnapshot.SnapshotStream Capture(Type type, object? value)
    {
        ValidateSchema(type);
        if (!StructuredFieldCodec.Validate(value, type)) throw new InvalidDataException("Saved value violates its declared schema.");
        ValidateText(value, type);
        ImmutableValueSnapshot.SnapshotStream stream = new(new SnapshotStorageBudget());
        try {
            using ImmutableValueSnapshot snapshot = ImmutableValueSnapshot.Capture(value, type);
            Dictionary<string, object?> references = new(StringComparer.Ordinal);
            using (Utf8JsonWriter writer = new(stream)) {
                writer.WriteStartObject(); writer.WriteNumber("formatVersion", VERSION);
                writer.WriteString("type", TypeIdentity(type)); writer.WriteString("schema", SchemaIdentity(type));
                writer.WritePropertyName("value"); snapshot.WriteCanonicalValue(writer, "", references); writer.WriteEndObject();
            }
            if (references.Count != 0) throw new InvalidDataException("Portable saved values cannot contain runtime identities.");
            stream.Position = 0; return stream;
        } catch { stream.Dispose(); throw; }
    }
    static readonly UTF8Encoding StrictUtf8 = new(false, true);
    static void ValidateText(object? value, Type type)
    {
        if (value is null) return;
        if (value is string text) { ValidateUtf8(text); return; }
        if (value is IAssetReference asset) { ValidateUtf8(asset.Reference); return; }
        if (ManagedFieldSchema.Element(type) is Type element) {
            foreach (object? item in (IList)value) ValidateText(item, element);
        } else if (ManagedFieldSchema.Kind(type) == "object") {
            foreach (ManagedFieldSchema.Member member in ManagedFieldSchema.Fields(type)) ValidateText(member.Field.GetValue(value), member.Field.FieldType);
        }
    }
    static void ValidateUtf8(string text)
    {
        try { _ = StrictUtf8.GetByteCount(text); }
        catch (EncoderFallbackException exception) { throw new InvalidDataException("Saved text violates its declared Unicode schema.", exception); }
    }
    internal static bool TryRead(Type type, object? defaults, string declaredType, string schema, string json, out object? value)
    {
        value = null; ValidateSchema(type);
        if (declaredType != TypeIdentity(type) || schema != SchemaIdentity(type)) return false;
        try {
            using JsonDocument document = JsonDocument.Parse(json, new JsonDocumentOptions { MaxDepth = StructuredFieldCodec.MAX_DEPTH + 4 });
            JsonElement root = document.RootElement;
            HashSet<string> keys = new(StringComparer.Ordinal);
            if (root.ValueKind != JsonValueKind.Object) return false;
            foreach (JsonProperty property in root.EnumerateObject())
                if (!keys.Add(property.Name) || property.Name is not ("formatVersion" or "type" or "schema" or "value")) return false;
            if (keys.Count != 4 || root.GetProperty("formatVersion").GetInt32() != VERSION
                || root.GetProperty("type").GetString() != declaredType || root.GetProperty("schema").GetString() != schema) return false;
            object? candidate = Apply(root.GetProperty("value"), type, defaults);
            if (!StructuredFieldCodec.Validate(candidate, type)) return false;
            value = candidate; return true;
        } catch (Exception exception) when (exception is InvalidDataException or JsonException or ArgumentException or InvalidOperationException or FormatException or OverflowException) { return false; }
    }
    static object? Apply(JsonElement json, Type type, object? defaults)
    {
        if (ManagedFieldSchema.Kind(type) != "object" || json.ValueKind == JsonValueKind.Null)
            return StructuredFieldCodec.DecodeValue(json, type, _ => throw new InvalidDataException("Portable store cannot resolve Scene GUIDs."));
        if (json.ValueKind != JsonValueKind.Object) throw new InvalidDataException("Saved object requires an object value.");
        object candidate = defaults is null ? RuntimeHelpers.GetUninitializedObject(type) : StructuredFieldCodec.CloneValue(defaults, type)!;
        var members = ManagedFieldSchema.Fields(type).ToArray(); HashSet<string> written = new(StringComparer.Ordinal);
        foreach (JsonProperty property in json.EnumerateObject()) {
            ManagedFieldSchema.Member member = members.FirstOrDefault(member => member.Key == property.Name || member.Previous.Contains(property.Name, StringComparer.Ordinal))
                ?? throw new InvalidDataException($"Unknown saved value field: {property.Name}.");
            if (!written.Add(member.Key)) throw new InvalidDataException($"Duplicate saved value field: {property.Name}.");
            member.Field.SetValue(candidate, Apply(property.Value, member.Field.FieldType, member.Field.GetValue(candidate)));
        }
        return candidate;
    }
}
