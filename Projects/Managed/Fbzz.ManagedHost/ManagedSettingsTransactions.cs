/// @file    ManagedSettingsTransactions.cs
/// @brief   Detached setting candidates and atomic normalized snapshot publication.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Fbzz;

namespace Fbzz.ManagedHost;

public sealed partial class ManagedScriptRuntime
{
    const int MAX_SETTINGS_TRANSACTIONS = 256;
    readonly Dictionary<ulong, SettingsTransaction> m_settingsTransactions = new();
    ulong m_nextSettingsTransaction = 1;
    long m_settingsTransactionBytes;
    internal long SettingsTransactionBytes => m_settingsTransactionBytes;
    internal long SettingsTransactionStorageBytes => m_settingsTransactions.Values.Sum(transaction => transaction.Preview.StorageBytes
        + (transaction.CompactPreview?.StorageBytes ?? 0) + transaction.Targets.Sum(target => target.Changes.Sum(change => change.StorageBytes)));
    internal string CurrentSettingsHash(ScriptInstance record) => SettingsDocument.Hash(record.Type.Fields.Where(field => field.IsSetting).ToDictionary(field => field, field => field.Read(record.Instance)));
    internal int SettingsTransactionCount => m_settingsTransactions.Count;

    sealed class SettingsTarget
    {
        internal ScriptInstance? Record;
        internal required ScriptTypeDescriptor Type;
        internal required ulong OwnerId;
        internal required bool Initial;
        internal required ulong Id;
        internal required ulong Counter;
        internal required ulong SceneLifetime;
        internal required ulong ObjectId;
        internal required string OwnerGuid;
        internal required string SavedScriptId;
        internal required Dictionary<ScriptFieldDescriptor, object?> Before;
        internal required Dictionary<ScriptFieldDescriptor, object?> After;
        internal required string BeforeRevision;
        internal required string AfterRevision;
        internal required SettingsLeafChanges.Change[] Changes;
    }
    sealed class SettingsTransaction
    {
        internal required ulong Generation;
        internal required SettingsTarget[] Targets;
        internal required ImmutableValueSnapshot Preview;
        internal required long Bytes;
        internal ImmutableValueSnapshot? CompactPreview;
        internal required long CompactBytes;
        internal bool Applied;
        internal bool EverApplied;
    }

    /// @note Existing edits clone settings without constructors. Reset runs only for a newly added instance; Engine scope is absent during candidate callbacks.
    public ManagedRuntimeResult PrepareSettings(ulong generation, string json, out ulong token)
    {
        token = 0;
        try { using JsonDocument document = JsonDocument.Parse(json); return PrepareSettingsDocument(generation, document.RootElement, out token); }
        catch (Exception exception) { return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }
    internal ManagedRuntimeResult PrepareSettingsDocument(ulong generation, JsonElement root, out ulong token)
    {
        token = 0;
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (m_settingsTransactions.Count >= MAX_SETTINGS_TRANSACTIONS) return new(BridgeStatus.PENDING_LIMIT, "Discard old setting previews or Undo receipts before preparing more.");
        List<SettingsTarget> targets = new();
        ImmutableValueSnapshot? pendingPreview = null, pendingCompact = null;
        try {
            Keys(root, "targets");
            JsonElement requests = root.GetProperty("targets");
            if (requests.ValueKind != JsonValueKind.Array || requests.GetArrayLength() == 0) throw new InvalidDataException("A settings transaction requires a nonempty batch of exact targets.");
            HashSet<ulong> ids = new();
            long bytes = 0;
            foreach (JsonElement request in requests.EnumerateArray()) {
                Keys(request, "instanceId", "expectedRevision", "editSession", "fields", "patches", "rebind", "moves", "initial", "action", "exactReferences");
                ulong id = Id(request.GetProperty("instanceId"));
                if (!ids.Add(id)) throw new InvalidDataException("Duplicate transaction target.");
                if (!TryInstance(generation, id, out ScriptInstance? record)) return new(BridgeStatus.STALE_GENERATION);
                if (request.TryGetProperty("editSession", out JsonElement editSession)) {
                    if (!TryEdit(generation, Id(editSession), out SettingsEditSession? selectedEdit) || selectedEdit.Id != id ||
                        selectedEdit.Failed || selectedEdit.Prepared != 0 || request.GetProperty("expectedRevision").GetString() != selectedEdit.Revision)
                        throw new InvalidDataException("The batch edit session must match its exact document and starting revision.");
                }
                Dictionary<ScriptFieldDescriptor, object?> before = CaptureSettings(record);
                string beforeRevision = SettingsHash(before);
                if (request.GetProperty("expectedRevision").GetString() != beforeRevision) return new(BridgeStatus.INVALID_ARGUMENT, "SETTINGS_CONFLICT: refresh the current settings snapshot.");
                Script candidate = (Script)RuntimeHelpers.GetUninitializedObject(record.Type.Type);
                AssignSettings(candidate, CopySettings(before));
                bool initial = request.TryGetProperty("initial", out JsonElement initialFlag) && initialFlag.GetBoolean();
                if (initial) InvokeCandidate(record, candidate, ScriptCallback.RESET);
                JsonElement fields = request.TryGetProperty("fields", out JsonElement explicitFields) ? explicitFields : JsonSerializer.SerializeToElement(new Dictionary<string, object?>());
                HashSet<string> structural = ApplyCandidate(record, candidate, fields, request);
                if (request.TryGetProperty("action", out JsonElement action)) {
                    if (initial) throw new InvalidDataException("Editor actions require an existing Script; add and configure it before invoking an action.");
                    ManagedEditorActions.Invoke(record.Type, candidate, action.GetString() ?? "");
                }
                InvokeCandidate(record, candidate, ScriptCallback.ON_VALIDATE);
                Dictionary<ScriptFieldDescriptor, object?> after = CaptureSettings(record, candidate);
                foreach ((ScriptFieldDescriptor field, object? value) in after)
                    if (!field.Validate(value) || (field.Kind == ScriptFieldKind.STRING && value is null))
                        throw new InvalidDataException($"CS_SETTING_VALUE {record.Type.TypeName}.{field.Key}: normalized settings are invalid.");
                SettingsLeafChanges.Change[] changes = SettingsLeafChanges.Diff(before, after, initial, structural);
                string afterRevision = SettingsHash(after);
                targets.Add(new SettingsTarget {
                    Record = record, Type = record.Type, OwnerId = record.OwnerId, Initial = initial, Id = id, Counter = record.SettingsRevision, SceneLifetime = record.Binding.GameObject.SceneLifetime,
                    ObjectId = record.Binding.GameObject.ObjectId, OwnerGuid = record.Binding.GameObject.Guid, SavedScriptId = record.Binding.SavedScriptId, Before = before, After = after,
                    BeforeRevision = beforeRevision, AfterRevision = afterRevision, Changes = changes
                });
            }
            string compactPreview = JsonSerializer.Serialize(new {
                compact = true, targets = targets.Select(target => new {
                    instanceId = target.Id.ToString(CultureInfo.InvariantCulture),
                    ownerGuid = target.OwnerGuid, savedScriptId = target.SavedScriptId, sourceInstanceId = target.Id.ToString(CultureInfo.InvariantCulture),
                    beforeRevision = target.BeforeRevision, afterRevision = target.AfterRevision,
                    changedPathCount = target.Changes.Length, action = target.Initial ? "add" : "update"
                }).ToArray()
            });
            long compactBytes = Encoding.UTF8.GetByteCount(compactPreview);
            foreach (SettingsTarget target in targets)
                foreach (SettingsLeafChanges.Change change in target.Changes)
                    compactBytes = checked(compactBytes + change.ResidentBytes + Encoding.UTF8.GetByteCount(change.Path));
            pendingPreview = ImmutableValueSnapshot.Capture(compactPreview, typeof(string));
            bytes = checked(pendingPreview.ResidentBytes + compactBytes);
            foreach (SettingsTarget target in targets) { target.Before.Clear(); target.After.Clear(); }
            token = m_nextSettingsTransaction++;
            m_settingsTransactions.Add(token, new SettingsTransaction { Generation = generation, Targets = targets.ToArray(), Preview = pendingPreview, Bytes = bytes, CompactPreview = pendingCompact, CompactBytes = compactBytes });
            m_settingsTransactionBytes += bytes;
            return new(BridgeStatus.OK);
        } catch (Exception exception) { token = 0; return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
        finally {
            if (token == 0) {
                pendingPreview?.Dispose(); pendingCompact?.Dispose();
                foreach (SettingsTarget target in targets) foreach (SettingsLeafChanges.Change change in target.Changes) change.Dispose();
            }
        }
    }

    public ManagedRuntimeResult ReadPreparedSettings(ulong generation, ulong token, out string preview)
    {
        preview = "";
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (!m_settingsTransactions.TryGetValue(token, out SettingsTransaction? transaction) || transaction.Generation != generation) return new(BridgeStatus.STALE_GENERATION);
        preview = (string)transaction.Preview.RestoreValue(typeof(string))!;
        return new(BridgeStatus.OK);
    }

    /// @note Commit/Undo/Redo never rerun callbacks. All targets are compared and cloned before the first live assignment.
    public ManagedRuntimeResult PublishSettings(ulong generation, ulong token, bool restore, string remapJson = "")
    {
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (!m_settingsTransactions.TryGetValue(token, out SettingsTransaction? transaction) || transaction.Generation != generation) return new(BridgeStatus.STALE_GENERATION);
        if (restore && !transaction.EverApplied) return new(BridgeStatus.INVALID_ARGUMENT, "The settings transaction was never committed.");
        if (transaction.Applied == !restore && transaction.EverApplied) return new(BridgeStatus.OK);
        try {
            Dictionary<ulong, (ulong Id, string Revision)> replacements = new();
            if (remapJson.Length != 0) {
                using JsonDocument remap = JsonDocument.Parse(remapJson);
                Keys(remap.RootElement, "replacements");
                foreach (JsonElement entry in remap.RootElement.GetProperty("replacements").EnumerateArray()) {
                    Keys(entry, "previousId", "instanceId", "expectedRevision");
                    if (restore || !replacements.TryAdd(Id(entry.GetProperty("previousId")), (Id(entry.GetProperty("instanceId")), entry.GetProperty("expectedRevision").GetString() ?? "")))
                        throw new InvalidDataException("Invalid or duplicate re-add replacement.");
                }
            }
            List<(SettingsTarget Target, ScriptInstance Record, ulong Id, Dictionary<ScriptFieldDescriptor, object?> Values, Dictionary<ScriptFieldDescriptor, object?> Rollback)> prepared = new();
            HashSet<ulong> destinationIds = new();
            foreach (SettingsTarget target in transaction.Targets) {
                bool replaced = replacements.TryGetValue(target.Id, out var replacement);
                ulong currentId = replaced ? replacement.Id : target.Id;
                if (!destinationIds.Add(currentId)) return new(BridgeStatus.INVALID_ARGUMENT, "Duplicate settings publication target identity.");
                if (!TryInstance(generation, currentId, out ScriptInstance? record)
                    || (replaced ? !target.Initial || target.Record is not null || !ReferenceEquals(record.Type, target.Type) : !ReferenceEquals(record, target.Record))
                    || record.Binding.GameObject.SceneLifetime != target.SceneLifetime || record.Binding.GameObject.ObjectId != target.ObjectId
                    || record.Binding.GameObject.Guid != target.OwnerGuid || record.Binding.SavedScriptId != target.SavedScriptId)
                    return new(BridgeStatus.STALE_GENERATION, "The settings target identity changed.");
                Dictionary<ScriptFieldDescriptor, object?> live = CaptureSettings(record);
                if (replaced) {
                    if (SettingsHash(live) != replacement.Revision) return new(BridgeStatus.INVALID_ARGUMENT, "SETTINGS_CONFLICT: the re-add candidate changed.");
                } else if (!transaction.EverApplied) {
                    if (record.SettingsRevision != target.Counter || SettingsHash(live) != target.BeforeRevision)
                        return new(BridgeStatus.INVALID_ARGUMENT, "SETTINGS_CONFLICT: the settings changed after preview.");
                }
                Dictionary<ScriptFieldDescriptor, object?> values = SettingsLeafChanges.Merge(live, target.Changes, restore, check: transaction.EverApplied && !replaced);
                SettingsDocument.Validate(values, target.Type.TypeName);
                prepared.Add((target, record, currentId, values, live));
            }
            if (replacements.Keys.Any(id => !transaction.Targets.Any(target => target.Id == id))) throw new InvalidDataException("Unknown re-add target.");
            int written = 0;
            try {
                foreach (var item in prepared) { AssignSettings(item.Record.Instance, item.Values); written++; }
            } catch {
                for (int index = 0; index <= written && index < prepared.Count; index++) AssignSettings(prepared[index].Record.Instance, prepared[index].Rollback);
                throw;
            }
            foreach (var item in prepared) {
                if (item.Values.Count != 0) item.Record.SettingsRevision++;
                item.Target.Record = item.Record;
                item.Target.Id = item.Id;
            }
            transaction.Applied = !restore;
            transaction.EverApplied = true;
            CompactSettingsTransaction(transaction);
            return new(BridgeStatus.OK);
        } catch (Exception exception) { return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }

    public ManagedRuntimeResult DiscardSettings(ulong generation, ulong token)
    {
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (m_settingsTransactions.TryGetValue(token, out SettingsTransaction? transaction) && transaction.Generation == generation) {
            m_settingsTransactionBytes -= transaction.Bytes;
            transaction.Preview.Dispose();
            transaction.CompactPreview?.Dispose();
            foreach (SettingsTarget target in transaction.Targets) foreach (SettingsLeafChanges.Change change in target.Changes) change.Dispose();
            m_settingsTransactions.Remove(token);
        }
        return new(BridgeStatus.OK);
    }
    void DiscardGenerationSettings(ulong generation)
    {
        foreach (ulong token in m_settingsTransactions.Where(pair => pair.Value.Generation == generation).Select(pair => pair.Key).ToArray()) DiscardSettings(generation, token);
    }
    void CompactSettingsTransaction(SettingsTransaction transaction)
    {
        if (transaction.CompactPreview is null) return;
        foreach (SettingsTarget target in transaction.Targets) { target.Before.Clear(); target.After.Clear(); }
        transaction.Preview.Dispose();
        transaction.Preview = transaction.CompactPreview;
        transaction.CompactPreview = null;
        m_settingsTransactionBytes -= transaction.Bytes;
        transaction.Bytes = transaction.CompactBytes;
        m_settingsTransactionBytes += transaction.Bytes;
    }

    static void InvokeCandidate(ScriptInstance record, Script candidate, ScriptCallback callback)
        => SettingsDocument.Invoke(record.Type, candidate, callback);
    static Dictionary<ScriptFieldDescriptor, object?> CaptureSettings(ScriptInstance record, Script? instance = null)
        => SettingsDocument.Capture(record.Type, instance ?? record.Instance);
    static Dictionary<ScriptFieldDescriptor, object?> CopySettings(Dictionary<ScriptFieldDescriptor, object?> values)
        => SettingsDocument.Copy(values);
    static void AssignSettings(Script instance, Dictionary<ScriptFieldDescriptor, object?> values) => SettingsDocument.Assign(instance, values);
    internal static string SettingsHash(Dictionary<ScriptFieldDescriptor, object?> values) => SettingsDocument.Hash(values);
    static string ValueHash(ScriptFieldDescriptor field, object? value) => SettingsDocument.FieldHash(field, value);
    static string SettingsJson(Dictionary<ScriptFieldDescriptor, object?> values) => SettingsDocument.Json(values);
    static Dictionary<string, SettingsReferenceSnapshot.Identity> ReferenceTable(Dictionary<ScriptFieldDescriptor, object?> values) => SettingsDocument.References(values);
    static ulong Id(JsonElement json) => json.ValueKind == JsonValueKind.String && ulong.TryParse(json.GetString(), NumberStyles.None, CultureInfo.InvariantCulture, out ulong id) && id != 0
        ? id : throw new InvalidDataException("Identity tokens must be nonzero decimal strings.");
    static void Keys(JsonElement json, params string[] allowed)
    {
        if (json.ValueKind != JsonValueKind.Object) throw new InvalidDataException("Expected an object.");
        HashSet<string> keys = new(StringComparer.Ordinal);
        foreach (JsonProperty property in json.EnumerateObject())
            if (!keys.Add(property.Name) || !allowed.Contains(property.Name, StringComparer.Ordinal)) throw new InvalidDataException("Unknown or duplicate transaction key.");
    }
    HashSet<string> ApplyCandidate(ScriptInstance record, Script candidate, JsonElement fields, JsonElement request)
    {
            if (fields.ValueKind != JsonValueKind.Object) throw new InvalidDataException("Settings delta must be an object.");
        Dictionary<string, GameObject> retained = new(StringComparer.Ordinal);
        Dictionary<string, SettingsReferenceSnapshot.Identity> imported = new(StringComparer.Ordinal);
        HashSet<string> importedUsed = new(StringComparer.Ordinal);
        if (request.TryGetProperty("exactReferences", out JsonElement exact)) {
            if (exact.ValueKind != JsonValueKind.Object) throw new InvalidDataException("Exact imported references require an object.");
            foreach (JsonProperty property in exact.EnumerateObject())
                if (!imported.TryAdd(property.Name, ReadReferenceIdentity(property.Value))) throw new InvalidDataException("Duplicate imported reference path.");
        }
        Dictionary<string, ComponentReferenceIdentity> retainedComponents = new(StringComparer.Ordinal);
        foreach (ScriptFieldDescriptor field in record.Type.Fields.Where(field => field.IsSetting))
            foreach ((string path, ComponentReferenceIdentity reference) in ComponentReferenceCodec.Paths(field.Read(record.Instance), field.ValueType))
                retainedComponents.Add(StructuredFieldCodec.ChildPath("", field.Key) + path, reference);
        foreach (ScriptFieldDescriptor field in record.Type.Fields.Where(field => field.IsSetting))
            foreach ((string path, GameObject reference) in StructuredFieldCodec.ReferencePaths(field.Read(record.Instance), field.ValueType))
                retained.Add(StructuredFieldCodec.ChildPath("", field.Key) + path, reference);
        HashSet<string> rebind = new(StringComparer.Ordinal), used = new(StringComparer.Ordinal);
        Dictionary<string, string> moves = new(StringComparer.Ordinal);
        if (request.TryGetProperty("rebind", out JsonElement paths)) {
            if (paths.ValueKind != JsonValueKind.Array) throw new InvalidDataException("Reference rebind paths require an array.");
            foreach (JsonElement path in paths.EnumerateArray()) if (!rebind.Add(ReferencePath(path))) throw new InvalidDataException("Duplicate rebind path.");
        }
        if (request.TryGetProperty("moves", out JsonElement entries)) {
            if (entries.ValueKind != JsonValueKind.Array) throw new InvalidDataException("Reference moves require an array.");
            foreach (JsonElement entry in entries.EnumerateArray()) {
                Keys(entry, "fromPath", "toPath");
                string from = ReferencePath(entry.GetProperty("fromPath")), to = ReferencePath(entry.GetProperty("toPath"));
                if ((!retained.ContainsKey(from) && !retainedComponents.ContainsKey(from)) || rebind.Contains(to) || !moves.TryAdd(to, from)) throw new InvalidDataException("Reference moves require existing source paths and unique, non-rebound destination paths.");
            }
        }
        HashSet<string> structural = new(StringComparer.Ordinal);
        List<string> writtenPaths = new();
        void Unique(string path) {
            if (writtenPaths.Any(previous => path == previous || path.StartsWith(previous + "/", StringComparison.Ordinal) || previous.StartsWith(path + "/", StringComparison.Ordinal)))
                throw new InvalidDataException($"Duplicate or overlapping setting patch: {path}.");
            writtenPaths.Add(path);
        }
        GameObject Resolve(string fullPath, string guid) {
            used.Add(fullPath);
            if (imported.TryGetValue(fullPath, out SettingsReferenceSnapshot.Identity? token)) {
                GameObject exact = new(Number(token.objectId), Number(token.sceneLifetime), token.guid);
                if (token.kind is not null || guid != token.guid || !IsExactImportedGameObject(record, exact)) throw new InvalidDataException("Imported GameObject token no longer matches its live saved address.");
                importedUsed.Add(fullPath); return exact;
            }
            if (rebind.Contains(fullPath)) return ResolveGameObject(record, guid);
            if (moves.TryGetValue(fullPath, out string? source)) {
                if (!retained.TryGetValue(source, out GameObject? moved)) throw new InvalidDataException("Reference move target kind differs from its source.");
                if (moved.Guid != guid) throw new InvalidDataException($"Reference move GUID mismatch at {fullPath}.");
                return moved;
            }
            if (retained.TryGetValue(fullPath, out GameObject? previous) && previous.Guid == guid) return previous;
            GameObject[] matching = retained.Values.Where(reference => reference.Guid == guid).ToArray();
            if (matching.Length != 0) {
                GameObject first = matching[0];
                if (matching.Any(reference => reference.ObjectId != first.ObjectId || reference.SceneLifetime != first.SceneLifetime))
                    throw new InvalidDataException($"Ambiguous reference identity at {fullPath}; supply an explicit move or rebind.");
                return first;
            }
            return ResolveGameObject(record, guid);
        }
        ComponentReferenceIdentity ResolveComponent(string path, ComponentReferenceIdentity address, Type wrapper) {
            used.Add(path);
            if (imported.ContainsKey(path)) { importedUsed.Add(path); return ResolveExactImportedComponent(record, path, address, wrapper, imported); }
            if (rebind.Contains(path)) return ResolveComponentReferencePrepared(record, address, wrapper);
            if (moves.TryGetValue(path, out string? source)) {
                if (!retainedComponents.TryGetValue(source, out ComponentReferenceIdentity? moved) || !ComponentReferenceCodec.SameAddress(address, moved))
                    throw new InvalidDataException($"Component reference move address mismatch at {path}.");
                return moved;
            }
            if (retainedComponents.TryGetValue(path, out ComponentReferenceIdentity? previous) && ComponentReferenceCodec.SameAddress(address, previous)) return previous;
            ComponentReferenceIdentity[] matching = retainedComponents.Values.Where(value => ComponentReferenceCodec.SameAddress(value, address)).Distinct().ToArray();
            if (matching.Length > 1) throw new InvalidDataException($"Ambiguous Component lifetime at {path}; supply an explicit move or rebind.");
            return matching.Length == 1 ? matching[0] : ResolveComponentReferencePrepared(record, address, wrapper);
        }
        foreach (JsonProperty property in fields.EnumerateObject()) {
            ScriptFieldDescriptor? field = record.Type.Fields.FirstOrDefault(field => field.IsSetting && (field.Key == property.Name || field.PreviousKeys.Contains(property.Name)));
            if (field is null) throw new InvalidDataException($"Unknown setting: {property.Name}.");
            string rootPath = StructuredFieldCodec.ChildPath("", field.Key);
            Unique(rootPath);
            field.AssignPrepared(candidate, StructuredFieldCodec.DecodeValue(property.Value, field.ValueType, guid => ResolveGameObject(record, guid),
                (path, guid) => Resolve(rootPath + path, guid), validate: false,
                resolveComponent: (path, address, wrapper) => ResolveComponent(rootPath + path, address, wrapper)));
        }
        if (request.TryGetProperty("patches", out JsonElement patches)) {
            if (patches.ValueKind != JsonValueKind.Array) throw new InvalidDataException("Settings patches require an array.");
            SettingsPatchValue.ApplyBatch(record.Type, candidate, patches, (value, type, path) => StructuredFieldCodec.DecodeValue(value, type,
                guid => ResolveGameObject(record, guid), (child, guid) => Resolve(path + child, guid), validate: false,
                resolveComponent: (child, address, wrapper) => ResolveComponent(path + child, address, wrapper)), Unique, structural);
        }
        if (!rebind.IsSubsetOf(used) || moves.Keys.Any(path => !used.Contains(path)) || importedUsed.Count != imported.Count) throw new InvalidDataException("Reference rebind/move/import destinations must identify nonempty leaves in the delta.");
        return structural;
    }
    static string ReferencePath(JsonElement json)
    {
        string path = json.GetString() ?? "";
        if (!path.StartsWith("/", StringComparison.Ordinal) || path.Length > SettingsProtocolLimits.MAX_PATH_CHARACTERS) throw new InvalidDataException("Reference paths must be RFC 6901 leaf paths.");
        for (int index = 0; index < path.Length; index++) if (path[index] == '~' && (++index >= path.Length || path[index] is not ('0' or '1')))
            throw new InvalidDataException("Invalid RFC 6901 escape.");
        return path;
    }
}
