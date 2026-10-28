/// @file    ManagedObservations.cs
/// @brief   不変の観測 snapshot を exact 個体と明示解放・期限・資源予算で所有する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text;
using Fbzz;

namespace Fbzz.ManagedHost;

public sealed record ObservationPolicy(int HandleCapacity = 32, long ResidentBytes = 1048576,
    long? SpoolBytes = null, long? TotalStorageBytes = null, long LifetimeMilliseconds = 60000);

public sealed partial class ManagedScriptRuntime
{
    sealed record Observation(ulong Generation, ulong Instance, ulong Object, ulong Lifetime, string OwnerGuid,
        string SavedScriptId, string RootPath, long ExpiresAt, ImmutableValueSnapshot Values);
    readonly Dictionary<ulong, Observation> m_observations = new();
    ObservationPolicy m_observationPolicy = new();
    ulong m_nextObservation = 1;
    bool m_observationIdentityExhausted;
    internal Func<long> ObservationClock { get; set; } = () => Environment.TickCount64;
    public int ObservationCount => m_observations.Count;

    /// @note 予算は観測の保持資源だけを制約し、配列や保存値の総件数を制約しない。
    public ManagedRuntimeResult ConfigureObservations(ObservationPolicy policy)
    {
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (policy.HandleCapacity < 1 || policy.ResidentBytes < 1 || policy.SpoolBytes is < 0
            || policy.TotalStorageBytes is < 0 || policy.LifetimeMilliseconds < 1)
            return new(BridgeStatus.INVALID_ARGUMENT, "Observation resource policy must specify positive capacity, resident bytes and lifetime.");
        m_observationPolicy = policy;
        ExpireObservations();
        return new(BridgeStatus.OK);
    }

    /// @note OPEN は出力を持たず token だけを返す変更要求。容量不足による再送を要求しない。
    public ManagedRuntimeResult OpenObservation(ulong generation, ulong instanceId, string path, out ulong observationId)
    {
        observationId = 0;
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (!TryInstance(generation, instanceId, out ScriptInstance? record) || !record.Binding.Active)
            return new(BridgeStatus.STALE_GENERATION);
        ExpireObservations();
        if (m_observationIdentityExhausted) return new(BridgeStatus.PENDING_LIMIT, "Observation identity space is exhausted.");
        if (record.Binding.GameObject.ObjectId == 0 || record.Binding.GameObject.SceneLifetime == 0)
            return new(BridgeStatus.INVALID_SCOPE, "Observation requires an exact bound owner identity.");
        if (m_observations.Count >= m_observationPolicy.HandleCapacity)
            return new(BridgeStatus.PENDING_LIMIT, "Observation handle capacity is exhausted; close snapshots or configure a larger handle capacity.");
        ImmutableValueSnapshot? snapshot = null;
        try {
            SnapshotStorageBudget budget = new(m_observationPolicy.ResidentBytes, m_observationPolicy.SpoolBytes);
            if (path.Length == 0) snapshot = ImmutableValueSnapshot.CaptureFields(record.Type.Fields.Where(field => field.IsInspectable)
                .Select(field => (field.Key, field.Read(record.Instance), field.ValueType)), budget);
            else {
                (object? value, Type type) = ObservationPath(record, path);
                snapshot = ImmutableValueSnapshot.Capture(value, type, budget);
            }
            if (m_observationPolicy.TotalStorageBytes is long capacity
                && snapshot.StorageBytes > capacity - m_observations.Values.Sum(value => value.Values.StorageBytes)) {
                snapshot.Dispose();
                return new(BridgeStatus.PENDING_LIMIT, "Observation storage capacity is exhausted; close snapshots or configure more storage.");
            }
            ulong id = m_nextObservation;
            if (id == ulong.MaxValue) m_observationIdentityExhausted = true;
            else m_nextObservation++;
            GameObject owner = record.Binding.GameObject;
            long now = ObservationClock();
            long expires = ObservationExpiry(now);
            m_observations.Add(id, new(generation, instanceId, owner.ObjectId, owner.SceneLifetime, owner.Guid,
                record.Binding.SavedScriptId, path, expires, snapshot));
            observationId = id;
            return new(BridgeStatus.OK);
        } catch (IOException exception) { snapshot?.Dispose(); return new(BridgeStatus.PENDING_LIMIT, Describe(exception)); }
        catch (Exception exception) { snapshot?.Dispose(); return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }

    /// @note ページ間の field 変更を混ぜない。返す revision は観測内容であり live 設定 revision ではない。
    public ManagedRuntimeResult ReadObservation(ulong generation, ulong instanceId, ulong observationId,
        string path, long start, int count, int wireBudget, out string json)
    {
        json = "";
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        ExpireObservations();
        if (!m_observations.TryGetValue(observationId, out Observation? observation))
            return new(BridgeStatus.NOT_FOUND, "Observation was closed or expired; open a new snapshot.");
        if (observation.Generation != generation || observation.Instance != instanceId)
            return new(BridgeStatus.STALE_GENERATION, "Observation belongs to another Script or generation.");
        if (!TryInstance(generation, instanceId, out ScriptInstance? record) || !record.Binding.Active
            || record.Binding.GameObject.ObjectId != observation.Object || record.Binding.GameObject.SceneLifetime != observation.Lifetime
            || record.Binding.GameObject.Guid != observation.OwnerGuid || record.Binding.SavedScriptId != observation.SavedScriptId) {
            RemoveObservation(observationId);
            return new(BridgeStatus.STALE_GENERATION, "Observation belongs to an expired Script or owner identity.");
        }
        if (start < 0 || count < 1 || wireBudget < 2048)
            return new(BridgeStatus.INVALID_ARGUMENT, "Observation range requires nonnegative start, positive count and at least 2048 wire bytes.");
        try {
            if (path != observation.RootPath && !path.StartsWith(observation.RootPath + "/", StringComparison.Ordinal))
                return new(BridgeStatus.INVALID_ARGUMENT, "The path is outside this observation's selected subtree.");
            string localPath = path[observation.RootPath.Length..];
            for (;;) {
                Dictionary<string, object?> page = observation.Values.ReadPage(localPath, start, count, wireBudget - 384);
                PrefixObservationPaths(page, observation.RootPath);
                page["observationId"] = observationId.ToString(CultureInfo.InvariantCulture);
                page["capturedRevision"] = observation.Values.Revision;
                page["sample"] = "snapshot";
                page["expiresInMilliseconds"] = m_observationPolicy.LifetimeMilliseconds;
                page["storageBytes"] = observation.Values.StorageBytes;
                page["spooled"] = observation.Values.IsSpooled;
                string packet = JsonSerializer.Serialize(page);
                int needed = Encoding.UTF8.GetByteCount(packet);
                if (needed <= wireBudget) {
                    json = packet;
                    m_observations[observationId] = observation with { ExpiresAt = ObservationExpiry(ObservationClock()) };
                    return new(BridgeStatus.OK);
                }
                if (count == 1) return new(BridgeStatus.PENDING_LIMIT, $"Observation packet requires {needed} wire bytes; request a larger wireBytes capacity or descend to a child path.");
                count = Math.Max(1, count / 2);
            }
        } catch (IOException exception) { return new(BridgeStatus.PENDING_LIMIT, Describe(exception)); }
        catch (Exception exception) { return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }

    public ManagedRuntimeResult CloseObservation(ulong generation, ulong instanceId, ulong observationId)
    {
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (!m_observations.TryGetValue(observationId, out Observation? value)) return new(BridgeStatus.OK);
        if (value.Generation != generation || value.Instance != instanceId) return new(BridgeStatus.STALE_GENERATION);
        RemoveObservation(observationId);
        return new(BridgeStatus.OK);
    }
    void RemoveObservation(ulong id) { if (m_observations.Remove(id, out Observation? value)) value.Values.Dispose(); }
    long ObservationExpiry(long now) => m_observationPolicy.LifetimeMilliseconds > long.MaxValue - now
        ? long.MaxValue : now + m_observationPolicy.LifetimeMilliseconds;
    void ExpireObservations()
    {
        long now = ObservationClock();
        foreach (ulong id in m_observations.Where(pair => pair.Value.ExpiresAt <= now).Select(pair => pair.Key).ToArray()) RemoveObservation(id);
    }
    void ReleaseObservations(ulong generation, ulong? instance = null)
    {
        foreach (ulong id in m_observations.Where(pair => pair.Value.Generation == generation
            && (instance is null || pair.Value.Instance == instance)).Select(pair => pair.Key).ToArray()) RemoveObservation(id);
    }
    static (object? Value, Type Type) ObservationPath(ScriptInstance record, string path)
    {
        if (!path.StartsWith("/", StringComparison.Ordinal)) throw new InvalidDataException("Observation path must be an RFC 6901 pointer.");
        string[] parts = path[1..].Split('/').Select(UnescapeObservationPath).ToArray();
        ScriptFieldDescriptor field = record.Type.Fields.SingleOrDefault(value => value.Key == parts[0] && value.IsInspectable)
            ?? throw new InvalidDataException("Observation field path was not found.");
        object? current = field.Read(record.Instance);
        Type type = field.ValueType;
        foreach (string part in parts.Skip(1)) {
            if (current is null) throw new InvalidDataException("Observation path descends through null.");
            if (current is Vector2 or Vector3 or Vector4 or Quaternion) {
                if (!int.TryParse(part, NumberStyles.None, CultureInfo.InvariantCulture, out int component) || part != component.ToString(CultureInfo.InvariantCulture))
                    throw new InvalidDataException("Observation math component index is invalid.");
                current = current switch {
                    Vector2 vector => component switch { 0 => vector.X, 1 => vector.Y, _ => throw new InvalidDataException("Vector2 component index is invalid.") },
                    Vector3 vector => component switch { 0 => vector.X, 1 => vector.Y, 2 => vector.Z, _ => throw new InvalidDataException("Vector3 component index is invalid.") },
                    Vector4 vector => component switch { 0 => vector.X, 1 => vector.Y, 2 => vector.Z, 3 => vector.W, _ => throw new InvalidDataException("Vector4 component index is invalid.") },
                    Quaternion vector => component switch { 0 => vector.X, 1 => vector.Y, 2 => vector.Z, 3 => vector.W, _ => throw new InvalidDataException("Quaternion component index is invalid.") },
                    _ => throw new InvalidDataException("Unknown math component.")
                };
                type = typeof(float);
            } else if (ManagedFieldSchema.Element(type) is Type element) {
                if (!int.TryParse(part, NumberStyles.None, CultureInfo.InvariantCulture, out int index) || index < 0
                    || (part.Length > 1 && part[0] == '0') || current is not IList values || index >= values.Count)
                    throw new InvalidDataException("Observation collection index is invalid.");
                current = values[index]; type = element;
            } else {
                ManagedFieldSchema.Member member = ManagedFieldSchema.Fields(type).SingleOrDefault(value => value.Key == part)
                    ?? throw new InvalidDataException("Observation member path was not found.");
                current = member.Field.GetValue(current); type = member.Field.FieldType;
            }
        }
        return (current, type);
    }
    /// @note 通常の inspect は少量だけ投影し、巨大な subtree は path と件数で示す。snapshot や spool は作成しない。
    internal static string ObservationSummary(ScriptInstance record, ScriptFieldDescriptor field)
    {
        int remaining = 64;
        string path = StructuredFieldCodec.ChildPath("", field.Key);
        object? root = field.Read(record.Instance);
        long Count(object? value, Type type) => value is string text ? text.Length
            : value is IList list ? list.Count : value is null ? 0 : ManagedFieldSchema.Kind(type) == "object" ? ManagedFieldSchema.Fields(type).Length : 1;
        object Deferred(object? value, Type type, string at)
        {
            Dictionary<string, object?> deferred = new() {
                ["$deferred"] = at, ["kind"] = ManagedFieldSchema.Kind(type), ["total"] = Count(value, type), ["snapshot"] = false
            };
            if (value is string) deferred["unit"] = "utf16_code_units";
            return deferred;
        }
        object? Read(object? value, Type type, string at, int depth)
        {
            if (--remaining < 0 || depth > StructuredFieldCodec.MAX_DEPTH) return Deferred(value, type, at);
            if (value is null) {
                if (type == typeof(string)) throw new ObservationValueException(depth == 0 ? "NULL_SCRIPT_SETTING" : "NULL_SCRIPT_ARRAY_ELEMENT", "String value is null.");
                if (ManagedFieldSchema.Element(type) is not null) throw new ObservationValueException("NULL_SCRIPT_ARRAY", "Array is null; empty and null arrays are distinct.");
                return null;
            }
            if (value is GameObject target) return new Dictionary<string, object?> {
                ["$observationGo"] = true, ["guid"] = target.Guid, ["objectId"] = target.ObjectId.ToString(CultureInfo.InvariantCulture),
                ["sceneLifetime"] = target.SceneLifetime.ToString(CultureInfo.InvariantCulture)
            };
            if (value is string text) {
                if (text.Length > 1024) return Deferred(value, type, at);
                _ = new UTF8Encoding(false, true).GetByteCount(text);
                return text;
            }
            if (ManagedFieldSchema.Element(type) is Type element && value is IList list) {
                if (list.Count > remaining) return Deferred(value, type, at);
                object?[] values = new object?[list.Count];
                for (int index = 0; index < list.Count; index++) {
                    try { values[index] = Read(list[index], element, StructuredFieldCodec.ChildPath(at, index.ToString(CultureInfo.InvariantCulture)), depth + 1); }
                    catch (ObservationValueException exception) { exception.Index = index; throw; }
                }
                return values;
            }
            if (ManagedFieldSchema.Kind(type) == "object") {
                Dictionary<string, object?> values = new(StringComparer.Ordinal);
                foreach (ManagedFieldSchema.Member member in ManagedFieldSchema.Fields(type)) {
                    if (remaining <= 0) return Deferred(value, type, at);
                    values.Add(member.Key, Read(member.Field.GetValue(value), member.Field.FieldType, StructuredFieldCodec.ChildPath(at, member.Key), depth + 1));
                }
                return values;
            }
            string encoded;
            try { encoded = StructuredFieldCodec.EncodeValue(value, type); }
            catch (Exception exception) when (ManagedPrimitiveCodec.IsNumeric(type) || type == typeof(Vector3) || type == typeof(Quaternion))
            { throw new ObservationValueException("NON_FINITE_SCRIPT_VALUE", exception.Message); }
            using JsonDocument scalar = JsonDocument.Parse(encoded);
            return scalar.RootElement.Clone();
        }
        try { return JsonSerializer.Serialize(new { value = Read(root, field.ValueType, path, 0), sample = "summary", snapshot = false }); }
        catch (ObservationValueException exception) {
            if (field.Kind == ScriptFieldKind.STRUCTURED) return JsonSerializer.Serialize(new { value = (object?)null,
                diagnostic = new { code = "MANAGED_FIELD_READ_FAILED", message = exception.Message, elementIndex = exception.Index } });
            return JsonSerializer.Serialize(new { value = (object?)null,
                diagnostic = new { code = exception.Code, message = exception.Message, elementIndex = exception.Index } });
        }
    }
    sealed class ObservationValueException(string code, string message) : Exception(message)
    {
        internal string Code { get; } = code;
        internal int Index { get; set; } = -1;
    }
    static string UnescapeObservationPath(string part)
    {
        for (int index = 0; index < part.Length; index++)
            if (part[index] == '~' && (index + 1 >= part.Length || part[++index] is not ('0' or '1')))
                throw new InvalidDataException("Observation path has an invalid RFC 6901 escape.");
        return part.Replace("~1", "/", StringComparison.Ordinal).Replace("~0", "~", StringComparison.Ordinal);
    }
    static void PrefixObservationPaths(Dictionary<string, object?> page, string prefix)
    {
        if (prefix.Length == 0) return;
        void Visit(object? value) {
            if (value is Dictionary<string, object?> node) {
                foreach (string key in node.Keys.ToArray()) {
                    if ((key == "path" || key == "$deferred") && node[key] is string path) node[key] = prefix + path;
                    else if (key == "references" && node[key] is IDictionary<string, object?> references) {
                        node[key] = references.ToDictionary(pair => prefix + pair.Key, pair => pair.Value, StringComparer.Ordinal);
                    } else Visit(node[key]);
                }
            } else if (value is IEnumerable values && value is not string) foreach (object? child in values) Visit(child);
        }
        Visit(page);
    }
}
