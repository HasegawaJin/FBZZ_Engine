/// @file    SettingsAdoptionCapture.cs
/// @brief   Selected Play settings survive Stop as owned primitive/schema snapshots without retaining a game generation.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Fbzz.ManagedHost;

/// @note Capture may scan revisions at this explicit authoring boundary; passive Inspector page reads never use it.
internal sealed class SettingsAdoptionCapture : IDisposable
{
    internal sealed record Leaf(string Path, string Schema, ImmutableValueSnapshot Value);
    readonly List<Leaf> m_leaves = new();
    SettingsAdoptionCapture(string typeKey, string sourceRevision) { TypeKey = typeKey; SourceRevision = sourceRevision; }
    internal string TypeKey { get; }
    internal string SourceRevision { get; }
    internal string Revision { get; private set; } = "";
    internal IReadOnlyList<Leaf> Leaves => m_leaves;
    internal long ResidentBytes => m_leaves.Sum(leaf => leaf.Value.ResidentBytes);
    internal long StorageBytes => m_leaves.Sum(leaf => leaf.Value.StorageBytes);

    /// @note No constructor, getter, lifecycle or normalization callback runs; incomplete captures dispose only their owned storage.
    internal static SettingsAdoptionCapture Capture(ManagedTypeDescriptor descriptor, object instance,
        IEnumerable<string> paths, SnapshotStorageBudget? budget = null)
    {
        budget ??= new();
        string before = SettingsDocument.Hash(SettingsDocument.Capture(descriptor, instance));
        SettingsAdoptionCapture captured = new(descriptor.PersistentId.Length == 0 ? descriptor.TypeName : descriptor.PersistentId, before);
        try {
            using IncrementalHash revision = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            foreach (string requested in paths) {
                SettingsLeafChanges.Location location = SettingsLeafChanges.Locate(descriptor, instance, requested);
                string path = location.Path;
                if (captured.m_leaves.Any(leaf => leaf.Path == path || leaf.Path.StartsWith(path + "/", StringComparison.Ordinal) || path.StartsWith(leaf.Path + "/", StringComparison.Ordinal)))
                    throw new InvalidDataException($"Duplicate or overlapping adoption path: {path}.");
                object? value = SettingsLeafChanges.Get(location.Field.Read(instance), location.Field.ValueType, location.Segments);
                long resident = Math.Max(0, budget.ResidentBytes - captured.ResidentBytes);
                long? spool = budget.SpoolBytes is long limit ? Math.Max(0, limit - captured.StorageBytes) : null;
                ImmutableValueSnapshot snapshot = ImmutableValueSnapshot.Capture(value, location.ValueType, new(resident, spool, budget.Directory));
                try {
                    string schema = Schema(location);
                    captured.m_leaves.Add(new(path, schema, snapshot));
                    revision.AppendData(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new[] { path, schema, snapshot.Revision })));
                } catch { snapshot.Dispose(); throw; }
            }
            if (captured.m_leaves.Count == 0) throw new InvalidDataException("Select at least one Setting path to capture for adoption.");
            string after = SettingsDocument.Hash(SettingsDocument.Capture(descriptor, instance));
            if (before != after) throw new InvalidDataException("SETTINGS_CONFLICT: Play settings changed while the selected adoption values were being captured.");
            captured.Revision = Convert.ToHexString(revision.GetHashAndReset());
            return captured;
        } catch { captured.Dispose(); throw; }
    }

    internal static string Schema(SettingsLeafChanges.Location location) => JsonSerializer.Serialize(new {
        value = ManagedFieldSchema.Build(location.ValueType), minimum = ManagedFieldSchema.RangeBound(location.ValueType, location.Range, false),
        maximum = ManagedFieldSchema.RangeBound(location.ValueType, location.Range, true), requiredReference = location.Field.RequiredReference
    });

    /// @note Validation compares selected schema paths in the destination generation without restoring source-generation Types.
    internal void ValidateDestination(ManagedTypeDescriptor descriptor, object instance)
    {
        string typeKey = descriptor.PersistentId.Length == 0 ? descriptor.TypeName : descriptor.PersistentId;
        if (typeKey != TypeKey) throw new InvalidDataException("ADOPTION_SCHEMA_CHANGED: the saved Script type differs from the captured Play type.");
        foreach (Leaf leaf in m_leaves) {
            SettingsLeafChanges.Location location = SettingsLeafChanges.Locate(descriptor, instance, leaf.Path);
            if (location.Path != leaf.Path || Schema(location) != leaf.Schema)
                throw new InvalidDataException($"ADOPTION_SCHEMA_CHANGED: the selected Setting contract changed at {leaf.Path}.");
        }
    }

    /// @note Explicit preparation streams selected values; the caller validates/remaps every exact reference before creating an Edit candidate.
    internal void WritePatches(Utf8JsonWriter writer, Dictionary<string, object?> references)
    {
        writer.WriteStartArray();
        foreach (Leaf leaf in m_leaves) {
            writer.WriteStartObject(); writer.WriteString("path", leaf.Path); writer.WritePropertyName("value");
            leaf.Value.WriteCanonicalValue(writer, leaf.Path, references); writer.WriteEndObject();
        }
        writer.WriteEndArray();
    }

    internal Dictionary<string, object?> ReadPage(int leafIndex, string path, long start, int count, int packetBytes)
    {
        if ((uint)leafIndex >= (uint)m_leaves.Count) throw new InvalidDataException("Adoption path index is out of range.");
        Leaf leaf = m_leaves[leafIndex];
        int reserve = checked(Encoding.UTF8.GetByteCount(leaf.Path) * 2 + 512);
        if (packetBytes - reserve < 1024) throw new InvalidDataException("Increase the adoption packet capacity to hold this path's metadata.");
        Dictionary<string, object?> page = leaf.Value.ReadPage(path, start, count, packetBytes - reserve);
        page["settingPath"] = leaf.Path; page["sample"] = "captured_play"; page["capturedRevision"] = Revision;
        if (JsonSerializer.SerializeToUtf8Bytes(page).Length > packetBytes) throw new InvalidDataException("Increase the adoption packet capacity for this page's metadata.");
        return page;
    }

    public void Dispose() { foreach (Leaf leaf in m_leaves) leaf.Value.Dispose(); m_leaves.Clear(); }
}
