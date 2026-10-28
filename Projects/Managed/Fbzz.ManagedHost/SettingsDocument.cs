/// @file    SettingsDocument.cs
/// @brief   Constructor-free settings values shared by Script, DataAsset and prepared transactions.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Fbzz;

namespace Fbzz.ManagedHost;

internal sealed class SettingsDocument
{
    internal SettingsDocument(ManagedTypeDescriptor descriptor, object value, string address)
    { Descriptor = descriptor; Value = value; Address = address; }
    internal ManagedTypeDescriptor Descriptor { get; }
    internal object Value { get; }
    internal string Address { get; }
    internal Dictionary<ScriptFieldDescriptor, object?> Capture() => Capture(Descriptor, Value);
    internal static Dictionary<ScriptFieldDescriptor, object?> Capture(ManagedTypeDescriptor descriptor, object instance)
    {
        Dictionary<ScriptFieldDescriptor, object?> result = new();
        foreach (ScriptFieldDescriptor field in descriptor.Fields.Where(field => field.IsSetting)) {
            object? value = field.Read(instance);
            if (!field.Validate(value)) throw new InvalidDataException($"CS_SETTING_VALUE {descriptor.TypeName}.{field.Key}: invalid setting value.");
            result.Add(field, StructuredFieldCodec.CloneValue(value, field.ValueType));
        }
        return result;
    }
    internal static Dictionary<ScriptFieldDescriptor, object?> Copy(Dictionary<ScriptFieldDescriptor, object?> values)
        => values.ToDictionary(pair => pair.Key, pair => StructuredFieldCodec.CloneValue(pair.Value, pair.Key.ValueType));
    internal static void Assign(object instance, Dictionary<ScriptFieldDescriptor, object?> values)
    { foreach ((ScriptFieldDescriptor field, object? value) in values) field.AssignPrepared(instance, value); }
    /// @note Inert schema defaults are deliberately distinct from user constructor/field initializer defaults.
    internal static object InertValue(ManagedTypeDescriptor descriptor)
    {
        object value = RuntimeHelpers.GetUninitializedObject(descriptor.Type);
        foreach (ScriptFieldDescriptor field in descriptor.Fields.Where(field => field.IsSetting)) {
            Dictionary<string, object?> schema = ManagedFieldSchema.Build(field.ValueType);
            if (field.Range is ValueRangeAttribute range) ManagedFieldSchema.RangeDefault(schema, range, field.ValueType);
            object? initial = StructuredFieldCodec.DecodeValue(JsonSerializer.SerializeToElement(schema.GetValueOrDefault("defaultValue")), field.ValueType,
                _ => throw new InvalidDataException("Schema defaults cannot resolve scene references."));
            field.AssignPrepared(value, initial);
        }
        return value;
    }
    internal static object Clone(ManagedTypeDescriptor descriptor, object current)
    {
        object value = RuntimeHelpers.GetUninitializedObject(descriptor.Type);
        Assign(value, Capture(descriptor, current));
        return value;
    }
    internal static void Validate(Dictionary<ScriptFieldDescriptor, object?> values, string typeName)
    {
        foreach ((ScriptFieldDescriptor field, object? value) in values)
            if (!field.Validate(value) || (field.Kind == ScriptFieldKind.STRING && value is null))
                throw new InvalidDataException($"CS_SETTING_VALUE {typeName}.{field.Key}: normalized settings are invalid.");
    }
    internal static string Json(Dictionary<ScriptFieldDescriptor, object?> values)
        => JsonSerializer.Serialize(values.ToDictionary(pair => pair.Key.Key,
            pair => JsonSerializer.Deserialize<JsonElement>(StructuredFieldCodec.EncodeValue(pair.Value, pair.Key.ValueType)), StringComparer.Ordinal));
    internal static Dictionary<string, SettingsReferenceSnapshot.Identity> References(Dictionary<ScriptFieldDescriptor, object?> values)
    {
        Dictionary<string, SettingsReferenceSnapshot.Identity> result = new(StringComparer.Ordinal);
        foreach ((ScriptFieldDescriptor field, object? value) in values)
            foreach ((string path, SettingsReferenceSnapshot.Identity token) in SettingsReferenceSnapshot.Capture(value, field.ValueType))
                result.Add(StructuredFieldCodec.ChildPath("", field.Key) + path, token);
        return result;
    }
    internal static string Hash(Dictionary<ScriptFieldDescriptor, object?> values)
    { using ImmutableValueSnapshot snapshot = ImmutableValueSnapshot.CaptureFields(values.Select(pair => (pair.Key.Key, pair.Value, pair.Key.ValueType))); return snapshot.Revision; }
    internal static string FieldHash(ScriptFieldDescriptor field, object? value)
        => Hash(new Dictionary<ScriptFieldDescriptor, object?> { [field] = value });
    static IEnumerable<Type> Ancestors(Type type)
    { for (Type? current = type; current is not null; current = current.BaseType) yield return current; }
    internal static string CatalogEntry(ManagedTypeDescriptor descriptor)
    {
        List<Dictionary<string, object?>> fields = new();
        foreach (ScriptFieldDescriptor field in descriptor.Fields.Where(field => field.IsInspectable)) {
            Dictionary<string, object?> schema = JsonSerializer.Deserialize<Dictionary<string, object?>>(ScriptFieldCodec.SchemaJson(field))!;
            schema["declaredType"] = field.ValueType.FullName;
            schema["type"] = ManagedFieldSchema.Kind(field.ValueType);
            schema["default"] = schema.GetValueOrDefault("defaultValue");
            schema["defaultSource"] = "inert_schema";
            schema["setting"] = field.IsSetting;
            schema["readOnly"] = !field.IsSetting;
            fields.Add(schema);
        }
        return JsonSerializer.Serialize(new {
            type = descriptor.PersistentId, displayName = descriptor.TypeName, fields, defaultSource = "inert_schema", observationStatus = "schema_only",
            assignableTypes = Ancestors(descriptor.Type).Select(type => type.FullName).ToArray(),
            requiredComponents = descriptor is ScriptTypeDescriptor script ? script.RequiredComponentTypes.Select(type => type.FullName).ToArray() : Array.Empty<string>(),
            executionOrder = descriptor is ScriptTypeDescriptor ordered ? ordered.ExecutionOrder : 0, optionalComponents = Array.Empty<string>(),
            actions = descriptor is ScriptTypeDescriptor actionable ? actionable.Actions.Select(action => new {
                id = action.Key, label = action.Label, operation = "script.configure.preview", argument = "settings.action", requires = "existing_exact_script"
            }).ToArray() : [],
            gizmoPreview = descriptor is ScriptTypeDescriptor drawable && drawable.Callbacks.Any(callback => callback.Callback is ScriptCallback.DRAW_GIZMOS or ScriptCallback.DRAW_GIZMOS_SELECTED)
                ? new { isolation = "detached_settings", runtimeCaches = false, work = "clone_all_settings_per_callback", cost = "linear_in_total_settings_size" } : null
        });
    }
    /// @note Candidate callbacks have no Engine scope; arbitrary .NET side effects remain prohibited authoring behavior, not sandboxed behavior.
    internal static void Invoke(ManagedTypeDescriptor descriptor, object value, ScriptCallback callback)
    {
        ScriptExecutionScope? previous = ScriptApiRuntime.Current;
        ScriptApiRuntime.Current = null;
        try {
            if (descriptor is ScriptTypeDescriptor script && value is Script candidate)
                script.Callbacks.FirstOrDefault(item => item.Callback == callback)?.CreateInvocation(candidate)?.Invoke();
            else if (descriptor is DataAssetTypeDescriptor asset)
                asset.AuthoringCallbacks.FirstOrDefault(item => item.Callback == callback)?.Invoke(value);
        } catch (Exception exception) {
            throw new InvalidDataException($"CS_AUTHORING_CALLBACK {descriptor.TypeName}.{callback}: {ManagedScriptRuntime.Describe(exception)}", exception);
        } finally { ScriptApiRuntime.Current = previous; }
    }
    /// @note Explicit creation is the only document operation that evaluates constructor initializers and Reset.
    internal static object Create(ManagedTypeDescriptor descriptor)
    {
        ScriptExecutionScope? previous = ScriptApiRuntime.Current;
        ScriptApiRuntime.Current = null;
        try {
            object value = descriptor.CreateValue();
            Invoke(descriptor, value, ScriptCallback.RESET);
            return value;
        } finally { ScriptApiRuntime.Current = previous; }
    }
}
