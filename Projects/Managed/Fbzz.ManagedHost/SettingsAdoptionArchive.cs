/// @file    SettingsAdoptionArchive.cs
/// @brief   Indexed reference/proof records keep large adoption metadata in owned bounded-resident storage.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;
namespace Fbzz.ManagedHost;
internal sealed class SettingsAdoptionArchive : IDisposable
{
    readonly ImmutableValueSnapshot.SnapshotStream m_offsets = new(new(ResidentBytes: 65536));
    readonly ImmutableValueSnapshot.SnapshotStream m_values = new(new(ResidentBytes: 65536));
    static readonly UTF8Encoding s_utf8 = new(false, true);
    internal long Count { get; private set; }
    internal long ResidentBytes => m_offsets.ResidentBytes + m_values.ResidentBytes;
    internal void Append<T>(T value)
    {
        byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(value);
        m_values.Position = m_values.Length; long offset = m_values.Position;
        using (BinaryWriter writer = new(m_values, s_utf8, true)) { writer.Write(bytes.Length); writer.Write(bytes); }
        m_offsets.Position = m_offsets.Length; using (BinaryWriter writer = new(m_offsets, s_utf8, true)) writer.Write(offset);
        Count++;
    }
    internal JsonElement Read(long index)
    {
        if (index < 0 || index >= Count) throw new InvalidDataException("Adoption archive index is out of range.");
        m_offsets.Position = checked(index * sizeof(long)); using BinaryReader pointer = new(m_offsets, s_utf8, true);
        m_values.Position = pointer.ReadInt64(); using BinaryReader reader = new(m_values, s_utf8, true);
        int size = reader.ReadInt32(); using JsonDocument document = JsonDocument.Parse(reader.ReadBytes(size));
        return document.RootElement.Clone();
    }
    internal Dictionary<string, object?> ReadPage(long start, int count, int capacity)
    {
        if (start < 0 || start > Count || count < 0 || capacity < 1024) throw new InvalidDataException("Adoption archive page bounds/capacity are invalid.");
        List<JsonElement> items = new(); long end = start + Math.Min((long)count, Count - start); int remaining = capacity - 256;
        for (long index = start; index < end; index++) {
            JsonElement item = Read(index); int size = JsonSerializer.SerializeToUtf8Bytes(item).Length;
            if (size + 1 > remaining) {
                if (items.Count == 0) throw new InvalidDataException("Increase the adoption archive packet capacity for this reference path.");
                break;
            }
            items.Add(item); remaining -= size + 1;
        }
        Dictionary<string, object?> page = new() { ["total"] = Count, ["start"] = start, ["returned"] = items.Count,
            ["nextStart"] = start + items.Count < Count ? start + items.Count : null, ["items"] = items };
        if (JsonSerializer.SerializeToUtf8Bytes(page).Length > capacity) throw new InvalidDataException("Increase the adoption archive packet capacity for its envelope.");
        return page;
    }
    public void Dispose() { try { m_offsets.Dispose(); } finally { m_values.Dispose(); } }
}
