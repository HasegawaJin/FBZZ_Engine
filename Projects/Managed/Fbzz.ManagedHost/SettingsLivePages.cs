/// @file    SettingsLivePages.cs
/// @brief   Direct bounded settings pages read only visible values without capturing, hashing or copying their full parent arrays.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using Fbzz;

namespace Fbzz.ManagedHost;

public sealed partial class ManagedScriptRuntime
{
    public ManagedRuntimeResult ReadSettingsPage(ulong generation, ulong instanceId, string request, out string page)
    {
        page = "";
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (!TryInstance(generation, instanceId, out ScriptInstance? record)) return new(BridgeStatus.STALE_GENERATION);
        try {
            using JsonDocument document = JsonDocument.Parse(request); JsonElement input = document.RootElement;
            Keys(input, "path", "start", "count", "packetBytes");
            string path = input.GetProperty("path").GetString() ?? "";
            long start = input.TryGetProperty("start", out JsonElement startValue) ? startValue.GetInt64() : 0;
            int count = input.TryGetProperty("count", out JsonElement countValue) ? countValue.GetInt32() : 64;
            int packetBytes = input.TryGetProperty("packetBytes", out JsonElement capacity) ? capacity.GetInt32() : 65536;
            page = SettingsLivePages.Read(record, path, start, count, packetBytes);
            return new(BridgeStatus.OK);
        } catch (Exception exception) { return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }
}

internal static class SettingsLivePages
{
    internal static string Read(ManagedScriptRuntime.ScriptInstance record, string path, long start, int count, int packetBytes)
        => Read(record.Type, record.Instance, new Dictionary<string, object?> {
            ["generation"] = record.Generation.ToString(CultureInfo.InvariantCulture), ["instanceId"] = record.Binding.InstanceId.ToString(CultureInfo.InvariantCulture),
            ["counter"] = record.SettingsRevision.ToString(CultureInfo.InvariantCulture) }, path, start, count, packetBytes);
    internal static string Read(ManagedTypeDescriptor descriptor, object instance, Dictionary<string, object?> identity, string path, long start, int count, int packetBytes)
    {
        if (packetBytes < 1024 || start < 0 || count < 0) throw new InvalidDataException("Settings page requires nonnegative bounds and at least 1024 packet bytes.");
        Dictionary<string, object?> references = new(StringComparer.Ordinal);
        Dictionary<string, object?> result = new(identity) { ["path"] = path, ["sample"] = "request" };
        int available = packetBytes - 512;
        if (path.Length == 0) {
            ScriptFieldDescriptor[] fields = descriptor.Fields.Where(field => field.IsInspectable).ToArray();
            if (start > fields.Length) throw new InvalidDataException("Settings root page is out of range.");
            List<object?> items = new();
            for (long index = start; index < fields.Length && items.Count < count && available > 256; index++) {
                ScriptFieldDescriptor field = fields[index]; string child = StructuredFieldCodec.ChildPath("", field.Key);
                object? value;
                try { value = Summary(field.Read(instance), field.ValueType, child, ref available, references); }
                catch (Exception exception) { value = new { diagnostic = exception.GetBaseException().Message }; }
                items.Add(new { name = field.Key, setting = field.IsSetting, value });
            }
            result["kind"] = "object"; result["total"] = fields.Length; result["items"] = items;
            Range(result, start, items.Count, fields.Length);
        } else {
            var location = Locate(descriptor, instance, path);
            object? value = location.Value; Type type = location.Type;
            string kind = value is null ? "null" : ManagedFieldSchema.Kind(type);
            result["path"] = location.Path; result["setting"] = location.Field.IsSetting; result["kind"] = kind;
            if (value is string text && (long)text.Length * 6 > available) {
                if (start > text.Length || (start < text.Length && start != 0 && char.IsLowSurrogate(text[(int)start]) && char.IsHighSurrogate(text[(int)start - 1])))
                    throw new InvalidDataException("Settings text start must be a UTF-16 character boundary.");
                int size = (int)Math.Min(Math.Min((long)count, text.Length - start), available / 6);
                if (size != 0 && start + size < text.Length && char.IsHighSurrogate(text[(int)start + size - 1]) && char.IsLowSurrogate(text[(int)start + size])) size--;
                if (size == 0 && start < text.Length && count != 0) throw new InvalidDataException("Text page requires capacity for one complete character.");
                string chunk = text.Substring((int)start, size); _ = new UTF8Encoding(false, true).GetByteCount(chunk);
                result["kind"] = "text"; result["text"] = chunk; result["unit"] = "utf16_code_units"; result["total"] = text.Length; Range(result, start, size, text.Length);
            } else if (value is IList list && ManagedFieldSchema.Element(type) is Type element) {
                if (start > list.Count) throw new InvalidDataException("Settings collection page is out of range.");
                List<object?> items = new();
                for (long index = start; index < list.Count && items.Count < count && available > 256; index++) {
                    string child = StructuredFieldCodec.ChildPath(location.Path, index.ToString(CultureInfo.InvariantCulture));
                    items.Add(Summary(list[(int)index], element, child, ref available, references));
                }
                result["items"] = items; result["total"] = list.Count; Range(result, start, items.Count, list.Count);
            } else if (value is not null && kind == "object") {
                ManagedFieldSchema.Member[] fields = ManagedFieldSchema.Fields(type);
                if (start > fields.Length) throw new InvalidDataException("Settings member page is out of range.");
                List<object?> items = new();
                for (long index = start; index < fields.Length && items.Count < count && available > 256; index++) {
                    var field = fields[index]; string child = StructuredFieldCodec.ChildPath(location.Path, field.Key);
                    items.Add(new { name = field.Key, value = Summary(field.Field.GetValue(value), field.Field.FieldType, child, ref available, references) });
                }
                result["items"] = items; result["total"] = fields.Length; Range(result, start, items.Count, fields.Length);
            } else {
                result["value"] = Summary(value, type, location.Path, ref available, references); result["total"] = 1; Range(result, 0, 1, 1);
            }
        }
        result["references"] = references;
        byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(result);
        if (bytes.Length > packetBytes) throw new InvalidDataException($"Settings packet capacity insufficient: requiredBytes={bytes.Length}, allocatedBytes={packetBytes}. Request a narrower path or fewer items.");
        return Encoding.UTF8.GetString(bytes);
    }
    static void Range(Dictionary<string, object?> result, long start, int returned, long total)
    { result["start"] = start; result["returned"] = returned; result["nextStart"] = start + returned < total ? start + returned : null; }
    static object? Summary(object? value, Type type, string path, ref int bytes, Dictionary<string, object?> references)
    {
        if (value is null) { bytes -= 8; return null; }
        string kind = ManagedFieldSchema.Kind(type);
        if (value is IList list && ManagedFieldSchema.Element(type) is not null) return Deferred(path, "list", list.Count, ref bytes);
        if (kind == "object") return Deferred(path, kind, ManagedFieldSchema.Fields(type).Length, ref bytes);
        if (value is string text && (long)text.Length * 6 > bytes - 64) return Deferred(path, "text", text.Length, ref bytes);
        if (value is GameObject target) {
            bytes -= target.Guid.Length * 12 + path.Length * 6 + 256;
            if (bytes < 0) throw new InvalidDataException("Reference identity exceeds the allocated packet capacity.");
            references.Add(path, new SettingsReferenceSnapshot.Identity(target.Guid, target.ObjectId.ToString(CultureInfo.InvariantCulture), target.SceneLifetime.ToString(CultureInfo.InvariantCulture)));
            return new Dictionary<string, object?> { ["$go"] = target.Guid };
        }
        if (value is IComponentReference reference && reference.Identity is ComponentReferenceIdentity identity) {
            bytes -= JsonSerializer.SerializeToUtf8Bytes(ComponentReferenceCodec.Address(identity)).Length
                + JsonSerializer.SerializeToUtf8Bytes(SettingsReferenceSnapshot.FromComponent(identity)).Length + path.Length * 6 + 256;
            if (bytes < 0) throw new InvalidDataException("Component reference identity exceeds the allocated packet capacity.");
            references.Add(path, SettingsReferenceSnapshot.FromComponent(identity));
            return new Dictionary<string, object?> { ["$component"] = ComponentReferenceCodec.Address(identity) };
        }
        string json = StructuredFieldCodec.EncodeValue(value, type); bytes -= Encoding.UTF8.GetByteCount(json) + 64;
        return JsonSerializer.Deserialize<JsonElement>(json);
    }
    static object Deferred(string path, string kind, long count, ref int bytes)
    { bytes -= path.Length * 6 + 96; return new Dictionary<string, object?> { ["$deferred"] = path, ["kind"] = kind, ["total"] = count }; }
    /// @note Traverses only FieldInfo and collection indices in the requested path; it never evaluates user properties or scans sibling values.
    static (object? Value, Type Type, ScriptFieldDescriptor Field, string Path) Locate(ManagedTypeDescriptor descriptor, object instance, string path)
    {
        string[] segments = SettingsLeafChanges.Parse(path);
        ScriptFieldDescriptor? root = descriptor.Fields.FirstOrDefault(field => field.IsInspectable && (field.Key == segments[0] || field.PreviousKeys.Contains(segments[0])));
        if (root is null) throw new InvalidDataException($"Unknown readable setting path: {path}.");
        object? value = root.Read(instance); Type type = root.ValueType; string canonical = StructuredFieldCodec.ChildPath("", root.Key);
        foreach (string segment in segments.Skip(1)) {
            if (value is null) throw new InvalidDataException($"Settings page path crosses null at {canonical}.");
            if (value is IList list && ManagedFieldSchema.Element(type) is Type element) {
                if (!int.TryParse(segment, NumberStyles.None, CultureInfo.InvariantCulture, out int index) || index < 0 || index >= list.Count || index.ToString(CultureInfo.InvariantCulture) != segment)
                    throw new InvalidDataException($"Settings page index is out of range at {canonical}.");
                value = list[index]; type = element; canonical = StructuredFieldCodec.ChildPath(canonical, segment);
            } else if (ManagedFieldSchema.Kind(type) == "object") {
                var member = ManagedFieldSchema.Fields(type).FirstOrDefault(field => field.Key == segment || field.Previous.Contains(segment))
                    ?? throw new InvalidDataException($"Settings page member is unavailable at {canonical}.");
                value = member.Field.GetValue(value); type = member.Field.FieldType; canonical = StructuredFieldCodec.ChildPath(canonical, member.Key);
            } else throw new InvalidDataException($"Settings page path crosses a scalar at {canonical}.");
        }
        return (value, type, root, canonical);
    }
}
