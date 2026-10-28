/// @file    ManagedSettingsAdoption.cs
/// @brief   Capture handles own primitive/schema storage across source generation retirement and release it explicitly.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
namespace Fbzz.ManagedHost;
public sealed partial class ManagedScriptRuntime
{
    /// @note This is a retained-handle resource budget; it does not limit settings values, arrays, paths or encoded document length.
    const int MAX_ADOPTION_CAPTURES = 256;
    sealed record AdoptionCapture(ulong SourceGeneration, ulong SourceInstance, string OwnerGuid, string SavedScriptId,
        string SourceBirth, ulong SourceSceneLifetime, SettingsAdoptionCapture Value) : IDisposable
    {
        internal SettingsAdoptionArchive References { get; } = new();
        internal SettingsAdoptionArchive Proofs { get; } = new();
        internal bool ProofComplete;
        public void Dispose() { Value.Dispose(); References.Dispose(); Proofs.Dispose(); }
    }
    readonly Dictionary<ulong, AdoptionCapture> m_adoptionCaptures = new();
    ulong m_nextAdoptionCapture = 1;
    bool m_adoptionIdsExhausted;

    public ManagedRuntimeResult CaptureSettingsAdoption(ulong generation, ulong instanceId, string input, out ulong captureId, out string summary)
    {
        captureId = 0; summary = "";
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (!TryInstance(generation, instanceId, out ScriptInstance? source)) return new(BridgeStatus.STALE_GENERATION);
        if (m_adoptionIdsExhausted) return new(BridgeStatus.PENDING_LIMIT, "Adoption capture handles are exhausted; start a new Host session.");
        if (m_adoptionCaptures.Count >= MAX_ADOPTION_CAPTURES) return new(BridgeStatus.PENDING_LIMIT, "Discard unused adoption captures before retaining another capture.");
        SettingsAdoptionCapture? pending = null;
        AdoptionCapture? pendingCapture = null;
        try {
            using JsonDocument request = JsonDocument.Parse(input); Keys(request.RootElement, "paths");
            JsonElement paths = request.RootElement.GetProperty("paths");
            if (paths.ValueKind != JsonValueKind.Array) throw new InvalidDataException("Select canonical Setting paths for adoption.");
            pending = SettingsAdoptionCapture.Capture(source.Type, source.Instance, paths.EnumerateArray().Select(path =>
                path.GetString() ?? throw new InvalidDataException("Adoption paths require strings.")));
            AdoptionCapture capture = new(generation, instanceId, source.Binding.GameObject.Guid, source.Binding.SavedScriptId,
                source.Binding.ScriptBirth, source.Binding.GameObject.SceneLifetime, pending);
            pendingCapture = capture; pending = null;
            foreach (var leaf in capture.Value.Leaves)
                leaf.Value.VisitReferences(leaf.Path, (path, identity) => capture.References.Append(new { path, identity }));
            capture.ProofComplete = capture.References.Count == 0;
            ulong next = m_nextAdoptionCapture;
            summary = JsonSerializer.Serialize(new {
                captureId = next.ToString(CultureInfo.InvariantCulture), ownerGuid = capture.OwnerGuid, savedScriptId = capture.SavedScriptId,
                sourceBirth = capture.SourceBirth, sourceGeneration = generation.ToString(CultureInfo.InvariantCulture),
                sourceInstanceId = instanceId.ToString(CultureInfo.InvariantCulture), sourceSceneLifetime = capture.SourceSceneLifetime.ToString(CultureInfo.InvariantCulture),
                typeKey = capture.Value.TypeKey, capturedRevision = capture.Value.Revision, sourceRevision = capture.Value.SourceRevision,
                pathCount = capture.Value.Leaves.Count, referenceCount = capture.References.Count, sample = "captured_play"
            });
            m_adoptionCaptures.Add(next, capture); pendingCapture = null;
            if (next == ulong.MaxValue) m_adoptionIdsExhausted = true; else m_nextAdoptionCapture++;
            captureId = next; return new(BridgeStatus.OK);
        } catch (Exception exception) { pending?.Dispose(); pendingCapture?.Dispose(); return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }

    public ManagedRuntimeResult ReadSettingsAdoption(ulong captureId, string input, out string page)
    {
        page = "";
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (!m_adoptionCaptures.TryGetValue(captureId, out AdoptionCapture? capture)) return new(BridgeStatus.STALE_GENERATION, "Adoption capture was closed or belongs to another Host session.");
        try {
            using JsonDocument request = JsonDocument.Parse(input); JsonElement root = request.RootElement;
            Keys(root, "action", "leafIndex", "path", "start", "count", "packetBytes", "items", "final");
            string action = root.TryGetProperty("action", out JsonElement selectedAction) ? selectedAction.GetString() ?? "" : "read";
            long start = root.TryGetProperty("start", out JsonElement offset) ? offset.GetInt64() : 0;
            int count = root.TryGetProperty("count", out JsonElement size) ? size.GetInt32() : 64;
            int capacity = root.TryGetProperty("packetBytes", out JsonElement bytes) ? bytes.GetInt32() : 65536;
            if (action == "summary") {
                page = JsonSerializer.Serialize(new { ownerGuid = capture.OwnerGuid, savedScriptId = capture.SavedScriptId, sourceBirth = capture.SourceBirth,
                    sourceGeneration = capture.SourceGeneration.ToString(CultureInfo.InvariantCulture), sourceInstanceId = capture.SourceInstance.ToString(CultureInfo.InvariantCulture),
                    sourceSceneLifetime = capture.SourceSceneLifetime.ToString(CultureInfo.InvariantCulture), typeKey = capture.Value.TypeKey,
                    capturedRevision = capture.Value.Revision, sourceRevision = capture.Value.SourceRevision, pathCount = capture.Value.Leaves.Count,
                    referenceCount = capture.References.Count, proofComplete = capture.ProofComplete, sample = "captured_play" });
                return new(BridgeStatus.OK);
            }
            if (action == "references") { page = JsonSerializer.Serialize(capture.References.ReadPage(start, count, capacity)); return new(BridgeStatus.OK); }
            if (action == "paths") {
                if (start < 0 || start > capture.Value.Leaves.Count || count < 0 || capacity < 1024) throw new InvalidDataException("Adoption path page bounds are invalid.");
                List<object> items = new(); int remaining = capacity - 256;
                for (long index = start; index < capture.Value.Leaves.Count && items.Count < count; index++) {
                    var leaf = capture.Value.Leaves[(int)index]; object item = new { index, path = leaf.Path, kind = leaf.Value.RootKind, total = leaf.Value.TotalCount };
                    int sizeBytes = JsonSerializer.SerializeToUtf8Bytes(item).Length;
                    if (sizeBytes + 1 > remaining) break;
                    items.Add(item); remaining -= sizeBytes + 1;
                }
                if (items.Count == 0 && start < capture.Value.Leaves.Count && count != 0) throw new InvalidDataException("Increase path review packet capacity.");
                page = JsonSerializer.Serialize(new { total = capture.Value.Leaves.Count, start, returned = items.Count, nextStart = start + items.Count < capture.Value.Leaves.Count ? (long?)(start + items.Count) : null, items });
                if (Encoding.UTF8.GetByteCount(page) > capacity) throw new InvalidDataException("Increase path review packet capacity for its envelope.");
                return new(BridgeStatus.OK);
            }
            if (action == "map") {
                JsonElement items = root.GetProperty("items"); bool final = root.GetProperty("final").GetBoolean();
                if (items.ValueKind != JsonValueKind.Array || start < 0 || start > capture.Proofs.Count || items.GetArrayLength() > capture.References.Count - start)
                    throw new InvalidDataException("Reference proof chunk bounds are invalid.");
                long position = start;
                foreach (JsonElement item in items.EnumerateArray()) {
                    Keys(item, "path", "source", "target"); JsonElement expected = capture.References.Read(position);
                    if (item.GetProperty("path").GetString() != expected.GetProperty("path").GetString() || !JsonElement.DeepEquals(item.GetProperty("source"), expected.GetProperty("identity")))
                        throw new InvalidDataException("Reference proof does not match its immutable source token/path.");
                    if (position < capture.Proofs.Count) { if (!JsonElement.DeepEquals(item, capture.Proofs.Read(position))) throw new InvalidDataException("Reference proof replay changed retained bytes."); }
                    else { if (capture.ProofComplete) throw new InvalidDataException("Completed reference proof is immutable."); capture.Proofs.Append(item); }
                    position++;
                }
                if (final && position != capture.References.Count) throw new InvalidDataException("Reference proof is incomplete.");
                if (final) capture.ProofComplete = true;
                page = JsonSerializer.Serialize(new { received = capture.Proofs.Count, complete = capture.ProofComplete }); return new(BridgeStatus.OK);
            }
            if (action != "read") throw new InvalidDataException("Unknown adoption management action.");
            int leafIndex = root.GetProperty("leafIndex").GetInt32(); string path = root.TryGetProperty("path", out JsonElement selected) ? selected.GetString() ?? "" : "";
            page = JsonSerializer.Serialize(capture.Value.ReadPage(leafIndex, path, start, count, capacity));
            return new(BridgeStatus.OK);
        } catch (Exception exception) { return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }
    public ManagedRuntimeResult CloseSettingsAdoption(ulong captureId)
    {
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (m_adoptionCaptures.Remove(captureId, out AdoptionCapture? capture)) capture.Dispose();
        return new(BridgeStatus.OK);
    }
    /// @note Source snapshots stay reviewable on validation failure; successful prepare still publishes no live destination fields.
    public ManagedRuntimeResult PrepareSettingsAdoption(ulong generation, string input, out ulong preparedToken)
    {
        preparedToken = 0;
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        try {
            using JsonDocument request = JsonDocument.Parse(input); Keys(request.RootElement, "targets");
            JsonElement selected = request.RootElement.GetProperty("targets");
            if (selected.ValueKind != JsonValueKind.Array || selected.GetArrayLength() == 0) throw new InvalidDataException("Select explicit original Edit targets for adoption.");
            using ImmutableValueSnapshot.SnapshotStream batch = new(new());
            using (Utf8JsonWriter writer = new(batch)) {
                writer.WriteStartObject(); writer.WritePropertyName("targets"); writer.WriteStartArray();
                foreach (JsonElement target in selected.EnumerateArray()) {
                    Keys(target, "instanceId", "expectedRevision", "scriptBirth", "captureId");
                    ulong id = Id(target.GetProperty("instanceId")), captureId = Id(target.GetProperty("captureId"));
                    if (!TryInstance(generation, id, out ScriptInstance? destination)) throw new InvalidDataException("Original destination instance has expired.");
                    if (!m_adoptionCaptures.TryGetValue(captureId, out AdoptionCapture? capture)) throw new InvalidDataException("Captured Play settings were closed.");
                    if (destination.Binding.GameObject.Guid != capture.OwnerGuid || destination.Binding.SavedScriptId != capture.SavedScriptId ||
                        destination.Binding.ScriptBirth != target.GetProperty("scriptBirth").GetString()) throw new InvalidDataException("ADOPTION_BIRTH_CHANGED: the original saved target was removed/readded.");
                    if (!capture.ProofComplete) throw new InvalidDataException("Complete the initial-birth reference proof before preparing adoption; the capture remains reviewable.");
                    capture.Value.ValidateDestination(destination.Type, destination.Instance);
                    using ImmutableValueSnapshot.SnapshotStream values = new(new()); Dictionary<string, object?> sourceRefs = new(StringComparer.Ordinal);
                    using (Utf8JsonWriter patchWriter = new(values)) capture.Value.WritePatches(patchWriter, sourceRefs);
                    values.Position = 0; using JsonDocument patches = JsonDocument.Parse(values, new JsonDocumentOptions { MaxDepth = StructuredFieldCodec.MAX_DEPTH + 8 });
                    Dictionary<string, JsonElement> source = new(StringComparer.Ordinal), mappings = new(StringComparer.Ordinal);
                    for (long index = 0; index < capture.References.Count; index++) {
                        JsonElement reference = capture.References.Read(index), proof = capture.Proofs.Read(index);
                        string path = reference.GetProperty("path").GetString()!;
                        source.Add(path, reference.GetProperty("identity"));
                        mappings.Add(path, JsonSerializer.SerializeToElement(new { source = proof.GetProperty("source"), target = proof.GetProperty("target") }));
                    }
                    Dictionary<string, SettingsReferenceSnapshot.Identity> exact = new(StringComparer.Ordinal);
                    foreach (JsonElement patch in patches.RootElement.EnumerateArray()) {
                        string root = patch.GetProperty("path").GetString()!;
                        var location = SettingsLeafChanges.Locate(destination.Type, destination.Instance, root);
                        bool Within(string path) => path == root || path.StartsWith(root + "/", StringComparison.Ordinal);
                        var imported = RebindImportedReferenceTable(destination, patch.GetProperty("value"), location.ValueType, root,
                            JsonSerializer.SerializeToElement(source.Where(pair => Within(pair.Key)).ToDictionary(pair => pair.Key, pair => pair.Value, StringComparer.Ordinal)),
                            JsonSerializer.SerializeToElement(mappings.Where(pair => Within(pair.Key)).ToDictionary(pair => pair.Key, pair => pair.Value, StringComparer.Ordinal)));
                        foreach (var pair in imported) exact.Add(pair.Key, pair.Value);
                    }
                    writer.WriteStartObject(); writer.WriteString("instanceId", id.ToString(CultureInfo.InvariantCulture));
                    writer.WriteString("expectedRevision", target.GetProperty("expectedRevision").GetString());
                    writer.WritePropertyName("fields"); writer.WriteStartObject(); writer.WriteEndObject();
                    writer.WritePropertyName("patches"); patches.RootElement.WriteTo(writer);
                    writer.WritePropertyName("exactReferences"); JsonSerializer.Serialize(writer, exact); writer.WriteEndObject();
                }
                writer.WriteEndArray(); writer.WriteEndObject();
            }
            batch.Position = 0; using JsonDocument candidate = JsonDocument.Parse(batch, new JsonDocumentOptions { MaxDepth = StructuredFieldCodec.MAX_DEPTH + 16 });
            return PrepareSettingsDocument(generation, candidate.RootElement, out preparedToken);
        } catch (Exception exception) { return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }
    void CloseSettingsAdoptions() { foreach (AdoptionCapture capture in m_adoptionCaptures.Values) capture.Dispose(); m_adoptionCaptures.Clear(); }
}
