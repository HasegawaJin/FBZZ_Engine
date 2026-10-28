/// @file    SettingsReferenceImport.cs
/// @brief   Play 採用の参照を保存住所・正確な生存 token・移行先 schema で一括検証する。
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

public sealed partial class ManagedScriptRuntime
{
    internal Dictionary<string, SettingsReferenceSnapshot.Identity> RebindImportedReferenceTable(ScriptInstance destination,
        JsonElement value, Type declaredType, string rootPath, JsonElement sourceRefs, JsonElement mappings)
    {
        if (sourceRefs.ValueKind != JsonValueKind.Object || mappings.ValueKind != JsonValueKind.Object) throw new InvalidDataException("Adoption requires complete reference tables.");
        Dictionary<string, SettingsReferenceSnapshot.Identity> source = new(StringComparer.Ordinal), target = new(StringComparer.Ordinal);
        foreach (JsonProperty property in sourceRefs.EnumerateObject()) {
            if (property.Name != rootPath && !property.Name.StartsWith(rootPath + "/", StringComparison.Ordinal)) throw new InvalidDataException("Adoption reference is outside the selected root.");
            if (!source.TryAdd(property.Name, ReadReferenceIdentity(property.Value))) throw new InvalidDataException("Duplicate captured adoption reference path.");
        }
        foreach (JsonProperty property in mappings.EnumerateObject()) {
            Keys(property.Value, "source", "target");
            SettingsReferenceSnapshot.Identity expected = ReadReferenceIdentity(property.Value.GetProperty("source")), next = ReadReferenceIdentity(property.Value.GetProperty("target"));
            if (!source.TryGetValue(property.Name, out SettingsReferenceSnapshot.Identity? captured) || expected != captured || !target.TryAdd(property.Name, next))
                throw new InvalidDataException("Adoption mapping does not match its captured exact identity.");
            if (Number(captured.objectId) == 0 || Number(captured.sceneLifetime) == 0 || Number(next.objectId) == 0 || Number(next.sceneLifetime) == 0)
                throw new InvalidDataException("Nonempty missing or expired adoption references cannot be imported.");
            if ((captured.kind is null) != (next.kind is null) || captured.guid != next.guid
                || captured.kind is not null && !ComponentReferenceCodec.SameAddress(SettingsReferenceSnapshot.ToComponent(captured), SettingsReferenceSnapshot.ToComponent(next)))
                throw new InvalidDataException("Adoption mapping must preserve the typed saved address.");
        }
        if (source.Count != target.Count) throw new InvalidDataException("Every nonempty adoption reference requires exactly one mapping.");
        JsonElement Relative(Dictionary<string, SettingsReferenceSnapshot.Identity> table)
            => JsonSerializer.SerializeToElement(table.ToDictionary(pair => pair.Key[rootPath.Length..], pair => pair.Value, StringComparer.Ordinal));
        _ = SettingsReferenceSnapshot.Restore(value, declaredType, Relative(source));
        object? restored = SettingsReferenceSnapshot.Restore(value, declaredType, Relative(target));
        foreach (GameObject reference in StructuredFieldCodec.ReferencePaths(restored, declaredType).Values)
            if (!IsExactImportedGameObject(destination, reference)) throw new InvalidDataException("Adoption GameObject destination has expired or changed GUID.");
        foreach ((string _, ComponentReferenceIdentity reference) in ComponentReferenceCodec.Paths(restored, declaredType))
            if (!IsComponentAlivePrepared(destination, reference)) throw new InvalidDataException("Adoption Component destination has expired or changed birth.");
        return target;
    }
    internal static SettingsReferenceSnapshot.Identity ReadReferenceIdentity(JsonElement value)
    {
        Keys(value, "guid", "objectId", "sceneLifetime", "kind", "componentKind", "savedScriptId", "typeKey", "componentLifetime", "scriptInstanceId", "generation", "scriptBirth");
        SettingsReferenceSnapshot.Identity identity = JsonSerializer.Deserialize<SettingsReferenceSnapshot.Identity>(value) ?? throw new InvalidDataException("Missing reference identity.");
        ScriptFieldCodec.ValidateGuid(identity.guid); _ = Number(identity.objectId); _ = Number(identity.sceneLifetime);
        if (identity.kind is null && (identity.componentKind is not null || identity.savedScriptId is not null || identity.typeKey is not null
            || identity.componentLifetime is not null || identity.scriptInstanceId is not null || identity.generation is not null || identity.scriptBirth is not null))
            throw new InvalidDataException("GameObject identity contains Component fields.");
        return identity;
    }
    static ulong Number(string value) => ulong.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out ulong number)
        ? number : throw new InvalidDataException("Reference identities require unsigned decimal tokens.");
    bool IsExactImportedGameObject(ScriptInstance destination, GameObject value)
        => IsAlivePrepared(destination, value) && m_bridge.ResolveGameObject(destination.Generation, destination.Binding.InstanceId, destination.OwnerId, value.Guid) == value.ObjectId;
    ComponentReferenceIdentity ResolveExactImportedComponent(ScriptInstance destination, string path, ComponentReferenceIdentity address, Type wrapper, Dictionary<string, SettingsReferenceSnapshot.Identity> exact)
    {
        if (!exact.TryGetValue(path, out SettingsReferenceSnapshot.Identity? token) || token.kind is null) throw new InvalidDataException("Missing exact imported Component token.");
        ComponentReferenceIdentity identity = SettingsReferenceSnapshot.ToComponent(token);
        ComponentReferenceCodec.Validate(identity, ComponentReferenceCodec.TargetType(wrapper));
        if (!ComponentReferenceCodec.SameAddress(address, identity) || !IsComponentAlivePrepared(destination, identity)
            || ResolveComponentReferencePrepared(destination, address, wrapper) != identity)
            throw new InvalidDataException("Imported Component token no longer matches the live saved address.");
        return identity;
    }
}
