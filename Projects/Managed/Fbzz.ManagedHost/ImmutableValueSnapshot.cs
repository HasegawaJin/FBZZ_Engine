/// @file    ImmutableValueSnapshot.cs
/// @brief   Incremental immutable values with bounded storage and lazy paths for indexed snapshots.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Fbzz;

namespace Fbzz.ManagedHost;

internal sealed record SnapshotStorageBudget(long ResidentBytes = 1024 * 1024, long? SpoolBytes = null, string? Directory = null);

/// @note Capture retains only primitive records and owned storage after returning, never game Types, FieldInfo, delegates or object graphs.
internal sealed class ImmutableValueSnapshot : IDisposable
{
    enum NodeKind : byte { NULL, BOOL, INT, UINT, FLOAT, STRING, LIST, OBJECT, GAME_OBJECT, COMPONENT_REFERENCE, INT8, UINT8, INT16, UINT16, INT64, UINT64, DOUBLE, DECIMAL, CHAR }
    readonly SnapshotStream m_storage;
    readonly long m_root;
    static readonly UTF8Encoding s_utf8 = new(false, true);
    sealed class PathStack
    {
        readonly List<(string? Key, int Index, bool IsIndex)> m_segments = new(8);
        readonly string m_root;
        internal PathStack(string root = "") => m_root = root;
        internal void PushKey(string key) => m_segments.Add((key, 0, false));
        internal void PushIndex(int index) => m_segments.Add((null, index, true));
        internal void Pop() => m_segments.RemoveAt(m_segments.Count - 1);
        internal string Value
        {
            get {
                string result = m_root;
                foreach (var segment in m_segments)
                    result = StructuredFieldCodec.ChildPath(result, segment.IsIndex
                        ? segment.Index.ToString(CultureInfo.InvariantCulture) : segment.Key!);
                return result;
            }
        }
    }
    ImmutableValueSnapshot(SnapshotStream storage, long root)
    {
        m_storage = storage; m_root = root;
        storage.Position = 0;
        Revision = Convert.ToHexString(SHA256.HashData(storage));
        var node = Header(root); RootKind = Kind(node.Kind); TotalCount = node.Count;
    }
    internal string Revision { get; }
    internal string RootKind { get; }
    internal long TotalCount { get; }
    internal long StorageBytes => m_storage.Length;
    internal bool IsSpooled => m_storage.IsSpooled;
    internal long ResidentBytes => m_storage.ResidentBytes;
    /// @note Streams primitive captured data and exact identities; does not retain or reconstruct a source-generation user type.
    internal void WriteCanonicalValue(Utf8JsonWriter writer, string rootPath, Dictionary<string, object?> references)
        => WriteCanonical(writer, m_root, new PathStack(rootPath), references);
    /// @note Walks stored index nodes only; string/scalar payloads are skipped and no game type, getter or constructor is consulted.
    internal void VisitReferences(string rootPath, Action<string, SettingsReferenceSnapshot.Identity> visitor)
    {
        PathStack path = new(rootPath);
        void Visit(long offset)
        {
            var node = Header(offset); long table = offset + 9;
            if (node.Kind == NodeKind.LIST) {
                for (long index = 0; index < node.Count; ++index) {
                    m_storage.Position = checked(table + index * sizeof(long));
                    using BinaryReader pointer = new(m_storage, s_utf8, true); long child = pointer.ReadInt64();
                    path.PushIndex(checked((int)index));
                    try { Visit(child); } finally { path.Pop(); }
                }
            } else if (node.Kind == NodeKind.OBJECT) {
                long position = table;
                for (long index = 0; index < node.Count; ++index) {
                    m_storage.Position = position; string key = ReadText();
                    using BinaryReader pointer = new(m_storage, s_utf8, true); long child = pointer.ReadInt64(); position = m_storage.Position;
                    path.PushKey(key);
                    try { Visit(child); } finally { path.Pop(); }
                }
            } else if (node.Kind == NodeKind.GAME_OBJECT) {
                using BinaryReader scalar = new(m_storage, s_utf8, true); ulong id = scalar.ReadUInt64(), lifetime = scalar.ReadUInt64(); string guid = ReadText();
                visitor(path.Value, new(guid, id.ToString(CultureInfo.InvariantCulture), lifetime.ToString(CultureInfo.InvariantCulture)));
            } else if (node.Kind == NodeKind.COMPONENT_REFERENCE) {
                var identity = JsonSerializer.Deserialize<ComponentReferenceIdentity>(ReadText()) ?? throw new InvalidDataException("Malformed stored Component identity.");
                visitor(path.Value, SettingsReferenceSnapshot.FromComponent(identity));
            }
        }
        Visit(m_root);
    }
    /// @note Full materialization is an explicit editing/publication step; page readers never take this path.
    internal object? RestoreValue(Type declaredType)
    {
        using MemoryStream json = new(); Dictionary<string, object?> references = new(StringComparer.Ordinal);
        using (Utf8JsonWriter writer = new(json)) WriteCanonical(writer, m_root, new PathStack(), references);
        json.Position = 0; using JsonDocument value = JsonDocument.Parse(json, new JsonDocumentOptions { MaxDepth = StructuredFieldCodec.MAX_DEPTH + 4 });
        return SettingsReferenceSnapshot.Restore(value.RootElement, declaredType, JsonSerializer.SerializeToElement(references));
    }
    void WriteCanonical(Utf8JsonWriter writer, long offset, PathStack path, Dictionary<string, object?> references)
    {
        var node = Header(offset); long table = offset + 9;
        if (node.Kind == NodeKind.LIST) {
            writer.WriteStartArray();
            for (long index = 0; index < node.Count; index++) {
                m_storage.Position = table + index * sizeof(long); using BinaryReader reader = new(m_storage, s_utf8, true); long child = reader.ReadInt64();
                path.PushIndex(checked((int)index));
                try { WriteCanonical(writer, child, path, references); } finally { path.Pop(); }
            }
            writer.WriteEndArray(); return;
        }
        if (node.Kind == NodeKind.OBJECT) {
            writer.WriteStartObject(); long position = table;
            for (long index = 0; index < node.Count; index++) {
                m_storage.Position = position; string name = ReadText(); using BinaryReader reader = new(m_storage, s_utf8, true); long child = reader.ReadInt64(); position = m_storage.Position;
                writer.WritePropertyName(name); path.PushKey(name);
                try { WriteCanonical(writer, child, path, references); } finally { path.Pop(); }
            }
            writer.WriteEndObject(); return;
        }
        if (node.Kind == NodeKind.STRING) {
            using BinaryReader reader = new(m_storage, s_utf8, true); writer.WriteStringValue(s_utf8.GetString(reader.ReadBytes(checked((int)node.Count)))); return;
        }
        if (ExtendedPrimitive(node.Kind)) { writer.WriteRawValue(ReadText()); return; }
        using BinaryReader scalar = new(m_storage, s_utf8, true);
        switch (node.Kind) {
            case NodeKind.NULL: writer.WriteNullValue(); break;
            case NodeKind.BOOL: writer.WriteBooleanValue(scalar.ReadByte() != 0); break;
            case NodeKind.INT: writer.WriteNumberValue(scalar.ReadInt32()); break;
            case NodeKind.UINT: writer.WriteNumberValue(scalar.ReadUInt32()); break;
            case NodeKind.FLOAT: writer.WriteNumberValue(scalar.ReadSingle()); break;
            case NodeKind.GAME_OBJECT:
                ulong id = scalar.ReadUInt64(), lifetime = scalar.ReadUInt64(); string guid = ReadText();
                writer.WriteStartObject(); writer.WriteString("$go", guid); writer.WriteEndObject();
                references.Add(path.Value, new { guid, objectId = id.ToString(CultureInfo.InvariantCulture), sceneLifetime = lifetime.ToString(CultureInfo.InvariantCulture) }); break;
            case NodeKind.COMPONENT_REFERENCE:
                ComponentReferenceIdentity reference = JsonSerializer.Deserialize<ComponentReferenceIdentity>(ReadText())!;
                writer.WriteStartObject(); writer.WritePropertyName("$component"); JsonSerializer.Serialize(writer, ComponentReferenceCodec.Address(reference)); writer.WriteEndObject();
                references.Add(path.Value, SettingsReferenceSnapshot.FromComponent(reference)); break;
            default: throw new InvalidDataException("Unknown snapshot node.");
        }
    }
    internal static ImmutableValueSnapshot Capture(object? value, Type declaredType, SnapshotStorageBudget? budget = null)
    {
        SnapshotStream storage = new(budget ?? new());
        try { long root = CaptureNode(storage, value, declaredType, new HashSet<object>(ReferenceEqualityComparer.Instance), 0, new PathStack()); return new(storage, root); }
        catch { storage.Dispose(); throw; }
    }
    internal static ImmutableValueSnapshot CaptureFields(IEnumerable<(string Key, object? Value, Type Type)> fields, SnapshotStorageBudget? budget = null)
    {
        SnapshotStream storage = new(budget ?? new());
        try {
            var members = fields.ToArray();
            if (members.Select(member => member.Key).Distinct(StringComparer.Ordinal).Count() != members.Length) throw new InvalidDataException("Snapshot root contains duplicate fields.");
            long root = Object(storage, members.Select(member => member.Key).ToArray());
            long[] slots = ObjectSlots(storage, root);
            PathStack path = new();
            for (int index = 0; index < members.Length; index++) {
                var member = members[index];
                path.PushKey(member.Key);
                long child;
                try { child = CaptureNode(storage, member.Value, member.Type, new HashSet<object>(ReferenceEqualityComparer.Instance), 0, path); }
                finally { path.Pop(); }
                Pointer(storage, slots[index], child);
            }
            return new(storage, root);
        } catch { storage.Dispose(); throw; }
    }
    static long CaptureNode(SnapshotStream storage, object? value, Type type, HashSet<object> ancestors, int depth, PathStack path)
    {
        if (depth > StructuredFieldCodec.MAX_DEPTH) throw new InvalidDataException($"Snapshot nesting exceeds its schema depth at {path.Value}.");
        if (!ManagedFieldSchema.IsSupported(type)) throw new InvalidDataException($"Unsupported snapshot type at {path.Value}: {type.FullName}.");
        if (value is null) {
            if (!ManagedFieldSchema.MayBeNull(type)) throw new InvalidDataException($"Null string/collection/value at {path.Value}.");
            return Header(storage, NodeKind.NULL, 0);
        }
        if (value.GetType() != type && type != typeof(GameObject)) throw new InvalidDataException($"Polymorphic snapshot values are unsupported at {path.Value}.");
        if (type == typeof(string)) return String(storage, (string)value);
        if (value is IAssetReference asset) return String(storage, asset.Reference ?? "");
        if (value is IComponentReference component) {
            if (component.Identity is not ComponentReferenceIdentity reference) return Header(storage, NodeKind.NULL, 0);
            ComponentReferenceCodec.Validate(reference, ComponentReferenceCodec.TargetType(type));
            long offset = Header(storage, NodeKind.COMPONENT_REFERENCE, 1);
            WriteText(storage, JsonSerializer.Serialize(reference)); return offset;
        }
        if (value is GameObject target) {
            long offset = Header(storage, NodeKind.GAME_OBJECT, 1);
            WriteUInt64(storage, target.ObjectId); WriteUInt64(storage, target.SceneLifetime);
            WriteText(storage, ScriptFieldCodec.ValidateGuid(target.Guid)); return offset;
        }
        if (type == typeof(bool)) { long offset = Header(storage, NodeKind.BOOL, 1); storage.WriteByte((bool)value ? (byte)1 : (byte)0); return offset; }
        if (type == typeof(int)) { long offset = Header(storage, NodeKind.INT, 1); WriteInt32(storage, (int)value); return offset; }
        if (type.IsEnum) {
            if (type.IsEnum && !ScriptFieldCodec.ValidEnum(type, value)) throw new InvalidDataException($"Invalid enum value at {path.Value}.");
            Type underlying = Enum.GetUnderlyingType(type); value = Convert.ChangeType(value, underlying, CultureInfo.InvariantCulture); type = underlying;
        }
        if (type == typeof(int)) { long offset = Header(storage, NodeKind.INT, 1); WriteInt32(storage, (int)value); return offset; }
        if (type == typeof(uint)) { long offset = Header(storage, NodeKind.UINT, 1); WriteUInt32(storage, (uint)value); return offset; }
        if (type == typeof(float)) {
            float number = (float)value;
            if (!float.IsFinite(number)) throw new InvalidDataException($"Nonfinite snapshot number at {path.Value}.");
            long offset = Header(storage, NodeKind.FLOAT, 1); WriteInt32(storage, BitConverter.SingleToInt32Bits(number)); return offset;
        }
        if (ManagedPrimitiveCodec.Kind(type) is string primitive) {
            NodeKind kind = primitive switch { "int8" => NodeKind.INT8, "uint8" => NodeKind.UINT8, "int16" => NodeKind.INT16, "uint16" => NodeKind.UINT16,
                "int64" => NodeKind.INT64, "uint64" => NodeKind.UINT64, "double" => NodeKind.DOUBLE, "decimal" => NodeKind.DECIMAL, "char" => NodeKind.CHAR,
                _ => throw new InvalidDataException("Unknown primitive snapshot kind.") };
            long offset = Header(storage, kind, 1); WriteText(storage, JsonSerializer.Serialize(ManagedPrimitiveCodec.WireValue(value, type))); return offset;
        }
        float[]? math = value switch {
            Vector2 vector => [vector.X, vector.Y], Vector3 vector => [vector.X, vector.Y, vector.Z],
            Vector4 vector => [vector.X, vector.Y, vector.Z, vector.W], Quaternion rotation => [rotation.X, rotation.Y, rotation.Z, rotation.W], _ => null
        };
        if (math is not null) return CaptureNode(storage, math, typeof(float[]), ancestors, depth, path);
        if (!type.IsValueType && !ancestors.Add(value)) throw new InvalidDataException($"Cyclic snapshot value at {path.Value}.");
        try {
            if (ManagedFieldSchema.Element(type) is Type element) {
                IList items = (IList)value;
                long root = Header(storage, NodeKind.LIST, items.Count), table = storage.Position;
                storage.Reserve(checked((long)items.Count * sizeof(long)));
                if (element == typeof(int)) {
                    IList<int> integers = (IList<int>)value;
                    for (int index = 0; index < integers.Count; index++) {
                        path.PushIndex(index);
                        try {
                            if (depth + 1 > StructuredFieldCodec.MAX_DEPTH) throw new InvalidDataException($"Snapshot nesting exceeds its supported schema depth at {path.Value}.");
                            long child = Header(storage, NodeKind.INT, 1); WriteInt32(storage, integers[index]);
                            Pointer(storage, table + (long)index * sizeof(long), child);
                        } finally { path.Pop(); }
                    }
                } else for (int index = 0; index < items.Count; index++) {
                    path.PushIndex(index);
                    try {
                        long child = CaptureNode(storage, items[index], element, ancestors, depth + 1, path);
                        Pointer(storage, table + (long)index * sizeof(long), child);
                    } finally { path.Pop(); }
                }
                return root;
            }
            ManagedFieldSchema.Member[] members = ManagedFieldSchema.Fields(type);
            long result = Object(storage, members.Select(member => member.Key).ToArray());
            long[] slots = ObjectSlots(storage, result);
            for (int index = 0; index < members.Length; index++) {
                var member = members[index]; object? child = member.Field.GetValue(value);
                if (member.Field.GetCustomAttributes(typeof(ValueRangeAttribute), false).FirstOrDefault() is ValueRangeAttribute range && !ManagedFieldSchema.WithinRange(child, range)) {
                    path.PushKey(member.Key);
                    try { throw new InvalidDataException($"Snapshot range rejected {path.Value}."); }
                    finally { path.Pop(); }
                }
                path.PushKey(member.Key);
                long offset;
                try { offset = CaptureNode(storage, child, member.Field.FieldType, ancestors, depth + 1, path); }
                finally { path.Pop(); }
                Pointer(storage, slots[index], offset);
            }
            return result;
        } finally { if (!type.IsValueType) ancestors.Remove(value); }
    }
    /// @note leaveOpenのBinaryWriter.DisposeはFlushするため、値ごとのwriterを作らず同じlittle-endian形式を直接書く。
    /// @see https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Private.CoreLib/src/System/IO/BinaryWriter.cs BinaryWriter.Dispose(bool)。
    static long Header(SnapshotStream storage, NodeKind kind, long count)
    { storage.Position = storage.Length; long offset = storage.Position; storage.WriteByte((byte)kind); WriteInt64(storage, count); return offset; }
    static long Object(SnapshotStream storage, string[] keys)
    {
        long offset = Header(storage, NodeKind.OBJECT, keys.Length);
        foreach (string key in keys) { WriteText(storage, key); WriteInt64(storage, 0); }
        return offset;
    }
    static long[] ObjectSlots(SnapshotStream storage, long offset)
    {
        long end = storage.Length; storage.Position = offset + 1;
        using BinaryReader reader = new(storage, s_utf8, true); long count = reader.ReadInt64(); long[] slots = new long[checked((int)count)];
        for (int index = 0; index < slots.Length; index++) { long length = reader.ReadInt64(); storage.Position += length; slots[index] = storage.Position; storage.Position += sizeof(long); }
        storage.Position = end; return slots;
    }
    static void Pointer(SnapshotStream storage, long slot, long child)
    { long end = storage.Length; storage.Position = slot; WriteInt64(storage, child); storage.Position = end; }
    static long String(SnapshotStream storage, string text)
    { long offset = Header(storage, NodeKind.STRING, s_utf8.GetByteCount(text)); WriteUtf8(storage, text); return offset; }
    static void WriteText(SnapshotStream storage, string text)
    { WriteInt64(storage, s_utf8.GetByteCount(text)); WriteUtf8(storage, text); }
    static void WriteInt32(SnapshotStream storage, int value)
    { Span<byte> bytes = stackalloc byte[sizeof(int)]; BinaryPrimitives.WriteInt32LittleEndian(bytes, value); storage.Write(bytes); }
    static void WriteUInt32(SnapshotStream storage, uint value)
    { Span<byte> bytes = stackalloc byte[sizeof(uint)]; BinaryPrimitives.WriteUInt32LittleEndian(bytes, value); storage.Write(bytes); }
    static void WriteInt64(SnapshotStream storage, long value)
    { Span<byte> bytes = stackalloc byte[sizeof(long)]; BinaryPrimitives.WriteInt64LittleEndian(bytes, value); storage.Write(bytes); }
    static void WriteUInt64(SnapshotStream storage, ulong value)
    { Span<byte> bytes = stackalloc byte[sizeof(ulong)]; BinaryPrimitives.WriteUInt64LittleEndian(bytes, value); storage.Write(bytes); }
    static void WriteUtf8(SnapshotStream storage, string text)
    {
        Encoder encoder = s_utf8.GetEncoder(); byte[] bytes = new byte[8192]; ReadOnlySpan<char> remaining = text.AsSpan();
        do {
            encoder.Convert(remaining, bytes, true, out int characters, out int written, out bool complete);
            storage.Write(bytes.AsSpan(0, written)); remaining = remaining[characters..]; if (complete) break;
        } while (true);
    }
    (NodeKind Kind, long Count) Header(long offset)
    { m_storage.Position = offset; int kind = m_storage.ReadByte(); using BinaryReader reader = new(m_storage, s_utf8, true); return ((NodeKind)kind, reader.ReadInt64()); }
    static bool ExtendedPrimitive(NodeKind kind) => kind >= NodeKind.INT8;
    static string Kind(NodeKind kind) => kind switch { NodeKind.NULL => "null", NodeKind.BOOL => "bool", NodeKind.INT => "int", NodeKind.UINT => "uint", NodeKind.FLOAT => "float", NodeKind.STRING => "string", NodeKind.LIST => "list", NodeKind.OBJECT => "object", NodeKind.GAME_OBJECT => "gameObject", NodeKind.COMPONENT_REFERENCE => "componentRef",
        NodeKind.INT8 => "int8", NodeKind.UINT8 => "uint8", NodeKind.INT16 => "int16", NodeKind.UINT16 => "uint16", NodeKind.INT64 => "int64", NodeKind.UINT64 => "uint64", NodeKind.DOUBLE => "double", NodeKind.DECIMAL => "decimal", NodeKind.CHAR => "char", _ => "unknown" };
    long Locate(string path)
    {
        if (path.Length == 0) return m_root;
        long offset = m_root;
        foreach (string key in SettingsLeafChanges.Parse(path)) {
            var node = Header(offset);
            if (node.Kind == NodeKind.LIST) {
                if (!long.TryParse(key, NumberStyles.None, CultureInfo.InvariantCulture, out long index) || index < 0 || index >= node.Count || index.ToString(CultureInfo.InvariantCulture) != key)
                    throw new InvalidDataException($"Snapshot collection path is out of range: {path}.");
                m_storage.Position = offset + 9 + index * sizeof(long); using BinaryReader reader = new(m_storage, s_utf8, true); offset = reader.ReadInt64();
            } else if (node.Kind == NodeKind.OBJECT) {
                bool found = false;
                for (long index = 0; index < node.Count; index++) {
                    string name = ReadText(); using BinaryReader reader = new(m_storage, s_utf8, true); long child = reader.ReadInt64();
                    if (name == key) { offset = child; found = true; break; }
                }
                if (!found) throw new InvalidDataException($"Snapshot field path is unavailable: {path}.");
            } else throw new InvalidDataException($"Snapshot path crosses a scalar: {path}.");
        }
        return offset;
    }
    string ReadText()
    { using BinaryReader reader = new(m_storage, s_utf8, true); long length = reader.ReadInt64(); return s_utf8.GetString(reader.ReadBytes(checked((int)length))); }
    internal Dictionary<string, object?> ReadValue(string relativePath, int wireBudget)
    {
        Budget(wireBudget); long offset = Locate(relativePath); var node = Header(offset);
        Dictionary<string, object?> references = new(StringComparer.Ordinal); int remaining = Math.Max(0, wireBudget - 512);
        object? value = ReadNode(offset, relativePath, ref remaining, references);
        Dictionary<string, object?> result = new() { ["path"] = relativePath, ["kind"] = Kind(node.Kind), ["total"] = node.Count, ["value"] = value, ["references"] = references };
        CheckPacket(result, wireBudget); return result;
    }
    internal Dictionary<string, object?> ReadPage(string relativePath, long start, int count, int wireBudget)
    {
        Budget(wireBudget); if (start < 0 || count < 0) throw new InvalidDataException("Snapshot page range must be nonnegative.");
        long offset = Locate(relativePath); var node = Header(offset);
        Dictionary<string, object?> result = new() { ["path"] = relativePath, ["kind"] = Kind(node.Kind), ["total"] = node.Count, ["start"] = start };
        if (node.Kind == NodeKind.STRING) {
            if (start > node.Count) throw new InvalidDataException("Snapshot text start exceeds its byte length.");
            m_storage.Position = offset + 9 + start;
            if (start < node.Count) { int leading = m_storage.ReadByte(); if ((leading & 0xc0) == 0x80) throw new InvalidDataException("Snapshot text start must be a UTF-8 character boundary."); }
            int size = (int)Math.Min(Math.Min((long)count, node.Count - start), (wireBudget - 512) / 6);
            byte[] bytes = new byte[size]; m_storage.Position = offset + 9 + start; m_storage.ReadExactly(bytes);
            if (start + size < node.Count) {
                m_storage.Position = offset + 9 + start + size; int next = m_storage.ReadByte();
                while (size > 0 && (next & 0xc0) == 0x80) { size--; next = bytes[size]; }
            }
            if (size == 0 && start < node.Count && count != 0) throw new InvalidDataException("Text packet capacity must hold one UTF-8 character; request at least four bytes.");
            result["text"] = s_utf8.GetString(bytes, 0, size); result["unit"] = "utf8_bytes"; result["returned"] = size;
            result["nextStart"] = start + size < node.Count ? start + size : null; result["references"] = new Dictionary<string, object?>();
        } else if (node.Kind == NodeKind.LIST || node.Kind == NodeKind.OBJECT) {
            if (start > node.Count) throw new InvalidDataException("Snapshot page start exceeds its item count.");
            List<object?> items = new(); Dictionary<string, object?> references = new(StringComparer.Ordinal);
            long returned = 0, position = offset + 9; int remaining = Math.Max(0, wireBudget - 512);
            if (node.Kind == NodeKind.OBJECT) {
                m_storage.Position = position;
                for (long index = 0; index < start; index++) { using BinaryReader reader = new(m_storage, s_utf8, true); long size = reader.ReadInt64(); m_storage.Position += size + sizeof(long); }
                position = m_storage.Position;
            }
            for (long index = start; index < node.Count && returned < count; index++) {
                if (remaining < 128) break;
                string key; long child;
                if (node.Kind == NodeKind.LIST) { key = index.ToString(CultureInfo.InvariantCulture); m_storage.Position = offset + 9 + index * sizeof(long); using BinaryReader reader = new(m_storage, s_utf8, true); child = reader.ReadInt64(); }
                else { m_storage.Position = position; key = ReadText(); using BinaryReader reader = new(m_storage, s_utf8, true); child = reader.ReadInt64(); position = m_storage.Position; }
                object? value = ReadNode(child, StructuredFieldCodec.ChildPath(relativePath, key), ref remaining, references);
                items.Add(node.Kind == NodeKind.OBJECT ? new Dictionary<string, object?> { ["name"] = key, ["value"] = value } : value); returned++;
            }
            result["items"] = items; result["returned"] = returned; result["nextStart"] = start + returned < node.Count ? start + returned : null; result["references"] = references;
        } else return ReadValue(relativePath, wireBudget);
        CheckPacket(result, wireBudget); return result;
    }
    object? ReadNode(long offset, string path, ref int remaining, Dictionary<string, object?> references)
    {
        var node = Header(offset);
        if (node.Kind is NodeKind.LIST or NodeKind.OBJECT) return Deferred(path, node.Kind, node.Count, ref remaining);
        if (node.Kind == NodeKind.STRING) {
            if (node.Count > Math.Max(0, remaining - 32) / 6) return Deferred(path, node.Kind, node.Count, ref remaining);
            using BinaryReader reader = new(m_storage, s_utf8, true); string text = s_utf8.GetString(reader.ReadBytes(checked((int)node.Count))); remaining -= 16 + s_utf8.GetByteCount(text) * 6; return text;
        }
        if (ExtendedPrimitive(node.Kind)) {
            string json = ReadText(); remaining -= s_utf8.GetByteCount(json) + 32; return JsonSerializer.Deserialize<JsonElement>(json);
        }
        using BinaryReader scalar = new(m_storage, s_utf8, true);
        if (node.Kind == NodeKind.GAME_OBJECT) {
            ulong id = scalar.ReadUInt64(), lifetime = scalar.ReadUInt64(); string guid = ReadText();
            if (guid.Length * 6L + path.Length * 6L + 256 > remaining) return Deferred(path, node.Kind, node.Count, ref remaining);
            remaining -= guid.Length * 6 + path.Length * 6 + 256;
            references.Add(path, new { guid, objectId = id.ToString(CultureInfo.InvariantCulture), sceneLifetime = lifetime.ToString(CultureInfo.InvariantCulture) });
            return new Dictionary<string, object?> { ["$go"] = guid };
        }
        if (node.Kind == NodeKind.COMPONENT_REFERENCE) {
            ComponentReferenceIdentity identity = JsonSerializer.Deserialize<ComponentReferenceIdentity>(ReadText())!;
            object address = new Dictionary<string, object?> { ["$component"] = ComponentReferenceCodec.Address(identity) };
            int bytes = JsonSerializer.SerializeToUtf8Bytes(address).Length + JsonSerializer.SerializeToUtf8Bytes(SettingsReferenceSnapshot.FromComponent(identity)).Length + path.Length * 6 + 128;
            if (bytes > remaining) return Deferred(path, node.Kind, node.Count, ref remaining);
            remaining -= bytes; references.Add(path, SettingsReferenceSnapshot.FromComponent(identity)); return address;
        }
        remaining -= 32;
        return node.Kind switch { NodeKind.NULL => null, NodeKind.BOOL => scalar.ReadByte() != 0, NodeKind.INT => scalar.ReadInt32(), NodeKind.UINT => scalar.ReadUInt32(), NodeKind.FLOAT => scalar.ReadSingle(), _ => throw new InvalidDataException("Unknown snapshot node kind.") };
    }
    static object Deferred(string path, NodeKind kind, long count, ref int remaining)
    {
        long bytes = 64L + s_utf8.GetByteCount(path) * 6L;
        remaining = bytes < remaining ? remaining - (int)bytes : 0;
        return new Dictionary<string, object?> { ["$deferred"] = path, ["kind"] = Kind(kind), ["total"] = count };
    }
    static void Budget(int bytes) { if (bytes < 1024) throw new InvalidDataException("Snapshot packet capacity requires at least 1024 bytes including its envelope."); }
    static void CheckPacket(Dictionary<string, object?> result, int budget)
    { int required = JsonSerializer.SerializeToUtf8Bytes(result).Length; if (required > budget) throw new InvalidDataException($"Snapshot packet capacity {budget} is insufficient; requiredBytes={required}. Request a larger packet or narrower path."); }
    public void Dispose() => m_storage.Dispose();

    /// @note Only storage exceeding the requested resident budget spills; the optional spool quota is an explicit resource allocation, not a value-length rule.
    internal sealed class SnapshotStream : Stream
    {
        Stream m_stream = new MemoryStream(); readonly SnapshotStorageBudget m_budget;
        internal SnapshotStream(SnapshotStorageBudget budget) {
            if (budget.ResidentBytes < 0 || budget.SpoolBytes < 0) throw new ArgumentOutOfRangeException(nameof(budget)); m_budget = budget;
        }
        internal bool IsSpooled => m_stream is PagedStorageStream;
        internal long ResidentBytes => m_stream is PagedStorageStream spooled ? spooled.ResidentBytes : m_stream.Length;
        void Ensure(long required)
        {
            if (!IsSpooled && required > m_budget.ResidentBytes) {
                string directory = m_budget.Directory ?? Path.GetTempPath(); Directory.CreateDirectory(directory);
                string path = Path.Combine(directory, "fbzz-snapshot-" + Guid.NewGuid().ToString("N") + ".tmp");
                FileStream spool = new(path, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None, 8192, FileOptions.DeleteOnClose | FileOptions.RandomAccess);
                long position = m_stream.Position;
                try {
                    m_stream.Position = 0; m_stream.CopyTo(spool);
                } catch { spool.Dispose(); throw; }
                m_stream.Dispose();
                try {
                    PagedStorageStream cached = new(spool, Math.Min(m_budget.ResidentBytes, PagedStorageStream.MAX_CACHE_BYTES));
                    cached.Position = position; m_stream = cached;
                } catch { spool.Dispose(); throw; }
            }
            if (IsSpooled && m_budget.SpoolBytes is long capacity && required > capacity) throw new IOException($"Snapshot spool resource exhausted: requiredBytes={required}, allocatedBytes={capacity}. Increase the explicit storage budget or close retained snapshots.");
        }
        internal void Reserve(long bytes) { long position = Position; SetLength(checked(position + bytes)); Position = position + bytes; }
        public override bool CanRead => m_stream.CanRead; public override bool CanSeek => m_stream.CanSeek; public override bool CanWrite => m_stream.CanWrite;
        public override long Length => m_stream.Length;
        public override long Position { get => m_stream.Position; set => m_stream.Position = value; }
        public override void Flush() => m_stream.Flush();
        public override int Read(byte[] buffer, int offset, int count) => m_stream.Read(buffer, offset, count);
        public override int Read(Span<byte> buffer) => m_stream.Read(buffer);
        public override long Seek(long offset, SeekOrigin origin) => m_stream.Seek(offset, origin);
        public override void SetLength(long value) { Ensure(value); m_stream.SetLength(value); }
        public override void Write(byte[] buffer, int offset, int count) { Ensure(checked(Position + count)); m_stream.Write(buffer, offset, count); }
        public override void Write(ReadOnlySpan<byte> buffer) { Ensure(checked(Position + buffer.Length)); m_stream.Write(buffer); }
        protected override void Dispose(bool disposing) { if (disposing) m_stream.Dispose(); base.Dispose(disposing); }
    }

    /// @note Dirty node and index pages stay within the snapshot's resident budget while the spool remains the sole durable owner.
    sealed class PagedStorageStream : Stream
    {
        const int PAGE_BYTES = 4096;
        const int PAGE_ACCOUNTED_BYTES = PAGE_BYTES + 128;
        internal const int MAX_CACHE_BYTES = 64 * 1024;
        sealed class Page
        {
            internal readonly byte[] Bytes = new byte[PAGE_BYTES];
            internal long Index = -1;
            internal long LastUse;
            internal bool Dirty;
        }
        readonly FileStream m_file;
        Page[] m_pagePool;
        readonly Dictionary<long, Page> m_pages;
        long m_position;
        long m_length;
        long m_clock;
        bool m_disposed;
        internal PagedStorageStream(FileStream file, long cacheBytes)
        {
            m_file = file;
            int pageCapacity = checked((int)(Math.Min(cacheBytes, MAX_CACHE_BYTES) / PAGE_ACCOUNTED_BYTES));
            m_pagePool = new Page[pageCapacity];
            for (int index = 0; index < m_pagePool.Length; index++) m_pagePool[index] = new Page();
            m_pages = new Dictionary<long, Page>(pageCapacity);
            m_length = file.Length;
        }
        internal long ResidentBytes => (long)m_pagePool.Length * PAGE_ACCOUNTED_BYTES;
        public override bool CanRead => m_file.CanRead;
        public override bool CanSeek => m_file.CanSeek;
        public override bool CanWrite => m_file.CanWrite;
        public override long Length => m_length;
        public override long Position
        {
            get => m_position;
            set { if (value < 0) throw new ArgumentOutOfRangeException(nameof(value)); m_position = value; }
        }
        public override void Flush()
        {
            foreach ((long index, Page page) in m_pages) FlushPage(index, page);
            m_file.Flush();
        }
        public override int Read(byte[] buffer, int offset, int count)
        {
            ArgumentNullException.ThrowIfNull(buffer);
            ArgumentOutOfRangeException.ThrowIfNegative(offset);
            ArgumentOutOfRangeException.ThrowIfNegative(count);
            if (offset > buffer.Length - count) throw new ArgumentException("Read range exceeds the destination buffer.");
            return Read(buffer.AsSpan(offset, count));
        }
        public override int Read(Span<byte> buffer)
        {
            if (m_position >= m_length || buffer.IsEmpty) return 0;
            int remaining = (int)Math.Min(buffer.Length, m_length - m_position), total = remaining;
            if (m_pagePool.Length == 0) {
                m_file.Position = m_position; int read = m_file.Read(buffer[..remaining]); m_position += read; return read;
            }
            while (remaining != 0) {
                long index = m_position / PAGE_BYTES; int within = (int)(m_position % PAGE_BYTES);
                int size = Math.Min(remaining, PAGE_BYTES - within); Page page = GetPage(index);
                page.Bytes.AsSpan(within, size).CopyTo(buffer[(total - remaining)..]);
                remaining -= size; m_position += size;
            }
            return total;
        }
        public override long Seek(long offset, SeekOrigin origin)
        {
            long next = origin switch {
                SeekOrigin.Begin => offset,
                SeekOrigin.Current => checked(m_position + offset),
                SeekOrigin.End => checked(m_length + offset),
                _ => throw new ArgumentOutOfRangeException(nameof(origin))
            };
            Position = next; return next;
        }
        public override void SetLength(long value)
        {
            if (value < 0) throw new ArgumentOutOfRangeException(nameof(value));
            Flush(); m_file.SetLength(value); m_length = value; ClearPages();
        }
        public override void Write(byte[] buffer, int offset, int count)
        {
            ArgumentNullException.ThrowIfNull(buffer);
            ArgumentOutOfRangeException.ThrowIfNegative(offset);
            ArgumentOutOfRangeException.ThrowIfNegative(count);
            if (offset > buffer.Length - count) throw new ArgumentException("Write range exceeds the source buffer.");
            Write(buffer.AsSpan(offset, count));
        }
        public override void Write(ReadOnlySpan<byte> buffer)
        {
            if (buffer.IsEmpty) return;
            long end = checked(m_position + buffer.Length);
            if (m_pagePool.Length == 0) {
                m_file.Position = m_position; m_file.Write(buffer); m_position = end; m_length = Math.Max(m_length, end); return;
            }
            m_length = Math.Max(m_length, end);
            int consumed = 0;
            while (consumed < buffer.Length) {
                long index = m_position / PAGE_BYTES; int within = (int)(m_position % PAGE_BYTES);
                int size = Math.Min(buffer.Length - consumed, PAGE_BYTES - within); Page page = GetPage(index);
                buffer.Slice(consumed, size).CopyTo(page.Bytes.AsSpan(within, size));
                page.Dirty = true;
                consumed += size; m_position += size;
            }
        }
        public override void WriteByte(byte value)
        {
            Span<byte> single = stackalloc byte[1]; single[0] = value; Write(single);
        }
        Page GetPage(long index)
        {
            if (m_pages.TryGetValue(index, out Page? found)) { found.LastUse = ++m_clock; return found; }
            Page? page = m_pagePool.FirstOrDefault(candidate => candidate.Index < 0);
            if (page is null) {
                long oldest = m_pages.MinBy(pair => pair.Value.LastUse).Key;
                page = m_pages[oldest]; FlushPage(oldest, page); m_pages.Remove(oldest); page.Index = -1;
            }
            Array.Clear(page.Bytes);
            long start = checked(index * PAGE_BYTES);
            long available = Math.Min(PAGE_BYTES, Math.Min(Math.Max(0, m_length - start), Math.Max(0, m_file.Length - start)));
            if (available != 0) {
                m_file.Position = start; int loaded = 0;
                while (loaded < available) {
                    int read = m_file.Read(page.Bytes, loaded, checked((int)available - loaded));
                    if (read == 0) break;
                    loaded += read;
                }
            }
            page.Index = index; page.LastUse = ++m_clock; page.Dirty = false; m_pages.Add(index, page); return page;
        }
        void FlushPage(long index, Page page)
        {
            if (!page.Dirty) return;
            long start = checked(index * PAGE_BYTES);
            int size = checked((int)Math.Min(PAGE_BYTES, Math.Max(0, m_length - start)));
            if (size != 0) { m_file.Position = start; m_file.Write(page.Bytes, 0, size); }
            page.Dirty = false;
        }
        void ClearPages()
        {
            m_pages.Clear();
            foreach (Page page in m_pagePool) { page.Index = -1; page.Dirty = false; Array.Clear(page.Bytes); }
        }
        protected override void Dispose(bool disposing)
        {
            if (disposing && !m_disposed) {
                m_disposed = true;
                try { Flush(); }
                finally {
                    try { ClearPages(); }
                    finally { m_pagePool = Array.Empty<Page>(); m_file.Dispose(); }
                }
            }
            base.Dispose(disposing);
        }
    }
}
