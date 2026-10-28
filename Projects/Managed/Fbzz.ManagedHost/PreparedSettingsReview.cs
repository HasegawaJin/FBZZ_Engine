/// @file    PreparedSettingsReview.cs
/// @brief   完全な候補と Undo を保持したまま user review だけを不変の範囲応答へ投影する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;
using Fbzz;

namespace Fbzz.ManagedHost;

public sealed partial class ManagedScriptRuntime
{
    /// @note wireBytes は全 target・change・参照を含む応答全体の byte 予算。保存値の総容量を制限しない。
    public ManagedRuntimeResult ReadPreparedReview(ulong generation, ulong token, string optionsJson, out string json)
    {
        json = "";
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (!m_settingsTransactions.TryGetValue(token, out SettingsTransaction? transaction) || transaction.Generation != generation)
            return new(BridgeStatus.STALE_GENERATION, "Prepared review was discarded or belongs to another generation.");
        try {
            using JsonDocument input = JsonDocument.Parse(optionsJson);
            JsonElement options = input.RootElement;
            string mode = options.TryGetProperty("mode", out JsonElement choice) ? choice.GetString() ?? "targets" : "targets";
            int wireBytes = ReviewNumber(options, "wireBytes", 8192, minimum: 2048);
            Dictionary<string, object?> result = new() {
                ["sample"] = "prepared_snapshot", ["totalTargets"] = transaction.Targets.Length,
                ["applied"] = transaction.Applied, ["everApplied"] = transaction.EverApplied
            };
            if (mode == "plan") {
                Keys(options, "mode", "targetOffset", "targetCount", "wireBytes");
                int planFirst = ReviewNumber(options, "targetOffset", 0), planLimit = ReviewNumber(options, "targetCount", 16, minimum: 1);
                if (planFirst > transaction.Targets.Length) throw new InvalidDataException("Prepared target page starts beyond its total count.");
                List<object?> planRows = new();
                int planNext = planFirst;
                for (int index = planFirst; index < transaction.Targets.Length && index - planFirst < planLimit; index++) {
                    SettingsTarget target = transaction.Targets[index];
                    Dictionary<string, object?> row = ReviewMetadata(target, index);
                    row["instanceId"] = target.Id.ToString(CultureInfo.InvariantCulture);
                    planRows.Add(row);
                    planNext = index + 1;
                    result["targets"] = planRows; result["targetOffset"] = planFirst;
                    result["returnedTargets"] = planRows.Count;
                    result["nextTargetOffset"] = planNext < transaction.Targets.Length ? planNext : null;
                    string packet = JsonSerializer.Serialize(result);
                    if (Encoding.UTF8.GetByteCount(packet) > wireBytes) {
                        planRows.RemoveAt(planRows.Count - 1); planNext = index;
                        result["returnedTargets"] = planRows.Count;
                        result["nextTargetOffset"] = planNext < transaction.Targets.Length ? planNext : null;
                        break;
                    }
                }
                result["targets"] = planRows; result["targetOffset"] = planFirst;
                result["returnedTargets"] = planRows.Count;
                result["nextTargetOffset"] = planNext < transaction.Targets.Length ? planNext : null;
                if (planRows.Count == 0 && planFirst < transaction.Targets.Length) return ReviewCapacity(JsonSerializer.Serialize(result), wireBytes);
                return ReviewPacket(result, wireBytes, out json);
            }
            if (mode == "validate") {
                Keys(options, "mode", "side", "deltaOnly", "wireBytes");
                string side = options.TryGetProperty("side", out JsonElement selected) ? selected.GetString() ?? "before" : "before";
                if (side is not ("before" or "after")) throw new InvalidDataException("Prepared validation side must be before or after.");
                bool deltaOnly = options.TryGetProperty("deltaOnly", out JsonElement delta) && delta.GetBoolean();
                string diagnostic = "";
                foreach (SettingsTarget target in transaction.Targets) {
                    if (deltaOnly && side == "before" && transaction.EverApplied && !transaction.Applied && target.Initial) continue;
                    ScriptInstance? record = target.Record;
                    if (record is null) { diagnostic = "SETTINGS_CONFLICT: prepared target instance is no longer available."; break; }
                    if (!deltaOnly) {
                        string expected = side == "after" ? target.AfterRevision : target.BeforeRevision;
                        if (CurrentSettingsHash(record) != expected) { diagnostic = "SETTINGS_CONFLICT: the full settings revision changed after preview."; break; }
                        continue;
                    }
                    foreach (SettingsLeafChanges.Change change in target.Changes) {
                        object? current;
                        try { current = SettingsLeafChanges.Get(change.Field.Read(record.Instance), change.Field.ValueType, change.Segments); }
                        catch (Exception exception) { diagnostic = Describe(exception); break; }
                        string expected = side == "after" ? change.AfterHash : change.BeforeHash;
                        if (SettingsLeafChanges.Hash(current, change.ValueType) != expected) {
                            diagnostic = $"SETTINGS_CONFLICT: {change.Path} changed after publication."; break;
                        }
                    }
                    if (diagnostic.Length != 0) break;
                }
                result["current"] = diagnostic.Length == 0;
                if (diagnostic.Length != 0) result["diagnostic"] = diagnostic;
                return ReviewPacket(result, wireBytes, out json);
            }
            if (mode == "value") {
                Keys(options, "mode", "targetIndex", "changeIndex", "side", "path", "start", "count", "wireBytes");
                int targetIndex = ReviewNumber(options, "targetIndex", 0), changeIndex = ReviewNumber(options, "changeIndex", 0);
                SettingsTarget target = ReviewTarget(transaction, targetIndex);
                if (changeIndex >= target.Changes.Length) throw new InvalidDataException("Prepared review change index is out of range.");
                string side = options.TryGetProperty("side", out JsonElement selected) ? selected.GetString() ?? "after" : "after";
                if (side is not ("before" or "after")) throw new InvalidDataException("Prepared review side must be before or after.");
                SettingsLeafChanges.Change change = target.Changes[changeIndex];
                string path = options.TryGetProperty("path", out JsonElement requested) ? requested.GetString() ?? change.Path : change.Path;
                if (path != change.Path && !path.StartsWith(change.Path + "/", StringComparison.Ordinal))
                    throw new InvalidDataException("Review path must be inside the selected changed subtree.");
                long start = options.TryGetProperty("start", out JsonElement offset) ? offset.GetInt64() : 0;
                int count = ReviewNumber(options, "count", 32, minimum: 1);
                if (start < 0) throw new InvalidDataException("Prepared review start must be nonnegative.");
                for (;;) {
                    Dictionary<string, object?> page = change.ReadReview(side == "after", path[change.Path.Length..], start, count, wireBytes - 512);
                    PrefixObservationPaths(page, change.Path);
                    result["targetIndex"] = targetIndex; result["changeIndex"] = changeIndex; result["side"] = side;
                    result["beforeRevision"] = target.BeforeRevision; result["afterRevision"] = target.AfterRevision;
                    result["page"] = page;
                    string packet = JsonSerializer.Serialize(result);
                    if (Encoding.UTF8.GetByteCount(packet) <= wireBytes) { json = packet; return new(BridgeStatus.OK); }
                    if (count == 1) return ReviewCapacity(packet, wireBytes);
                    count = Math.Max(1, count / 2);
                }
            }
            if (mode == "changes") {
                Keys(options, "mode", "targetIndex", "changeOffset", "changeCount", "wireBytes");
                int targetIndex = ReviewNumber(options, "targetIndex", 0);
                SettingsTarget target = ReviewTarget(transaction, targetIndex);
                int start = ReviewNumber(options, "changeOffset", 0), count = ReviewNumber(options, "changeCount", 16, minimum: 1);
                if (start > target.Changes.Length) throw new InvalidDataException("Prepared change page starts beyond its total count.");
                Dictionary<string, object?> row = ReviewMetadata(target, targetIndex);
                List<object?> changes = new(); row["changes"] = changes; row["changeOffset"] = start; row["returnedChanges"] = 0;
                result["target"] = row;
                long next = start;
                foreach (SettingsLeafChanges.Change change in target.Changes.AsSpan(start, Math.Min(count, target.Changes.Length - start))) {
                    changes.Add(ReviewChange(change, checked((int)next), wireBytes)); next++;
                    row["returnedChanges"] = changes.Count;
                    row["nextChangeOffset"] = next < target.Changes.Length ? next : null;
                    if (Encoding.UTF8.GetByteCount(JsonSerializer.Serialize(result)) > wireBytes) {
                        changes.RemoveAt(changes.Count - 1); next--; row["nextChangeOffset"] = next; row["returnedChanges"] = changes.Count;
                        break;
                    }
                }
                row["returnedChanges"] = changes.Count;
                row["nextChangeOffset"] = next < target.Changes.Length ? next : null;
                if (changes.Count == 0 && start < target.Changes.Length) return ReviewCapacity(JsonSerializer.Serialize(result), wireBytes);
                return ReviewPacket(result, wireBytes, out json);
            }
            if (mode != "targets") throw new InvalidDataException("Prepared review mode must be plan, validate, targets, changes or value.");
            Keys(options, "mode", "targetOffset", "targetCount", "changeOffset", "changeCount", "wireBytes");
            int first = ReviewNumber(options, "targetOffset", 0), limit = ReviewNumber(options, "targetCount", 16, minimum: 1);
            int changeStart = ReviewNumber(options, "changeOffset", 0), changeLimit = ReviewNumber(options, "changeCount", 16, minimum: 1);
            if (first > transaction.Targets.Length) throw new InvalidDataException("Prepared target page starts beyond its total count.");
            result["targetOffset"] = first;
            result["returnedTargets"] = 0;
            List<object?> rows = new(); result["targets"] = rows;
            int nextTarget = first;
            for (int index = first; index < transaction.Targets.Length && index - first < limit; index++) {
                SettingsTarget target = transaction.Targets[index];
                Dictionary<string, object?> row = ReviewMetadata(target, index);
                List<object?> changes = new(); row["changes"] = changes;
                int from = Math.Min(changeStart, target.Changes.Length), next = from;
                row["changeOffset"] = from; row["returnedChanges"] = 0;
                row["nextChangeOffset"] = next < target.Changes.Length ? next : null;
                rows.Add(row); result["returnedTargets"] = rows.Count; result["nextTargetOffset"] = index + 1 < transaction.Targets.Length ? index + 1 : null;
                if (Encoding.UTF8.GetByteCount(JsonSerializer.Serialize(result)) > wireBytes) { rows.RemoveAt(rows.Count - 1); result["returnedTargets"] = rows.Count; break; }
                for (int changeIndex = from; changeIndex < target.Changes.Length && changeIndex - from < changeLimit; changeIndex++) {
                    changes.Add(ReviewChange(target.Changes[changeIndex], changeIndex, wireBytes)); next++;
                    row["nextChangeOffset"] = next < target.Changes.Length ? next : null;
                    row["returnedChanges"] = changes.Count;
                    if (Encoding.UTF8.GetByteCount(JsonSerializer.Serialize(result)) > wireBytes) {
                        changes.RemoveAt(changes.Count - 1); next--; row["nextChangeOffset"] = next;
                        row["returnedChanges"] = changes.Count; break;
                    }
                }
                nextTarget = index + 1;
            }
            result["returnedTargets"] = rows.Count; result["nextTargetOffset"] = nextTarget < transaction.Targets.Length ? nextTarget : null;
            if (rows.Count == 0 && first < transaction.Targets.Length) return ReviewCapacity(JsonSerializer.Serialize(result), wireBytes);
            return ReviewPacket(result, wireBytes, out json);
        } catch (IOException exception) { return new(BridgeStatus.PENDING_LIMIT, Describe(exception)); }
        catch (Exception exception) { return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }
    static SettingsTarget ReviewTarget(SettingsTransaction transaction, int index) => index < transaction.Targets.Length
        ? transaction.Targets[index] : throw new InvalidDataException("Prepared target index is out of range.");
    static Dictionary<string, object?> ReviewMetadata(SettingsTarget target, int index) => new() {
        ["targetIndex"] = index, ["ownerGuid"] = target.OwnerGuid, ["savedScriptId"] = target.SavedScriptId,
        ["scriptType"] = target.Type.PersistentId, ["beforeRevision"] = target.BeforeRevision, ["afterRevision"] = target.AfterRevision,
        ["changedPathCount"] = target.Changes.Length, ["action"] = target.Initial ? "add" : "update"
    };
    static Dictionary<string, object?> ReviewChange(SettingsLeafChanges.Change change, int index, int wireBytes)
    {
        Dictionary<string, object?> before = change.ReadReviewValue(false, Math.Min(2048, wireBytes - 512));
        Dictionary<string, object?> after = change.ReadReviewValue(true, Math.Min(2048, wireBytes - 512));
        PrefixObservationPaths(before, change.Path); PrefixObservationPaths(after, change.Path);
        return new() { ["changeIndex"] = index, ["path"] = change.Path,
            ["before"] = before, ["after"] = after, ["beforeHash"] = change.BeforeHash, ["afterHash"] = change.AfterHash };
    }
    static int ReviewNumber(JsonElement options, string key, int fallback, int minimum = 0)
    {
        int number = options.TryGetProperty(key, out JsonElement value) ? value.GetInt32() : fallback;
        return number >= minimum ? number : throw new InvalidDataException($"Prepared review {key} must be at least {minimum}.");
    }
    static ManagedRuntimeResult ReviewPacket(Dictionary<string, object?> value, int capacity, out string json)
    {
        string packet = JsonSerializer.Serialize(value);
        if (Encoding.UTF8.GetByteCount(packet) > capacity) { json = ""; return ReviewCapacity(packet, capacity); }
        json = packet; return new(BridgeStatus.OK);
    }
    static ManagedRuntimeResult ReviewCapacity(string packet, int capacity)
        => new(BridgeStatus.PENDING_LIMIT, $"Prepared review metadata requires more than {capacity} wire bytes (measured {Encoding.UTF8.GetByteCount(packet)}); increase wireBytes or read a value at a deeper path.");
}
