/// @file    SettingsEditSessions.cs
/// @brief   Exact settings edit revisions with replay-checked chunk staging and atomic candidate preparation.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Fbzz.ManagedHost;

public sealed partial class ManagedScriptRuntime
{
    const int MAX_EDIT_SESSIONS = 32;
    internal const int EDIT_PACKET_BYTES = 65536;
    readonly Dictionary<ulong, SettingsEditSession> m_settingsEdits = new();
    ulong m_nextSettingsEdit = 1;
    bool m_editIdsExhausted;
    internal int SettingsEditSessionCount => m_settingsEdits.Count;
    internal long SettingsEditStorageBytes => m_settingsEdits.Values.Where(session => !session.Failed).Sum(session => session.Input.Length);
    sealed class SettingsEditSession : IDisposable
    {
        internal required ScriptInstance Record;
        internal required ulong Id;
        internal required string Revision;
        internal required ulong SceneLifetime;
        internal required ulong ObjectId;
        internal readonly ImmutableValueSnapshot.SnapshotStream Input = new(new SnapshotStorageBudget());
        internal bool Complete;
        internal ulong Prepared;
        internal bool Failed;
        public void Dispose() => Input.Dispose();
    }
    /// @note This full revision scan runs only when editing starts; ordinary page display does not call it.
    public ManagedRuntimeResult OpenSettingsEdit(ulong generation, ulong instanceId, out ulong sessionId, out string revision)
    {
        sessionId = 0; revision = "";
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (!TryInstance(generation, instanceId, out ScriptInstance? record)) return new(BridgeStatus.STALE_GENERATION);
        if (m_editIdsExhausted) return new(BridgeStatus.PENDING_LIMIT, "Settings edit handle space is exhausted; start a new Host session.");
        if (m_settingsEdits.Count >= MAX_EDIT_SESSIONS) return new(BridgeStatus.PENDING_LIMIT, "Close unused settings edit sessions; the retained session resource budget is full.");
        try {
            revision = CurrentSettingsHash(record); sessionId = m_nextSettingsEdit;
            if (sessionId == ulong.MaxValue) m_editIdsExhausted = true;
            else m_nextSettingsEdit++;
            m_settingsEdits.Add(sessionId, new SettingsEditSession { Record = record, Id = instanceId, Revision = revision,
                SceneLifetime = record.Binding.GameObject.SceneLifetime, ObjectId = record.Binding.GameObject.ObjectId });
            return new(BridgeStatus.OK);
        } catch (Exception exception) { sessionId = 0; return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }
    public ManagedRuntimeResult StageSettingsEdit(ulong generation, ulong sessionId, string packet)
    {
        if (!TryEdit(generation, sessionId, out SettingsEditSession? session)) return new(BridgeStatus.STALE_GENERATION);
        if (session.Failed) return new(BridgeStatus.INVALID_ARGUMENT, "The staged request failed; close this session and begin a new edit.");
        if (session.Prepared != 0) return new(BridgeStatus.INVALID_ARGUMENT, "Prepared edit input is immutable; close the session before starting another request.");
        try {
            if (Encoding.UTF8.GetByteCount(packet) > EDIT_PACKET_BYTES * 2) throw new InvalidDataException($"Settings chunk packet exceeds its explicit transport capacity of {EDIT_PACKET_BYTES} decoded bytes.");
            using JsonDocument document = JsonDocument.Parse(packet); JsonElement root = document.RootElement;
            Keys(root, "offset", "data", "sha256", "final");
            long offset = root.GetProperty("offset").GetInt64();
            byte[] bytes = Convert.FromBase64String(root.GetProperty("data").GetString() ?? "");
            if (bytes.Length > EDIT_PACKET_BYTES || offset < 0) throw new InvalidDataException("Settings chunk bounds exceed the allocated packet capacity.");
            string hash = Convert.ToHexString(SHA256.HashData(bytes));
            if (!string.Equals(hash, root.GetProperty("sha256").GetString(), StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Settings chunk checksum mismatch; resend the same offset and bytes.");
            bool final = root.GetProperty("final").GetBoolean();
            long end = checked(offset + bytes.Length);
            if (offset < session.Input.Length || session.Complete) {
                if (end > session.Input.Length || (final && end != session.Input.Length)) throw new InvalidDataException("Settings chunk replay bounds do not match retained input.");
                byte[] retained = new byte[bytes.Length]; session.Input.Position = offset; session.Input.ReadExactly(retained);
                if (!retained.AsSpan().SequenceEqual(bytes) || (final && !session.Complete)) throw new InvalidDataException("Settings chunk replay changed previously received bytes or finalization.");
                return new(BridgeStatus.OK);
            }
            if (offset != session.Input.Length) throw new InvalidDataException($"Settings chunk is out of sequence: expectedOffset={session.Input.Length}. No staged bytes were changed.");
            session.Input.Position = offset;
            try { session.Input.Write(bytes); session.Complete = final; }
            catch { session.Failed = true; session.Input.Dispose(); throw; }
            return new(BridgeStatus.OK);
        } catch (Exception exception) { return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }
    /// @note Complete staged input is parsed once. Replays return the same prepared token without repeating callbacks.
    public ManagedRuntimeResult PrepareSettingsEdit(ulong generation, ulong sessionId, out ulong preparedToken)
    {
        preparedToken = 0;
        if (!TryEdit(generation, sessionId, out SettingsEditSession? session)) return new(BridgeStatus.STALE_GENERATION);
        if (session.Failed || !session.Complete) return new(BridgeStatus.INVALID_ARGUMENT, "Settings input is not complete; finish all chunks before preparing.");
        if (session.Prepared != 0) {
            if (!m_settingsTransactions.ContainsKey(session.Prepared)) return new(BridgeStatus.STALE_GENERATION, "The prepared settings token was discarded; close this edit session.");
            preparedToken = session.Prepared; return new(BridgeStatus.OK);
        }
        try {
            session.Input.Position = 0;
            using JsonDocument document = JsonDocument.Parse(session.Input, new JsonDocumentOptions { MaxDepth = StructuredFieldCodec.MAX_DEPTH + 16 });
            JsonElement request = document.RootElement; Keys(request, "targets");
            JsonElement targets = request.GetProperty("targets");
            if (targets.ValueKind != JsonValueKind.Array || targets.GetArrayLength() == 0) throw new InvalidDataException("A staged settings batch requires explicit document targets.");
            int sessionMatches = 0;
            foreach (JsonElement target in targets.EnumerateArray()) {
                if (Id(target.GetProperty("instanceId")) != session.Id) continue;
                sessionMatches++;
                if (target.GetProperty("expectedRevision").GetString() != session.Revision ||
                    (target.TryGetProperty("initial", out JsonElement initial) && initial.GetBoolean()))
                    throw new InvalidDataException("Staged target must match the exact edit-session instance and starting revision.");
            }
            if (sessionMatches != 1) throw new InvalidDataException("The staging session document must occur exactly once in its atomic batch.");
            ManagedRuntimeResult result = PrepareSettingsDocument(generation, request, out preparedToken);
            if (result.Success) session.Prepared = preparedToken;
            else { session.Failed = true; session.Input.Dispose(); }
            return result;
        } catch (Exception exception) { session.Failed = true; session.Input.Dispose(); return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }
    public ManagedRuntimeResult CloseSettingsEdit(ulong generation, ulong sessionId, bool retainPrepared = false)
    {
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (m_settingsEdits.TryGetValue(sessionId, out SettingsEditSession? session) && session.Record.Generation == generation) {
            if (!retainPrepared && session.Prepared != 0 && m_settingsTransactions.TryGetValue(session.Prepared, out SettingsTransaction? prepared) && !prepared.EverApplied)
                DiscardSettings(generation, session.Prepared);
            m_settingsEdits.Remove(sessionId); session.Dispose();
        }
        return new(BridgeStatus.OK);
    }
    bool TryEdit(ulong generation, ulong id, [System.Diagnostics.CodeAnalysis.NotNullWhen(true)] out SettingsEditSession? session)
    {
        session = null;
        if (!IsPreparationThread() || !m_settingsEdits.TryGetValue(id, out SettingsEditSession? found)
            || !TryInstance(generation, found.Id, out ScriptInstance? record) || !ReferenceEquals(record, found.Record)
            || record.Binding.GameObject.SceneLifetime != found.SceneLifetime || record.Binding.GameObject.ObjectId != found.ObjectId) return false;
        session = found; return true;
    }
    void CloseOwnedSettingsEdits(ulong generation, ulong? instanceId = null)
    {
        foreach (ulong id in m_settingsEdits.Where(pair => pair.Value.Record.Generation == generation && (instanceId is null || pair.Value.Id == instanceId)).Select(pair => pair.Key).ToArray())
            CloseSettingsEdit(generation, id);
    }
}
