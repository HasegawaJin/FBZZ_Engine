/// @file    ManagedDataAssetDocuments.cs
/// @brief   DataAsset の世代付き owned document、分割転送、正規化候補と page を管理する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;

namespace Fbzz.ManagedHost;

public sealed partial class ManagedScriptRuntime
{
    readonly Dictionary<ulong, AssetDocument> m_assetDocuments = new();
    ulong m_nextAssetDocument = 1;
    bool m_assetDocumentIdsExhausted;
    internal int AssetDocumentCount => m_assetDocuments.Count;
    sealed class AssetDocument : IDisposable
    {
        internal required ulong Generation;
        internal required DataAssetTypeDescriptor Type;
        internal required string Guid;
        internal required bool Create;
        internal object? Current;
        internal string Revision = "";
        internal string AfterRevision = "";
        internal ImmutableValueSnapshot.SnapshotStream Input = new(new SnapshotStorageBudget());
        internal ImmutableValueSnapshot.SnapshotStream? Output;
        internal SettingsLeafChanges.Change[] Changes = [];
        internal bool Preparing;
        internal bool Complete;
        internal bool Failed;
        public void Dispose() { Input.Dispose(); Output?.Dispose(); foreach (var change in Changes) change.Dispose(); }
    }
    public ManagedRuntimeResult OpenDataAssetDocument(ulong generation, string request, out ulong id)
    {
        id = 0;
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (!m_generations.TryGetValue(generation, out ScriptGeneration? active)) return new(BridgeStatus.STALE_GENERATION);
        if (m_assetDocumentIdsExhausted || m_assetDocuments.Count >= 32) return new(BridgeStatus.PENDING_LIMIT, "Close unused DataAsset documents; the explicit owned-handle resource budget is full.");
        try {
            using JsonDocument input = JsonDocument.Parse(request); JsonElement root = input.RootElement;
            Keys(root, "type", "guid", "create");
            string type = root.GetProperty("type").GetString() ?? "", guid = root.GetProperty("guid").GetString() ?? "";
            if (!Fbzz.AssetReferenceGuid.Canonical(guid).StartsWith("guid:", StringComparison.Ordinal)) throw new InvalidDataException("DataAsset requires a stable GUID address.");
            DataAssetTypeDescriptor descriptor = active.Catalog.DataAssets.FirstOrDefault(value => value.PersistentId == type)
                ?? throw new InvalidDataException("DataAsset type is unavailable in this code generation.");
            PortableDataAssetDocument.ValidateSchema(descriptor);
            bool create = root.TryGetProperty("create", out JsonElement flag) && flag.GetBoolean();
            object? current = create ? SettingsDocument.InertValue(descriptor) : null;
            id = m_nextAssetDocument;
            if (id == ulong.MaxValue) m_assetDocumentIdsExhausted = true; else m_nextAssetDocument++;
            m_assetDocuments.Add(id, new AssetDocument { Generation = generation, Type = descriptor, Guid = guid, Create = create,
                Current = current, Revision = current is null ? "" : SettingsDocument.Hash(SettingsDocument.Capture(descriptor, current)) });
            return new(BridgeStatus.OK);
        } catch (Exception exception) { id = 0; return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }
    public ManagedRuntimeResult BeginDataAssetPreparation(ulong generation, ulong id)
    {
        if (!TryAssetDocument(generation, id, out AssetDocument? document)) return new(BridgeStatus.STALE_GENERATION);
        if (document.Current is null) return new(BridgeStatus.INVALID_ARGUMENT, "Finish loading the DataAsset source document first.");
        document.Input.Dispose(); document.Input = new(new SnapshotStorageBudget()); document.Preparing = true; document.Complete = false; document.Failed = false;
        return new(BridgeStatus.OK);
    }
    /// @note Sequential raw UTF8 chunks are caller-owned, at most 64KiB each; duplicate identical chunks are accepted without appending twice.
    public ManagedRuntimeResult StageDataAssetDocument(ulong generation, ulong id, long offset, ReadOnlySpan<byte> bytes)
    {
        if (!TryAssetDocument(generation, id, out AssetDocument? document)) return new(BridgeStatus.STALE_GENERATION);
        if (document.Failed || bytes.Length > 65536 || offset < 0) return new(BridgeStatus.INVALID_ARGUMENT, "DataAsset staging requires a live transfer, nonnegative offset and at most64KiB per packet.");
        try {
            if (offset < document.Input.Length || document.Complete) {
                if (offset > document.Input.Length || bytes.Length > document.Input.Length - offset) throw new InvalidDataException("DataAsset chunk replay bounds mismatch.");
                byte[] retained = new byte[bytes.Length]; document.Input.Position = offset; document.Input.ReadExactly(retained);
                if (!bytes.SequenceEqual(retained)) throw new InvalidDataException("DataAsset chunk replay changed retained bytes.");
                return new(BridgeStatus.OK);
            }
            if (offset != document.Input.Length) throw new InvalidDataException($"DataAsset chunk is out of sequence: expectedOffset={document.Input.Length}.");
            document.Input.Position = offset; document.Input.Write(bytes); return new(BridgeStatus.OK);
        } catch (Exception exception) { return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }
    public ManagedRuntimeResult FinishDataAssetDocument(ulong generation, ulong id, out string summary)
    {
        summary = "";
        if (!TryAssetDocument(generation, id, out AssetDocument? document)) return new(BridgeStatus.STALE_GENERATION);
        if (document.Complete) { summary = AssetSummary(document); return new(BridgeStatus.OK); }
        if (document.Failed) return new(BridgeStatus.INVALID_ARGUMENT, "Failed DataAsset input must be replaced by a new transfer.");
        ImmutableValueSnapshot.SnapshotStream? output = null; SettingsLeafChanges.Change[] changes = [];
        try {
            document.Input.Position = 0;
            using JsonDocument input = JsonDocument.Parse(document.Input, new JsonDocumentOptions { MaxDepth = StructuredFieldCodec.MAX_DEPTH + 8 });
            if (!document.Preparing) {
                object loaded = PortableDataAssetDocument.Read(document.Type, input.RootElement);
                string revision = SettingsDocument.Hash(SettingsDocument.Capture(document.Type, loaded));
                document.Current = loaded; document.Revision = revision;
            } else {
                JsonElement request = input.RootElement; Keys(request, "expectedRevision", "fields", "patches");
                if (request.GetProperty("expectedRevision").GetString() != document.Revision) throw new InvalidDataException("SETTINGS_CONFLICT: DataAsset logical revision changed.");
                JsonElement fields = request.TryGetProperty("fields", out JsonElement supplied) ? supplied : JsonSerializer.SerializeToElement(new Dictionary<string, object?>());
                request.TryGetProperty("patches", out JsonElement patches);
                var prepared = PortableDataAssetDocument.Prepare(document.Type, document.Create ? null : document.Current, fields, patches); changes = prepared.Changes;
                string afterRevision = SettingsDocument.Hash(prepared.After);
                output = new(new SnapshotStorageBudget());
                using (Utf8JsonWriter writer = new(output)) PortableDataAssetDocument.Write(writer, document.Type, prepared.After);
                foreach (var previous in document.Changes) previous.Dispose(); document.Output?.Dispose();
                document.Output = output; output = null; document.Changes = changes; changes = []; document.AfterRevision = afterRevision;
            }
            document.Complete = true; summary = AssetSummary(document); return new(BridgeStatus.OK);
        } catch (Exception exception) {
            document.Failed = true; return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception));
        } finally { output?.Dispose(); foreach (var change in changes) change.Dispose(); }
    }
    public ManagedRuntimeResult ReadDataAssetPage(ulong generation, ulong id, string request, out string page)
    {
        page = "";
        if (!TryAssetDocument(generation, id, out AssetDocument? document)) return new(BridgeStatus.STALE_GENERATION);
        if (document.Current is null) return new(BridgeStatus.INVALID_ARGUMENT, "DataAsset document is not loaded.");
        try {
            using JsonDocument input = JsonDocument.Parse(request); JsonElement root = input.RootElement; Keys(root, "path", "start", "count", "packetBytes");
            page = SettingsLivePages.Read(document.Type, document.Current, new() { ["generation"] = generation.ToString(CultureInfo.InvariantCulture),
                ["documentId"] = id.ToString(CultureInfo.InvariantCulture), ["revision"] = document.Revision, ["assetGuid"] = document.Guid },
                root.GetProperty("path").GetString() ?? "", root.TryGetProperty("start", out JsonElement start) ? start.GetInt64() : 0,
                root.TryGetProperty("count", out JsonElement count) ? count.GetInt32() : 64, root.TryGetProperty("packetBytes", out JsonElement bytes) ? bytes.GetInt32() : 65536);
            return new(BridgeStatus.OK);
        } catch (Exception exception) { return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }
    public ManagedRuntimeResult ReadPreparedDataAssetBytes(ulong generation, ulong id, long start, int count, out byte[] bytes, out long total)
    {
        bytes = []; total = 0;
        if (!TryAssetDocument(generation, id, out AssetDocument? document)) return new(BridgeStatus.STALE_GENERATION);
        if (document.Output is null || !document.Complete || document.Failed) return new(BridgeStatus.INVALID_ARGUMENT, "DataAsset candidate is unavailable.");
        total = document.Output.Length;
        if (start < 0 || start > total || count < 0 || count > 65536) return new(BridgeStatus.INVALID_ARGUMENT, "DataAsset output chunks require valid offsets and at most64KiB.");
        bytes = new byte[(int)Math.Min(count, total - start)]; document.Output.Position = start; document.Output.ReadExactly(bytes); return new(BridgeStatus.OK);
    }
    public ManagedRuntimeResult ReadPreparedDataAssetReview(ulong generation, ulong id, string request, out string review)
    {
        review = "";
        if (!TryAssetDocument(generation, id, out AssetDocument? document)) return new(BridgeStatus.STALE_GENERATION);
        if (document.Output is null || !document.Complete || document.Failed) return new(BridgeStatus.INVALID_ARGUMENT, "DataAsset candidate is unavailable.");
        try {
            using JsonDocument input = JsonDocument.Parse(request); JsonElement root = input.RootElement; Keys(root, "start", "count", "wireBytes", "change", "path", "after", "valueStart", "valueCount");
            int budget = root.TryGetProperty("wireBytes", out JsonElement wire) ? wire.GetInt32() : 8192;
            if (budget < 2048 || budget > 65536) throw new InvalidDataException("DataAsset review payload requires2048..65536bytes.");
            Dictionary<string, object?> result = new() { ["type"] = document.Type.PersistentId, ["revision"] = document.AfterRevision, ["changeCount"] = document.Changes.Length };
            if (root.TryGetProperty("change", out JsonElement selected)) {
                int index = selected.GetInt32(); if (index < 0 || index >= document.Changes.Length) throw new InvalidDataException("DataAsset review change is outside the retained snapshot.");
                bool after = !root.TryGetProperty("after", out JsonElement side) || side.GetBoolean();
                result["detail"] = document.Changes[index].ReadReview(after, root.TryGetProperty("path", out JsonElement path) ? path.GetString() ?? "" : "",
                    root.TryGetProperty("valueStart", out JsonElement start) ? start.GetInt64() : 0, root.TryGetProperty("valueCount", out JsonElement count) ? count.GetInt32() : 64, budget - 512);
            } else {
                int start = root.TryGetProperty("start", out JsonElement offset) ? offset.GetInt32() : 0, count = root.TryGetProperty("count", out JsonElement size) ? size.GetInt32() : 64;
                if (start < 0 || start > document.Changes.Length || count < 0) throw new InvalidDataException("DataAsset review bounds are invalid.");
                List<object?> changes = new();
                for (int index = start; index < document.Changes.Length && changes.Count < count; index++) {
                    changes.Add(new { index, path = document.Changes[index].Path }); result["changes"] = changes;
                    if (JsonSerializer.SerializeToUtf8Bytes(result).Length > budget - 128) { changes.RemoveAt(changes.Count - 1); break; }
                }
                result["changes"] = changes; result["start"] = start; result["returned"] = changes.Count; result["nextStart"] = start + changes.Count < document.Changes.Length ? start + changes.Count : null;
            }
            byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(result); if (bytes.Length > budget) throw new InvalidDataException("DataAsset review exceeds the aggregate JSON payload budget.");
            review = System.Text.Encoding.UTF8.GetString(bytes); return new(BridgeStatus.OK);
        } catch (Exception exception) { return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }
    public ManagedRuntimeResult CloseDataAssetDocument(ulong generation, ulong id)
    {
        if (!TryAssetDocument(generation, id, out AssetDocument? document)) return new(BridgeStatus.STALE_GENERATION);
        m_assetDocuments.Remove(id); document.Dispose(); return new(BridgeStatus.OK);
    }
    static string AssetSummary(AssetDocument value) => JsonSerializer.Serialize(new { type = value.Type.PersistentId, assetGuid = value.Guid,
        revision = value.Revision, afterRevision = value.AfterRevision, changeCount = value.Changes.Length, create = value.Create });
    bool TryAssetDocument(ulong generation, ulong id, [System.Diagnostics.CodeAnalysis.NotNullWhen(true)] out AssetDocument? document)
    {
        if (IsPreparationThread() && m_assetDocuments.TryGetValue(id, out document) && document.Generation == generation && m_generations.ContainsKey(generation)) return true;
        document = null; return false;
    }
    void CloseGenerationDataAssetDocuments(ulong generation)
    { foreach (ulong id in m_assetDocuments.Where(pair => pair.Value.Generation == generation).Select(pair => pair.Key).ToArray()) { m_assetDocuments[id].Dispose(); m_assetDocuments.Remove(id); } }
}
