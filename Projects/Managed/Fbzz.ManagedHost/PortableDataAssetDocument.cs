/// @file    PortableDataAssetDocument.cs
/// @brief   DataAsset の受動的復元と明示的な設定候補作成を共通 schema で処理する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using Fbzz;

namespace Fbzz.ManagedHost;

internal static class PortableDataAssetDocument
{
    internal static void ValidateSchema(DataAssetTypeDescriptor descriptor)
    {
        foreach (ScriptFieldDescriptor field in descriptor.Fields.Where(field => field.IsSetting))
            ValidateType(field.ValueType, StructuredFieldCodec.ChildPath("", field.Key), new());
    }
    static void ValidateType(Type type, string path, HashSet<Type> visited)
    {
        if (type == typeof(GameObject) || ComponentReferenceCodec.IsReference(type) || typeof(Component).IsAssignableFrom(type))
            throw new InvalidDataException($"CS_DATA_ASSET_SCENE_REFERENCE {path}: DataAsset is portable across Scenes; use an asset GUID or ordinary saved data, and keep live Scene references in private runtime caches.");
        if (ManagedFieldSchema.Element(type) is Type element) { ValidateType(element, path + "/*", visited); return; }
        if (ManagedFieldSchema.Kind(type) != "object" || !visited.Add(type)) return;
        try { foreach (ManagedFieldSchema.Member field in ManagedFieldSchema.Fields(type)) ValidateType(field.Field.FieldType, StructuredFieldCodec.ChildPath(path, field.Key), visited); }
        finally { visited.Remove(type); }
    }
    internal static object Read(DataAssetTypeDescriptor descriptor, JsonElement document)
    {
        ValidateSchema(descriptor);
        Keys(document, "formatVersion", "type", "fields");
        if (document.GetProperty("formatVersion").GetInt32() != 2 || document.GetProperty("type").GetString() != descriptor.PersistentId)
            throw new InvalidDataException("DataAsset format or persistent type does not match its current schema.");
        object instance = SettingsDocument.InertValue(descriptor);
        AssignFields(descriptor, instance, document.GetProperty("fields"));
        SettingsDocument.Validate(SettingsDocument.Capture(descriptor, instance), descriptor.TypeName);
        return instance;
    }
    /// @note Constructor/Reset execute only for explicit create. Edit/read/Undo never take this branch; no Engine scope is installed.
    internal static (Dictionary<ScriptFieldDescriptor, object?> Before, Dictionary<ScriptFieldDescriptor, object?> After, SettingsLeafChanges.Change[] Changes)
        Prepare(DataAssetTypeDescriptor descriptor, object? current, JsonElement fields, JsonElement patches)
    {
        if (ScriptApiRuntime.Current is not null) throw new InvalidOperationException("DataAsset authoring requires the idle Host thread without an Engine scope.");
        ValidateSchema(descriptor);
        bool create = current is null;
        object baseline = current ?? SettingsDocument.InertValue(descriptor);
        Dictionary<ScriptFieldDescriptor, object?> before = SettingsDocument.Capture(descriptor, baseline);
        object candidate = create ? descriptor.CreateInstance() : SettingsDocument.Clone(descriptor, baseline);
        if (create) descriptor.AuthoringCallbacks.FirstOrDefault(callback => callback.Callback == ScriptCallback.RESET)?.Invoke(candidate);
        HashSet<string> written = new(StringComparer.Ordinal), structural = new(StringComparer.Ordinal);
        void Unique(string path) {
            if (written.Any(previous => previous == path || previous.StartsWith(path + "/", StringComparison.Ordinal) || path.StartsWith(previous + "/", StringComparison.Ordinal)))
                throw new InvalidDataException($"Duplicate or overlapping DataAsset patch at {path}.");
            written.Add(path);
        }
        AssignFields(descriptor, candidate, fields, Unique);
        if (patches.ValueKind != JsonValueKind.Undefined) {
            if (patches.ValueKind != JsonValueKind.Array) throw new InvalidDataException("DataAsset patches require an array.");
            SettingsPatchValue.ApplyBatch(descriptor, candidate, patches, (value, type, _) => Decode(value, type), Unique, structural);
        }
        descriptor.AuthoringCallbacks.FirstOrDefault(callback => callback.Callback == ScriptCallback.ON_VALIDATE)?.Invoke(candidate);
        Dictionary<ScriptFieldDescriptor, object?> after = SettingsDocument.Capture(descriptor, candidate);
        SettingsDocument.Validate(after, descriptor.TypeName);
        return (before, after, SettingsLeafChanges.Diff(before, after, create, structural));
    }
    internal static void Write(Utf8JsonWriter writer, DataAssetTypeDescriptor descriptor, Dictionary<ScriptFieldDescriptor, object?> fields)
    {
        writer.WriteStartObject(); writer.WriteNumber("formatVersion", 2); writer.WriteString("type", descriptor.PersistentId); writer.WritePropertyName("fields");
        using ImmutableValueSnapshot snapshot = ImmutableValueSnapshot.CaptureFields(fields.Select(pair => (pair.Key.Key, pair.Value, pair.Key.ValueType)));
        Dictionary<string, object?> references = new(StringComparer.Ordinal); snapshot.WriteCanonicalValue(writer, "", references);
        if (references.Count != 0) throw new InvalidDataException("Portable DataAsset values cannot carry Scene runtime identities.");
        writer.WriteEndObject();
    }
    static void AssignFields(DataAssetTypeDescriptor descriptor, object candidate, JsonElement fields, Action<string>? unique = null)
    {
        if (fields.ValueKind != JsonValueKind.Object) throw new InvalidDataException("DataAsset fields require an object.");
        HashSet<string> saved = new(StringComparer.Ordinal);
        foreach (JsonProperty property in fields.EnumerateObject()) {
            ScriptFieldDescriptor field = descriptor.Fields.FirstOrDefault(field => field.IsSetting && (field.Key == property.Name || field.PreviousKeys.Contains(property.Name)))
                ?? throw new InvalidDataException($"Unknown DataAsset field {property.Name}.");
            if (!saved.Add(field.Key)) throw new InvalidDataException($"Duplicate or ambiguous DataAsset field {property.Name}.");
            unique?.Invoke(StructuredFieldCodec.ChildPath("", field.Key));
            field.AssignPrepared(candidate, Decode(property.Value, field.ValueType));
        }
    }
    static object? Decode(JsonElement value, Type type)
        => StructuredFieldCodec.DecodeValue(value, type, _ => throw new InvalidDataException("DataAsset cannot resolve Scene GUIDs."),
            resolveComponent: (_, _, _) => throw new InvalidDataException("DataAsset cannot resolve Scene Component addresses."));
    static void Keys(JsonElement document, params string[] names)
    {
        if (document.ValueKind != JsonValueKind.Object) throw new InvalidDataException("Expected a DataAsset object.");
        HashSet<string> keys = new(StringComparer.Ordinal);
        foreach (JsonProperty property in document.EnumerateObject())
            if (!keys.Add(property.Name) || !names.Contains(property.Name, StringComparer.Ordinal)) throw new InvalidDataException("Unknown or duplicate DataAsset document key.");
    }
}
