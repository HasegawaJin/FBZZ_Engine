/// @file    SettingsReferenceSnapshot.cs
/// @brief   Field-local exact reference tokens shared by documents, reload and Undo.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;
using Fbzz;

namespace Fbzz.ManagedHost;

internal static class SettingsReferenceSnapshot
{
    internal sealed record Identity(string guid, string objectId, string sceneLifetime,
        string? kind = null, uint? componentKind = null, string? savedScriptId = null, string? typeKey = null,
        string? componentLifetime = null, string? scriptInstanceId = null, string? generation = null, string? scriptBirth = null);
    /// @note Paths are relative to a single field, so renaming its root saved key does not change its reference slots.
    internal static Dictionary<string, Identity> Capture(object? value, Type type)
    {
        Dictionary<string, Identity> result = StructuredFieldCodec.ReferencePaths(value, type).ToDictionary(pair => pair.Key,
            pair => new Identity(pair.Value.Guid, Text(pair.Value.ObjectId), Text(pair.Value.SceneLifetime)), StringComparer.Ordinal);
        foreach ((string path, ComponentReferenceIdentity reference) in ComponentReferenceCodec.Paths(value, type))
            result.Add(path, FromComponent(reference));
        return result;
    }
    internal static Identity FromComponent(ComponentReferenceIdentity value) => new(value.OwnerGuid, Text(value.ObjectId), Text(value.SceneLifetime),
        value.Kind, value.ComponentKind, value.SavedScriptId, value.TypeKey, Text(value.ComponentLifetime), Text(value.ScriptInstanceId), Text(value.Generation), value.ScriptBirth);
    internal static ComponentReferenceIdentity ToComponent(Identity value) => new(value.kind ?? "", value.guid, value.componentKind ?? uint.MaxValue,
        value.savedScriptId ?? "", value.typeKey ?? "", Unsigned(value.objectId), Unsigned(value.sceneLifetime), Unsigned(value.componentLifetime), Unsigned(value.scriptInstanceId), Unsigned(value.generation), value.scriptBirth ?? "");
    static string Text(ulong value) => value.ToString(CultureInfo.InvariantCulture);
    static ulong Unsigned(string? text) => text is null ? 0 : ulong.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out ulong value)
        ? value : throw new InvalidDataException("Reference identity must be an unsigned decimal string.");
    /// @note No GUID resolution, constructor or getter runs; a missing, extra or mismatched token rejects the whole value.
    internal static object? Restore(JsonElement value, Type type, JsonElement references)
    {
        if (references.ValueKind != JsonValueKind.Object) throw new InvalidDataException("Exact reference table must be an object.");
        Dictionary<string, GameObject> tokens = new(StringComparer.Ordinal);
        Dictionary<string, ComponentReferenceIdentity> components = new(StringComparer.Ordinal);
        foreach (JsonProperty property in references.EnumerateObject()) {
            JsonElement identity = property.Value;
            if (identity.ValueKind != JsonValueKind.Object
                || !identity.TryGetProperty("guid", out JsonElement guid) || !identity.TryGetProperty("objectId", out JsonElement id)
                || !identity.TryGetProperty("sceneLifetime", out JsonElement lifetime)) throw new InvalidDataException("Malformed exact reference token.");
            HashSet<string> names = new(StringComparer.Ordinal);
            foreach (JsonProperty member in identity.EnumerateObject())
                if (!names.Add(member.Name) || member.Name is not ("guid" or "objectId" or "sceneLifetime" or "kind" or "componentKind"
                    or "savedScriptId" or "typeKey" or "componentLifetime" or "scriptInstanceId" or "generation" or "scriptBirth"))
                    throw new InvalidDataException("Unknown or duplicate exact reference identity field.");
            Identity token = JsonSerializer.Deserialize<Identity>(identity.GetRawText()) ?? throw new InvalidDataException("Malformed exact reference token.");
            if (token.kind is not null) {
                if (!components.TryAdd(property.Name, ToComponent(token))) throw new InvalidDataException("Duplicate exact Component path.");
                continue;
            }
            if (token.componentKind is not null || token.savedScriptId is not null || token.typeKey is not null || token.componentLifetime is not null
                || token.scriptInstanceId is not null || token.generation is not null || token.scriptBirth is not null) throw new InvalidDataException("GameObject token cannot contain Component identity fields.");
            string text = ScriptFieldCodec.ValidateGuid(guid.GetString() ?? "");
            ulong objectId = Number(id), sceneLifetime = Number(lifetime);
            if (!tokens.TryAdd(property.Name, new GameObject(objectId, sceneLifetime, text))) throw new InvalidDataException("Duplicate exact reference path.");
        }
        HashSet<string> used = new(StringComparer.Ordinal);
        GameObject Resolve(string path, string guid) {
            if (!tokens.TryGetValue(path, out GameObject? token) || token.Guid != guid || !used.Add(path))
                throw new InvalidDataException($"Exact reference token is missing or mismatched at {path}.");
            return token;
        }
        ComponentReferenceIdentity ResolveComponent(string path, ComponentReferenceIdentity address, Type wrapper) {
            if (!components.TryGetValue(path, out ComponentReferenceIdentity? token) || !ComponentReferenceCodec.SameAddress(address, token) || !used.Add(path))
                throw new InvalidDataException($"Exact Component token is missing or mismatched at {path}.");
            ComponentReferenceCodec.Validate(token, ComponentReferenceCodec.TargetType(wrapper));
            return token;
        }
        object? result = StructuredFieldCodec.DecodeValue(value, type, _ => throw new InvalidDataException("Exact references cannot be resolved by GUID."), Resolve, resolveComponent: ResolveComponent);
        if (used.Count != tokens.Count + components.Count) throw new InvalidDataException("Unused exact reference snapshot token.");
        return result;
    }
    static ulong Number(JsonElement value)
        => value.ValueKind == JsonValueKind.String && ulong.TryParse(value.GetString(), NumberStyles.None, CultureInfo.InvariantCulture, out ulong number)
            ? number : throw new InvalidDataException("Reference identity must be an unsigned decimal string.");
}
