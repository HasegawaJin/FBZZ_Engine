/// @file    ComponentReferenceCodec.cs
/// @brief   型付き参照の保存アドレスと runtime token の検証・値復元を共有する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text.Json;
using Fbzz;

namespace Fbzz.ManagedHost;

internal static class ComponentReferenceCodec
{
    internal static bool IsReference(Type type) => type.IsGenericType && type.GetGenericTypeDefinition() == typeof(ComponentRef<>);
    internal static Type TargetType(Type type) => type.GetGenericArguments()[0];
    internal static object Create(Type type, ComponentReferenceIdentity? identity)
    {
        if (!IsReference(type)) throw new InvalidDataException("Expected a ComponentRef<T> declared type.");
        if (identity is not null) Validate(identity, TargetType(type));
        ConstructorInfo constructor = type.GetConstructor(BindingFlags.Instance | BindingFlags.NonPublic, null, [typeof(ComponentReferenceIdentity)], null)!;
        return constructor.Invoke([identity]);
    }
    internal static void Validate(ComponentReferenceIdentity identity, Type targetType)
    {
        ScriptFieldCodec.ValidateGuid(identity.OwnerGuid);
        if (identity.Kind == "native") {
            Type facade = NativeComponentFactory.GetType(identity.ComponentKind) ?? throw new InvalidDataException("Unknown native Component reference kind.");
            if (!targetType.IsAssignableFrom(facade) || identity.SavedScriptId.Length != 0 || identity.TypeKey.Length != 0)
                throw new InvalidDataException("Native Component reference does not match its declared type.");
        } else if (identity.Kind == "script") {
            if (!typeof(Script).IsAssignableFrom(targetType) && targetType != typeof(Component)
                || identity.ComponentKind != uint.MaxValue || !Guid.TryParseExact(identity.SavedScriptId, "D", out Guid id) || id == Guid.Empty
                || string.IsNullOrWhiteSpace(identity.TypeKey) || identity.TypeKey.Contains('\0'))
                throw new InvalidDataException("Script reference requires a saved Script UUID and persistent type key.");
        } else throw new InvalidDataException("Unknown Component reference kind.");
        if ((identity.ObjectId == 0) != (identity.SceneLifetime == 0)) throw new InvalidDataException("Reference object and Scene runtime tokens must be both present or absent.");
        if (identity.Kind == "script" && identity.ObjectId != 0 && (!Guid.TryParseExact(identity.ScriptBirth, "D", out Guid birth) || birth == Guid.Empty))
            throw new InvalidDataException("Resolved Script reference requires an exact adapter birth token.");
    }
    internal static Dictionary<string, object?> Address(ComponentReferenceIdentity identity) => new() {
        ["kind"] = identity.Kind, ["ownerGuid"] = identity.OwnerGuid,
        ["componentKind"] = identity.ComponentKind, ["savedScriptId"] = identity.SavedScriptId, ["typeKey"] = identity.TypeKey
    };
    internal static ComponentReferenceIdentity ReadAddress(JsonElement json, Type type)
    {
        if (json.ValueKind != JsonValueKind.Object || json.EnumerateObject().Count() != 1 || !json.TryGetProperty("$component", out JsonElement address)
            || address.ValueKind != JsonValueKind.Object || address.EnumerateObject().Count() != 5)
            throw new InvalidDataException("Component reference requires a typed $component address.");
        ComponentReferenceIdentity identity = new(address.GetProperty("kind").GetString() ?? "", address.GetProperty("ownerGuid").GetString() ?? "",
            address.GetProperty("componentKind").GetUInt32(), address.GetProperty("savedScriptId").GetString() ?? "", address.GetProperty("typeKey").GetString() ?? "");
        Validate(identity, TargetType(type));
        return identity;
    }
    internal static bool SameAddress(ComponentReferenceIdentity left, ComponentReferenceIdentity right)
        => left.Kind == right.Kind && left.OwnerGuid == right.OwnerGuid && left.ComponentKind == right.ComponentKind
            && left.SavedScriptId == right.SavedScriptId && left.TypeKey == right.TypeKey;
    internal static Dictionary<string, ComponentReferenceIdentity> Paths(object? value, Type type)
    {
        Dictionary<string, ComponentReferenceIdentity> values = new(StringComparer.Ordinal);
        void Visit(object? current, Type declared, string path, HashSet<object> ancestors) {
            if (current is null) return;
            if (IsReference(declared)) {
                if (((IComponentReference)current).Identity is ComponentReferenceIdentity identity) values.Add(path, identity);
                return;
            }
            if (ManagedFieldSchema.Kind(declared) is not ("object" or "list")) return;
            if (!declared.IsValueType && !ancestors.Add(current)) throw new InvalidDataException("Reference settings must not contain cycles.");
            try {
                if (ManagedFieldSchema.Element(declared) is Type element) {
                    int index = 0;
                    foreach (object? item in (System.Collections.IEnumerable)current)
                        Visit(item, element, StructuredFieldCodec.ChildPath(path, (index++).ToString(CultureInfo.InvariantCulture)), ancestors);
                } else foreach (ManagedFieldSchema.Member member in ManagedFieldSchema.Fields(declared))
                    Visit(member.Field.GetValue(current), member.Field.FieldType, StructuredFieldCodec.ChildPath(path, member.Key), ancestors);
            } finally { if (!declared.IsValueType) ancestors.Remove(current); }
        }
        Visit(value, type, "", new(ReferenceEqualityComparer.Instance));
        return values;
    }
}

public sealed partial class ManagedScriptRuntime : IComponentReferenceLookup
{
    internal bool IsComponentAlivePrepared(ScriptInstance record, ComponentReferenceIdentity identity)
    {
        if (identity.ObjectId == 0 || identity.SceneLifetime != record.Binding.GameObject.SceneLifetime) return false;
        ScriptExecutionScope scope = new(m_bridge, this, record.Generation, record.Binding.InstanceId, record.OwnerId, 0, identity.SceneLifetime, ScriptCallback.ON_VALIDATE);
        if (!m_bridge.IsAlive(scope, identity.ObjectId)) return false;
        if (identity.Kind == "native") return identity.ComponentLifetime != 0
            && m_bridge.ReadComponentLifetime(scope, identity.ObjectId, identity.ComponentKind) == identity.ComponentLifetime;
        return identity.Generation == record.Generation && TryInstance(identity.Generation, identity.ScriptInstanceId, out ScriptInstance? target)
            && target.Binding.Active && target.Binding.ScriptBirth == identity.ScriptBirth && target.Type.PersistentId == identity.TypeKey
            && target.Binding.SavedScriptId == identity.SavedScriptId && target.Binding.GameObject.ObjectId == identity.ObjectId
            && target.Binding.GameObject.SceneLifetime == identity.SceneLifetime && m_bridge.HasScriptEntry(scope, identity.ObjectId, identity.SavedScriptId);
    }
    Component? IComponentReferenceLookup.GetHeldReference(ScriptExecutionScope scope, ComponentReferenceIdentity identity, Type declaredType)
    {
        if (!ScriptApiRuntime.IsSameScope(scope, ScriptApiRuntime.Current) || identity.Generation != scope.Generation
            || !TryInstance(scope.Generation, identity.ScriptInstanceId, out ScriptInstance? record) || !record.Binding.Active
            || record.Binding.SavedScriptId != identity.SavedScriptId || record.Type.PersistentId != identity.TypeKey
            || record.Binding.ScriptBirth != identity.ScriptBirth
            || record.Binding.GameObject.ObjectId != identity.ObjectId || record.Binding.GameObject.SceneLifetime != identity.SceneLifetime
            || record.Binding.GameObject.Guid != identity.OwnerGuid || !declaredType.IsInstanceOfType(record.Instance)
            || !m_bridge.HasScriptEntry(scope, identity.ObjectId, identity.SavedScriptId)) return null;
        return record.Instance;
    }
    internal ComponentReferenceIdentity ResolveComponentReferencePrepared(ScriptInstance record, ComponentReferenceIdentity address, Type wrapperType)
    {
        ComponentReferenceCodec.Validate(address, ComponentReferenceCodec.TargetType(wrapperType));
        ScriptExecutionScope scope = new(m_bridge, this, record.Generation, record.Binding.InstanceId, record.OwnerId, 0,
            record.Binding.GameObject.SceneLifetime, ScriptCallback.ON_VALIDATE);
        ComponentReferenceIdentity resolved = m_bridge.ResolveComponentReference(scope, address);
        if (resolved.Kind == "script" && resolved.ObjectId != 0) {
            ScriptInstance? target = BoundScripts(record.Generation, resolved.ObjectId, resolved.SceneLifetime)?.GetValueOrDefault(address.SavedScriptId);
            if (target is null) return address with { ObjectId = 0, SceneLifetime = 0, ComponentLifetime = 0, ScriptInstanceId = 0, Generation = 0 };
            if (target.Type.PersistentId != address.TypeKey || !ComponentReferenceCodec.TargetType(wrapperType).IsInstanceOfType(target.Instance))
                throw new InvalidDataException("Saved Script reference is incompatible with the declared target type.");
            if (target.Binding.ScriptBirth != resolved.ScriptBirth) throw new InvalidDataException("Saved Script reference belongs to a different adapter lifetime.");
            resolved = resolved with { ScriptInstanceId = target.Binding.InstanceId, Generation = record.Generation };
        }
        return resolved;
    }
    internal object? RebindReloadReferences(ScriptInstance record, object? value, Type declared)
    {
        if (value is null) return null;
        if (ComponentReferenceCodec.IsReference(declared)) {
            ComponentReferenceIdentity? identity = ((IComponentReference)value).Identity;
            if (identity is null || identity.Kind != "script" || identity.ObjectId == 0) return value;
            if (!TryInstance(identity.Generation, identity.ScriptInstanceId, out ScriptInstance? previous) || !previous.Binding.Active
                || previous.Binding.SavedScriptId != identity.SavedScriptId || previous.Binding.GameObject.ObjectId != identity.ObjectId
                || previous.Binding.ScriptBirth != identity.ScriptBirth
                || previous.Binding.GameObject.SceneLifetime != identity.SceneLifetime) return value;
            ScriptInstance? next = BoundScripts(record.Generation, identity.ObjectId, identity.SceneLifetime)?.GetValueOrDefault(identity.SavedScriptId);
            if (next is null || next.Binding.ScriptBirth != identity.ScriptBirth || next.Type.PersistentId != identity.TypeKey
                || !ComponentReferenceCodec.TargetType(declared).IsInstanceOfType(next.Instance)) return value;
            return ComponentReferenceCodec.Create(declared, identity with { ScriptInstanceId = next.Binding.InstanceId, Generation = record.Generation });
        }
        if (ManagedFieldSchema.Element(declared) is Type element && value is System.Collections.IList values) {
            for (int index = 0; index < values.Count; index++) values[index] = RebindReloadReferences(record, values[index], element);
        } else if (ManagedFieldSchema.Kind(declared) == "object")
            foreach (ManagedFieldSchema.Member field in ManagedFieldSchema.Fields(declared)) field.Field.SetValue(value, RebindReloadReferences(record, field.Field.GetValue(value), field.Field.FieldType));
        return value;
    }
    public ManagedRuntimeResult ResolveSavedComponentReferences(ulong generation, ulong instanceId)
    {
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (!TryInstance(generation, instanceId, out ScriptInstance? record)) return new(BridgeStatus.STALE_GENERATION);
        try {
            object? Visit(object? value, Type type) {
                if (value is null) return null;
                if (ComponentReferenceCodec.IsReference(type)) {
                    ComponentReferenceIdentity? identity = ((IComponentReference)value).Identity;
                    return identity is null || identity.ObjectId != 0 ? value : ComponentReferenceCodec.Create(type, ResolveComponentReferencePrepared(record, identity, type));
                }
                if (ManagedFieldSchema.Element(type) is Type element && value is System.Collections.IList list)
                    for (int index = 0; index < list.Count; index++) list[index] = Visit(list[index], element);
                else if (ManagedFieldSchema.Kind(type) == "object")
                    foreach (ManagedFieldSchema.Member field in ManagedFieldSchema.Fields(type)) field.Field.SetValue(value, Visit(field.Field.GetValue(value), field.Field.FieldType));
                return value;
            }
            Dictionary<ScriptFieldDescriptor, object?> prepared = SettingsDocument.Capture(record.Type, record.Instance);
            foreach (ScriptFieldDescriptor field in prepared.Keys.ToArray()) prepared[field] = Visit(prepared[field], field.ValueType);
            SettingsDocument.Validate(prepared, record.Type.TypeName);
            foreach ((ScriptFieldDescriptor field, object? value) in prepared) field.AssignPrepared(record.Instance, value);
            return new(BridgeStatus.OK);
        } catch (Exception exception) { return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }
}
