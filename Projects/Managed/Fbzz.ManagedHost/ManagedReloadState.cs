/// @file    ManagedReloadState.cs
/// @brief   世代をまたぐ値の snapshot と、所有者 ID に基づく非所有 cache 再接続。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text.Json;
using Fbzz;

namespace Fbzz.ManagedHost;

public sealed partial class ManagedScriptRuntime
{
    /// @note user callback / property を呼ばず、設定・KeepOnReload と直接の GO / Component cache だけを値へ変換する。
    public ManagedRuntimeResult CaptureReload(ulong generation, ulong instanceId, out string snapshot, bool includeRuntimeCaches = true)
    {
        snapshot = "";
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (!TryInstance(generation, instanceId, out ScriptInstance? record)) return new(BridgeStatus.STALE_GENERATION);
        try
        {
            object[] fields = record.Type.Fields.Where((ScriptFieldDescriptor field) => field.IsSetting || field.IsReloadState)
                .Select((ScriptFieldDescriptor field) => (object)new { key = field.Key, kind = (uint)field.Kind,
                    value = JsonSerializer.Deserialize<JsonElement>(StructuredFieldCodec.EncodeValue(field.Read(record.Instance), field.ValueType)),
                    references = SettingsReferenceSnapshot.Capture(field.Read(record.Instance), field.ValueType) }).ToArray();
            HashSet<FieldInfo> saved = record.Type.Fields.Where((ScriptFieldDescriptor field) => field.IsSetting || field.IsReloadState)
                .Select((ScriptFieldDescriptor field) => field.ReflectionField).ToHashSet();
            object[] caches = (includeRuntimeCaches ? CacheFields(record.Type.Type) : Enumerable.Empty<FieldInfo>()).Where((FieldInfo field) => !saved.Contains(field))
                .Select((FieldInfo field) => (object)new { field = field.Name, declaring = field.DeclaringType!.FullName,
                    value = CaptureCache(field.GetValue(record.Instance), field.FieldType) }).ToArray();
            snapshot = JsonSerializer.Serialize(new { version = 2, faulted = record.Faulted, fields, caches });

            return new(BridgeStatus.OK);
        }
        catch (Exception exception) { return new(BridgeStatus.LOAD_FAILED, Describe(exception)); }
    }

    /// @note 全候補個体を作成・BindOwner 後に呼ぶ。復元値を全件準備してから代入し、旧世代には書かない。
    public ManagedRuntimeResult RestoreReload(ulong generation, ulong instanceId, string snapshot)
    {
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (!TryInstance(generation, instanceId, out ScriptInstance? record)) return new(BridgeStatus.STALE_GENERATION);
        try
        {

            using JsonDocument document = JsonDocument.Parse(snapshot, new JsonDocumentOptions { MaxDepth = StructuredFieldCodec.MAX_DEPTH + 8 });
            JsonElement root = document.RootElement;
            if (root.GetProperty("version").GetInt32() != 2) throw new InvalidDataException("Unsupported reload snapshot version.");
            Dictionary<ScriptFieldDescriptor, object?> writes = new();
            foreach (JsonElement saved in root.GetProperty("fields").EnumerateArray())
            {
                string key = saved.GetProperty("key").GetString()!;
                ScriptFieldDescriptor? field = record.Type.Fields.FirstOrDefault((ScriptFieldDescriptor value)
                    => (value.IsSetting || value.IsReloadState) && (value.Key == key || value.PreviousKeys.Contains(key)));
                if (field is null) continue;
                if ((uint)field.Kind != saved.GetProperty("kind").GetUInt32() || writes.ContainsKey(field))
                    throw new InvalidDataException($"Reload field shape changed: {record.Type.TypeName}.{key}.");
                object? value = SettingsReferenceSnapshot.Restore(saved.GetProperty("value"), field.ValueType, saved.GetProperty("references"));
                value = RebindReloadReferences(record, value, field.ValueType);
                if (!field.Validate(value)) throw new InvalidDataException($"Reload value is invalid: {key}.");
                writes.Add(field, value);
            }
            List<(FieldInfo Field, object? Value)> caches = new();
            FieldInfo[] available = CacheFields(record.Type.Type).ToArray();
            foreach (JsonElement cached in root.GetProperty("caches").EnumerateArray())
            {
                string name = cached.GetProperty("field").GetString()!;
                string declaring = cached.GetProperty("declaring").GetString()!;
                FieldInfo[] matches = available.Where((FieldInfo field) => field.Name == name).ToArray();
                FieldInfo? field = matches.FirstOrDefault((FieldInfo field) => field.DeclaringType!.FullName == declaring);
                if (field is null && matches.Length == 1) field = matches[0];
                if (field is null) { if (matches.Length > 1) throw new InvalidDataException($"Ambiguous reload cache: {name}."); continue; }
                caches.Add((field, RestoreCache(cached.GetProperty("value"), field.FieldType, record)));
            }
            foreach (ScriptFieldDescriptor field in record.Type.Fields.Where((ScriptFieldDescriptor field) => field.IsSetting || field.IsReloadState))
                if (!field.Validate(writes.TryGetValue(field, out object? value) ? value : field.Read(record.Instance)))
                    throw new InvalidDataException($"Candidate default or restored state is invalid: {field.Key}.");
            foreach (KeyValuePair<ScriptFieldDescriptor, object?> write in writes) write.Key.AssignPrepared(record.Instance, write.Value);
            foreach ((FieldInfo field, object? value) in caches) field.SetValue(record.Instance, value);
            record.SettingsRevision++;
            record.Faulted = root.GetProperty("faulted").GetBoolean();
            return new(BridgeStatus.OK);
        }
        catch (Exception exception) { return new(BridgeStatus.LOAD_FAILED, Describe(exception)); }
    }

    static IEnumerable<FieldInfo> CacheFields(Type type)
    {
        for (Type? declaring = type; declaring is not null && declaring != typeof(Script) && declaring != typeof(Component); declaring = declaring.BaseType)
            foreach (FieldInfo field in declaring.GetFields(BindingFlags.DeclaredOnly | BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
            {
                Type value = CacheElementType(field.FieldType) ?? field.FieldType;
                if (!field.IsInitOnly && (value == typeof(GameObject) || typeof(Component).IsAssignableFrom(value))) yield return field;
            }
    }
    static object? CaptureCache(object? value, Type type)
    {
        if (value is null) return null;
        if (CacheElementType(type) is Type element)
        {
            IList values = (IList)value;

            return values.Cast<object?>().Select((object? item) => CaptureCache(item, element)).ToArray();
        }
        if (value is GameObject target) return new { kind = "go", guid = target.Guid, id = target.ObjectId, lifetime = target.SceneLifetime };
        if (value is Script script)
            return script.Binding is ScriptBinding binding && binding.Active ? new { kind = "script", guid = binding.GameObject.Guid,
                id = binding.GameObject.ObjectId, lifetime = binding.GameObject.SceneLifetime, savedScriptId = binding.SavedScriptId } : null;
        if (value is Component component && component.GetType().Assembly == typeof(Script).Assembly)
        {
            GameObject? owner = null;
            for (Type? declaring = component.GetType(); declaring is not null && declaring != typeof(Component); declaring = declaring.BaseType)
                foreach (FieldInfo field in declaring.GetFields(BindingFlags.DeclaredOnly | BindingFlags.Instance | BindingFlags.NonPublic))
                    if (field.FieldType == typeof(GameObject)) owner = (GameObject?)field.GetValue(component);
            ulong componentLifetime = component switch
            {
                NativeComponent native => native.LifetimeToken,
                RigidBody body => body.LifetimeToken,
                Transform => owner?.ObjectId ?? 0,
                _ => throw new InvalidDataException("Unknown native component cache lifetime.")
            };
            return owner is null ? null : new { kind = "native", guid = owner.Guid, id = owner.ObjectId,
                lifetime = owner.SceneLifetime, componentLifetime, componentKind = NativeComponentFactory.GetKind(component.GetType()) };
        }
        throw new InvalidDataException("Unknown component cache cannot be reconnected.");
    }
    object? RestoreCache(JsonElement json, Type type, ScriptInstance record)
    {
        if (json.ValueKind == JsonValueKind.Null) return null;
        if (CacheElementType(type) is Type element)
        {
            if (json.ValueKind != JsonValueKind.Array) throw new InvalidDataException("Invalid reload cache array.");
            IList values = type.IsSZArray ? Array.CreateInstance(element, json.GetArrayLength())
                : (IList)(Activator.CreateInstance(type) ?? throw new InvalidDataException("Reload cache list construction failed."));
            for (int index = 0; index < json.GetArrayLength(); index++)
            {
                object? value = RestoreCache(json[index], element, record);
                if (type.IsSZArray) values[index] = value; else values.Add(value);
            }
            return values;
        }
        string kind = json.GetProperty("kind").GetString()!;
        if (kind == "script")
        {
            string savedScriptId = json.GetProperty("savedScriptId").GetString()!;
            string guid = json.GetProperty("guid").GetString()!;
            ulong objectId = json.GetProperty("id").GetUInt64();
            ulong lifetime = json.GetProperty("lifetime").GetUInt64();
            ScriptInstance? target = BoundScripts(record.Generation, objectId, lifetime)?.GetValueOrDefault(savedScriptId);
            if (target is not null && (!target.Binding.Active || target.Binding.GameObject.Guid != guid)) target = null;
            return target is not null && type.IsInstanceOfType(target.Instance) ? target.Instance : null;
        }
        /// @note private cache は opaque 寿命を保つ。同 GUID の別実体を再検索して失効 cache を復活させない。
        GameObject reference = new(json.GetProperty("id").GetUInt64(), json.GetProperty("lifetime").GetUInt64(), json.GetProperty("guid").GetString()!);
        if (kind == "go" && type == typeof(GameObject)) return reference;
        if (kind != "native" || type.Assembly != typeof(Script).Assembly) throw new InvalidDataException("Invalid reload component cache type.");
        Type facade = NativeComponentFactory.GetType(json.GetProperty("componentKind").GetUInt32())
            ?? throw new InvalidDataException("Invalid reload native component kind.");
        Component? component = NativeComponentFactory.Create(facade, reference, json.GetProperty("componentLifetime").GetUInt64());
        if (component is not null && !type.IsInstanceOfType(component)) throw new InvalidDataException("Reload native component cache type changed.");
        return component;
    }
    static Type? CacheElementType(Type type) => type.IsSZArray ? type.GetElementType()
        : type.IsGenericType && type.GetGenericTypeDefinition() == typeof(List<>) ? type.GetGenericArguments()[0] : null;
}
