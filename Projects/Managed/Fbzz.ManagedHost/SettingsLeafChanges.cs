/// @file    SettingsLeafChanges.cs
/// @brief   Schema-directed leaf patches and exact normalized differences shared by all settings documents.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text.Json;
using Fbzz;

namespace Fbzz.ManagedHost;

internal static class SettingsLeafChanges
{
    internal sealed class Change : IDisposable
    {
        readonly ImmutableValueSnapshot m_before;
        readonly ImmutableValueSnapshot m_after;
        internal Change(ScriptFieldDescriptor field, string[] segments, Type valueType, object? before, object? after)
        {
            Field = field; Segments = segments; ValueType = valueType;
            m_before = ImmutableValueSnapshot.Capture(before, valueType, new(ResidentBytes: 65536));
            try { m_after = ImmutableValueSnapshot.Capture(after, valueType, new(ResidentBytes: 65536)); }
            catch { m_before.Dispose(); throw; }
        }
        internal ScriptFieldDescriptor Field { get; }
        internal string[] Segments { get; }
        internal Type ValueType { get; }
        internal object? Before => m_before.RestoreValue(ValueType);
        internal object? After => m_after.RestoreValue(ValueType);
        internal long ResidentBytes => m_before.ResidentBytes + m_after.ResidentBytes;
        internal long StorageBytes => m_before.StorageBytes + m_after.StorageBytes;
        internal string Path => Segments.Aggregate(StructuredFieldCodec.ChildPath("", Field.Key), StructuredFieldCodec.ChildPath);
        internal string BeforeHash => m_before.Revision;
        internal string AfterHash => m_after.Revision;
        internal Dictionary<string, object?> ReadReview(bool after, string path, long start, int count, int wireBytes)
            => (after ? m_after : m_before).ReadPage(path, start, count, wireBytes);
        internal Dictionary<string, object?> ReadReviewValue(bool after, int wireBytes)
            => (after ? m_after : m_before).ReadValue("", wireBytes);
        public void Dispose() { m_before.Dispose(); m_after.Dispose(); }
    }
    internal sealed record Location(ScriptFieldDescriptor Field, string[] Segments, Type ValueType, bool Append = false, ValueRangeAttribute? Range = null)
    {
        internal string Path => Segments.Aggregate(StructuredFieldCodec.ChildPath("", Field.Key), StructuredFieldCodec.ChildPath);
    }
    internal static Change[] Diff(Dictionary<ScriptFieldDescriptor, object?> before, Dictionary<ScriptFieldDescriptor, object?> after, bool initial, IReadOnlySet<string>? structural = null)
    {
        List<Change> result = new();
        int nodes = 0;
        try {
            foreach ((ScriptFieldDescriptor field, object? value) in before) Visit(field, [], field.ValueType, value, after[field], initial, result, 0, ref nodes, structural);
            return result.ToArray();
        } catch { foreach (Change change in result) change.Dispose(); throw; }
    }
    static void Visit(ScriptFieldDescriptor field, string[] path, Type type, object? before, object? after, bool whole,
        List<Change> changes, int depth, ref int nodes, IReadOnlySet<string>? structural)
    {
        if (depth > StructuredFieldCodec.MAX_DEPTH) throw new InvalidDataException("Settings diff nesting exceeds its supported schema depth.");
        string kind = ManagedFieldSchema.Kind(type);
        string currentPath = path.Aggregate(StructuredFieldCodec.ChildPath("", field.Key), StructuredFieldCodec.ChildPath);
        whole |= structural?.Contains(currentPath) == true;
        if (!whole && before is not null && after is not null) {
            if (ManagedFieldSchema.Element(type) is Type element && before is IList oldItems && after is IList newItems && oldItems.Count == newItems.Count) {
                for (int index = 0; index < oldItems.Count; index++) Visit(field, [.. path, index.ToString(CultureInfo.InvariantCulture)], element, oldItems[index], newItems[index], false, changes, depth + 1, ref nodes, structural);
                return;
            }
            if (kind == "object") {
                foreach (ManagedFieldSchema.Member member in ManagedFieldSchema.Fields(type))
                    Visit(field, [.. path, member.Key], member.Field.FieldType, member.Field.GetValue(before), member.Field.GetValue(after), false, changes, depth + 1, ref nodes, structural);
                return;
            }
            if (MathCount(type) is int count && count != 0) {
                for (int index = 0; index < count; index++) Visit(field, [.. path, index.ToString(CultureInfo.InvariantCulture)], typeof(float), MathGet(before, index), MathGet(after, index), false, changes, depth + 1, ref nodes, structural);
                return;
            }
        }
        if (whole || !SameLeaf(before, after, type)) changes.Add(new(field, path, type,
            StructuredFieldCodec.CloneValue(before, type), StructuredFieldCodec.CloneValue(after, type)));
    }
    static bool SameLeaf(object? before, object? after, Type type)
    {
        if (before is null || after is null) return before is null && after is null;
        if (before is GameObject left && after is GameObject right) return left.ObjectId == right.ObjectId && left.SceneLifetime == right.SceneLifetime && left.Guid == right.Guid;
        if (before is IDataAssetReference asset && after is IDataAssetReference next) return asset.Reference == next.Reference;
        if (ManagedFieldSchema.Kind(type) is "object" or "list") return false;
        return before.Equals(after);
    }
    internal static string Hash(object? value, Type type) { using ImmutableValueSnapshot snapshot = ImmutableValueSnapshot.Capture(value, type); return snapshot.Revision; }
    internal static Location Locate(ManagedTypeDescriptor descriptor, object instance, string path, bool create = false)
    {
        string[] segments = Parse(path);
        ScriptFieldDescriptor? field = descriptor.Fields.FirstOrDefault(field => field.IsSetting && (field.Key == segments[0] || field.PreviousKeys.Contains(segments[0])));
        if (field is null) throw new InvalidDataException($"Unknown setting path: {path}.");
        object? value = field.Read(instance);
        Type type = field.ValueType;
        ValueRangeAttribute? range = field.Range;
        List<string> canonical = new();
        bool append = false;
        foreach (string segment in segments.Skip(1)) {
            if (value is null) throw new InvalidDataException($"Setting path crosses a null value: {path}.");
            if (ManagedFieldSchema.Element(type) is Type element) {
                if (create && segment == "-" && canonical.Count == segments.Length - 2) {
                    canonical.Add(((IList)value).Count.ToString(CultureInfo.InvariantCulture)); type = element; value = null; append = true; continue;
                }
                int index = Index(segment, ((IList)value).Count);
                value = ((IList)value)[index]; type = element; canonical.Add(index.ToString(CultureInfo.InvariantCulture));
            } else if (MathCount(type) != 0) {
                int index = Index(segment, MathCount(type)); value = MathGet(value, index); type = typeof(float); range = null; canonical.Add(index.ToString(CultureInfo.InvariantCulture));
            } else if (ManagedFieldSchema.Kind(type) == "object") {
                ManagedFieldSchema.Member member = Member(type, segment);
                range = member.Field.GetCustomAttribute<ValueRangeAttribute>(false);
                value = member.Field.GetValue(value); type = member.Field.FieldType; canonical.Add(member.Key);
            } else throw new InvalidDataException($"Setting path crosses a scalar value: {path}.");
        }
        return new(field, canonical.ToArray(), type, append, range);
    }
    internal static object? Get(object? value, Type type, IReadOnlyList<string> segments)
    {
        foreach (string segment in segments) {
            if (value is null) throw new InvalidDataException("SETTINGS_CONFLICT: leaf parent became null.");
            if (ManagedFieldSchema.Element(type) is Type element) { value = ((IList)value)[Index(segment, ((IList)value).Count)]; type = element; }
            else if (MathCount(type) != 0) { value = MathGet(value, Index(segment, MathCount(type))); type = typeof(float); }
            else if (ManagedFieldSchema.Kind(type) == "object") { ManagedFieldSchema.Member member = Member(type, segment); value = member.Field.GetValue(value); type = member.Field.FieldType; }
            else throw new InvalidDataException("SETTINGS_CONFLICT: leaf parent shape changed.");
        }
        return value;
    }
    /// @note Caller owns a detached settings graph; boxed structs are returned to their parent after each leaf assignment.
    internal static object? Set(object? value, Type type, IReadOnlyList<string> segments, object? next, int depth = 0, bool append = false)
    {
        if (depth == segments.Count) return next;
        if (value is null) throw new InvalidDataException("SETTINGS_CONFLICT: leaf parent became null.");
        string segment = segments[depth];
        if (ManagedFieldSchema.Element(type) is Type element) {
            IList items = (IList)value;
            if (append && depth == segments.Count - 1 && segment == items.Count.ToString(CultureInfo.InvariantCulture)) {
                if (value is Array array) {
                    Array result = Array.CreateInstance(element, items.Count + 1); Array.Copy(array, result, items.Count); result.SetValue(next, items.Count); return result;
                }
                items.Add(next); return value;
            }
            int index = Index(segment, items.Count);
            items[index] = Set(items[index], element, segments, next, depth + 1, append); return value;
        }
        if (MathCount(type) != 0) {
            int index = Index(segment, MathCount(type));
            return MathSet(value, index, (float)Set(MathGet(value, index), typeof(float), segments, next, depth + 1, append)!);
        }
        if (ManagedFieldSchema.Kind(type) != "object") throw new InvalidDataException("SETTINGS_CONFLICT: leaf parent shape changed.");
        ManagedFieldSchema.Member member = Member(type, segment);
        member.Field.SetValue(value, Set(member.Field.GetValue(value), member.Field.FieldType, segments, next, depth + 1, append));
        return value;
    }
    internal static Dictionary<ScriptFieldDescriptor, object?> Merge(Dictionary<ScriptFieldDescriptor, object?> live, Change[] changes, bool restore, bool check)
    {
        Dictionary<ScriptFieldDescriptor, object?> writes = new();
        foreach (Change change in changes) {
            object? current = Get(live[change.Field], change.Field.ValueType, change.Segments);
            if (check && Hash(current, change.ValueType) != (restore ? change.AfterHash : change.BeforeHash))
                throw new InvalidDataException($"SETTINGS_CONFLICT: {change.Path} changed after publication.");
            if (!writes.TryGetValue(change.Field, out object? value)) value = StructuredFieldCodec.CloneValue(live[change.Field], change.Field.ValueType);
            writes[change.Field] = Set(value, change.Field.ValueType, change.Segments,
                StructuredFieldCodec.CloneValue(restore ? change.Before : change.After, change.ValueType));
        }
        return writes;
    }
    internal static object? CreateValue(Type type, ValueRangeAttribute? range = null)
    {
        ScriptExecutionScope? previous = ScriptApiRuntime.Current;
        ScriptApiRuntime.Current = null;
        try {
            if (ManagedFieldSchema.Kind(type) == "object") return type.IsValueType ? Activator.CreateInstance(type) : ManagedFieldSchema.Constructor(type)!.Invoke(null);
            Dictionary<string, object?> schema = ManagedFieldSchema.Build(type);
            if (range is not null) ManagedFieldSchema.RangeDefault(schema, range, type);
            return StructuredFieldCodec.DecodeValue(JsonSerializer.SerializeToElement(schema.GetValueOrDefault("defaultValue")), type,
                _ => throw new InvalidDataException("Explicit value creation cannot resolve scene references."), validate: false);
        } finally { ScriptApiRuntime.Current = previous; }
    }
    internal static string[] Parse(string path)
    {
        if (!path.StartsWith("/", StringComparison.Ordinal) || path.Length > SettingsProtocolLimits.MAX_PATH_CHARACTERS) throw new InvalidDataException("Settings paths require RFC 6901 absolute paths.");
        for (int index = 0; index < path.Length; index++) if (path[index] == '~' && (++index >= path.Length || path[index] is not ('0' or '1'))) throw new InvalidDataException("Invalid RFC 6901 escape.");
        string[] parts = path[1..].Split('/').Select(segment => segment.Replace("~1", "/", StringComparison.Ordinal).Replace("~0", "~", StringComparison.Ordinal)).ToArray();
        if (parts.Length > StructuredFieldCodec.MAX_DEPTH + 1) throw new InvalidDataException("Settings path exceeds its depth budget.");
        return parts;
    }
    static int Index(string text, int count)
        => int.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out int index) && index >= 0 && index < count && index.ToString(CultureInfo.InvariantCulture) == text
            ? index : throw new InvalidDataException("SETTINGS_CONFLICT: collection slot is missing or invalid.");
    static ManagedFieldSchema.Member Member(Type type, string key)
        => ManagedFieldSchema.Fields(type).FirstOrDefault(member => member.Key == key || member.Previous.Contains(key)) ?? throw new InvalidDataException("Setting member is not present in the schema.");
    static int MathCount(Type type) => type == typeof(Vector2) ? 2 : type == typeof(Vector3) ? 3 : type == typeof(Vector4) || type == typeof(Quaternion) ? 4 : 0;
    static float MathGet(object value, int index) => value switch {
        Vector2 vector => index == 0 ? vector.X : vector.Y,
        Vector3 vector => index == 0 ? vector.X : index == 1 ? vector.Y : vector.Z,
        Vector4 vector => index == 0 ? vector.X : index == 1 ? vector.Y : index == 2 ? vector.Z : vector.W,
        Quaternion vector => index == 0 ? vector.X : index == 1 ? vector.Y : index == 2 ? vector.Z : vector.W,
        _ => throw new InvalidDataException("Math leaf is unsupported.")
    };
    static object MathSet(object value, int index, float next)
    {
        float[] components = Enumerable.Range(0, MathCount(value.GetType())).Select(component => MathGet(value, component)).ToArray(); components[index] = next;
        return value switch {
            Vector2 => new Vector2(components[0], components[1]),
            Vector3 => new Vector3(components[0], components[1], components[2]),
            Vector4 => new Vector4(components[0], components[1], components[2], components[3]),
            Quaternion => new Quaternion(components[0], components[1], components[2], components[3]),
            _ => throw new InvalidDataException("Math leaf is unsupported.")
        };
    }
}
