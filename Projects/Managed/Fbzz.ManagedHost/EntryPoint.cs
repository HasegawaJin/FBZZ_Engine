/// @file    EntryPoint.cs
/// @brief   常駐 Host の単一 C ABI 入口と native SDK サービスの接続。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Buffers;
using System.Collections.Generic;
using System.Buffers.Binary;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Fbzz;

namespace Fbzz.ManagedHost;

/// @note UCO 入口は常駐 Host に属し、ゲーム DLL の delegate を native に公開しない。
/// @see https://learn.microsoft.com/en-us/dotnet/core/tutorials/netcore-hosting load_assembly_and_get_function_pointer。
public static unsafe partial class EntryPoint
{
    static readonly object s_gate = new();
    static readonly UTF8Encoding s_utf8 = new(false, true);
    static ManagedScriptRuntime? s_runtime;
    static nint s_nativeAddress;

    [UnmanagedCallersOnly(CallConvs = new Type[] { typeof(CallConvCdecl) })]
    public static int Dispatch(BridgeRequest* request, BridgeReply* reply)
    {
        if (request is null || reply is null) return (int)BridgeStatus.INVALID_ARGUMENT;
        return DispatchManaged(ref *request, ref *reply);
    }

    /// @note managed テストも同じ境界処理を利用する。callback 全体を再送しない。
    public static int DispatchManaged(ref BridgeRequest request, ref BridgeReply reply)
    {
        lock (s_gate)
        {
            reply = new() { Generation = request.Generation, InstanceId = request.InstanceId, CallId = request.CallId };
            try
            {
                if (request.AbiVersion != BridgeAbi.VERSION || request.InputBytes > int.MaxValue || request.OutputCapacity > int.MaxValue
                    || (request.InputBytes != 0 && request.Input is null) || (request.OutputCapacity != 0 && request.Output is null))
                    reply.Status = BridgeStatus.ABI_MISMATCH;
                else if (request.Op == BridgeOp.INITIALIZE) Initialize(ref request, ref reply);
                else if (s_runtime is null) reply.Status = BridgeStatus.INVALID_SCOPE;
                else Execute(s_runtime, ref request, ref reply);
            }
            catch (Exception exception)
            {
                reply.Status = request.Op == BridgeOp.INVOKE ? BridgeStatus.CALLBACK_FAILED : BridgeStatus.INVALID_ARGUMENT;
                WriteMessage(ref request, ref reply, ManagedScriptRuntime.Describe(exception));
            }
            return (int)reply.Status;
        }
    }

    static void Initialize(ref BridgeRequest request, ref BridgeReply reply)
    {
        if (!BridgeAbi.HasExpectedLayout() || request.Generation != BridgeAbi.SCHEMA_HASH
            || request.Index != BridgeAbi.REQUEST_SIZE || request.Flags != BridgeAbi.REPLY_SIZE || request.Integer == 0)
        { reply.Status = BridgeStatus.ABI_MISMATCH; return; }
        nint address = unchecked((nint)request.Integer);
        if (s_runtime is not null && s_nativeAddress != address) { reply.Status = BridgeStatus.INVALID_SCOPE; return; }
        s_nativeAddress = address;
        ManagedDiagnostics.Initialize();
        s_runtime ??= new ManagedScriptRuntime(new NativeBridge(address));
        reply.Index = BridgeAbi.REQUEST_SIZE; reply.Flags = BridgeAbi.REPLY_SIZE; reply.Generation = BridgeAbi.SCHEMA_HASH;
    }

    static void Execute(ManagedScriptRuntime runtime, ref BridgeRequest request, ref BridgeReply reply)
    {
        switch (request.Op)
        {
            case BridgeOp.GET_API_CATALOG:
                WriteOutput(ref request, ref reply, ApiCatalog.Json);
                break;
            case BridgeOp.LOAD_CANDIDATE:
                Complete(runtime.LoadCandidate(ReadInput(request), request.Generation, (request.Flags & BridgeAbi.INTERNAL_MODE) != 0), ref request, ref reply);
                break;
            case BridgeOp.GET_TYPE:
                ScriptCatalog? catalog = runtime.GetCatalog(request.Generation);
                if (catalog is null) { reply.Status = BridgeStatus.STALE_GENERATION; break; }
                reply.Index = (uint)catalog.Types.Count;
                if (request.Index >= catalog.Types.Count) { reply.Status = BridgeStatus.NOT_FOUND; break; }
                ScriptTypeDescriptor type = catalog.Types[(int)request.Index];
                reply.Flags = type.ExecuteAlways ? 1U : 0U;
                if (request.Flags == 0) {
                    /// @note bit 1 は optional Gizmo metadata の対応印。旧 Host の Integer=0 と「両方なし」を区別する。
                    reply.Flags |= 2U | 4U;
                    /// @note bit 2 は optional 更新 metadata の対応印、bit 3/4/5 は Update/FixedUpdate/LateUpdate。旧 Gizmo Integer は変更しない。
                    foreach (ScriptCallbackDescriptor declared in type.Callbacks) {
                        if (declared.Callback == ScriptCallback.DRAW_GIZMOS) reply.Integer |= 1L;
                        else if (declared.Callback == ScriptCallback.DRAW_GIZMOS_SELECTED) reply.Integer |= 2L;
                        else if (declared.Callback == ScriptCallback.UPDATE) reply.Flags |= 8U;
                        else if (declared.Callback == ScriptCallback.FIXED_UPDATE) reply.Flags |= 16U;
                        else if (declared.Callback == ScriptCallback.LATE_UPDATE) reply.Flags |= 32U;
                    }
                }
                if (request.Flags == 4) reply.Integer = type.ExecutionOrder;
                WriteOutput(ref request, ref reply, request.Flags switch
                {
                    0 => type.PersistentId,
                    1 => type.TypeName,
                    2 => ScriptManifest.SourcesJson([new ScriptSourceEntry(type.TypeName, type.Sources)]),
                    3 => SettingsDocument.CatalogEntry(type),
                    4 => string.Join('\n', type.NativeRequiredComponentNames),
                    _ => throw new ArgumentException("Unknown type metadata selector.")
                });
                break;
            case BridgeOp.GET_DATA_ASSET_SCHEMA:
            case BridgeOp.OPEN_DATA_ASSET_DOCUMENT:
            case BridgeOp.READ_DATA_ASSET_PAGE:
            case BridgeOp.PREPARE_DATA_ASSET_DOCUMENT:
            case BridgeOp.READ_PREPARED_DATA_ASSET:
            case BridgeOp.CLOSE_DATA_ASSET_DOCUMENT:
                DataAssetOperation(runtime, ref request, ref reply); break;
            case BridgeOp.CREATE:
                string creation = ReadInput(request);
                int identitySeparator = creation.IndexOf('\n');
                if (identitySeparator <= 0 || identitySeparator != creation.LastIndexOf('\n'))
                { reply.Status = BridgeStatus.INVALID_ARGUMENT; break; }
                ManagedRuntimeResult create = runtime.CreateInstance(request.Generation, creation[..identitySeparator], request.OwnerId,
                    creation[(identitySeparator + 1)..], out ulong id);
                reply.InstanceId = id;
                Complete(create, ref request, ref reply);
                break;
            case BridgeOp.INVOKE:
                if (!Enum.IsDefined((ScriptCallback)request.Index)) { reply.Status = BridgeStatus.INVALID_ARGUMENT; break; }
                ScriptCallback callback = (ScriptCallback)request.Index;
                if (ScriptCatalogBuilder.IsEngineNotification(callback) && ScriptCatalogBuilder.CallbackArgument(callback) is null && request.InputBytes != 0)
                { reply.Status = BridgeStatus.INVALID_ARGUMENT; break; }
                ManagedRuntimeResult invoked = callback >= ScriptCallback.ON_COLLISION_ENTER && callback <= ScriptCallback.ON_TRIGGER_EXIT
                    ? runtime.TryInvokeCollision(request.Generation, request.InstanceId, request.OwnerId, request.CallId, callback, DecodeCollision(request))
                    : ScriptCatalogBuilder.IsEngineNotification(callback) && ScriptCatalogBuilder.CallbackArgument(callback) is not null
                    ? runtime.TryInvokeEngineNotification(request.Generation, request.InstanceId, request.OwnerId, request.CallId, callback, DecodeEngineNotification(request, callback))
                    : runtime.TryInvoke(request.Generation, request.InstanceId, request.OwnerId, request.CallId, callback);
                Complete(invoked, ref request, ref reply);
                break;
            case BridgeOp.GET_FIELD:
            case BridgeOp.READ_FIELD:
            case BridgeOp.WRITE_FIELD:
                FieldOperation(runtime, ref request, ref reply);
                break;
            case BridgeOp.GET_SETTINGS_SNAPSHOT:
                if (ScriptApiRuntime.Current is not null) { reply.Status = BridgeStatus.INVALID_SCOPE; break; }
                if (!runtime.TryInstance(request.Generation, request.InstanceId, out ManagedScriptRuntime.ScriptInstance? snapshotInstance))
                { reply.Status = BridgeStatus.STALE_GENERATION; break; }
                WriteOutput(ref request, ref reply, ManagedSettingsSnapshot.Read(snapshotInstance, request.Generation, request.InstanceId));
                break;
            case BridgeOp.GET_SETTINGS_REVISION:
                if (ScriptApiRuntime.Current is not null) { reply.Status = BridgeStatus.INVALID_SCOPE; break; }
                if (!runtime.TryInstance(request.Generation, request.InstanceId, out ManagedScriptRuntime.ScriptInstance? revisionInstance))
                { reply.Status = BridgeStatus.STALE_GENERATION; break; }
                reply.Integer = unchecked((long)revisionInstance.SettingsRevision);
                break;
            case BridgeOp.READ_SETTINGS_PAGE:
                ManagedRuntimeResult pageRead = runtime.ReadSettingsPage(request.Generation, request.InstanceId, ReadInput(request), out string settingsPage);
                Complete(pageRead, ref request, ref reply);
                if (pageRead.Success) WriteOutput(ref request, ref reply, settingsPage);
                break;
            case BridgeOp.OPEN_SETTINGS_EDIT:
                ManagedRuntimeResult openedEdit = runtime.OpenSettingsEdit(request.Generation, request.InstanceId, out ulong editId, out string editRevision);
                reply.Integer = unchecked((long)editId); Complete(openedEdit, ref request, ref reply);
                if (openedEdit.Success) WriteOutput(ref request, ref reply, editRevision);
                break;
            case BridgeOp.STAGE_SETTINGS_EDIT:
                Complete(runtime.StageSettingsEdit(request.Generation, unchecked((ulong)request.Integer), ReadInput(request)), ref request, ref reply);
                break;
            case BridgeOp.PREPARE_SETTINGS_EDIT:
                ManagedRuntimeResult stagedPrepared = runtime.PrepareSettingsEdit(request.Generation, unchecked((ulong)request.Integer), out ulong stagedToken);
                reply.Integer = unchecked((long)stagedToken); Complete(stagedPrepared, ref request, ref reply);
                break;
            case BridgeOp.CLOSE_SETTINGS_EDIT:
                bool retainPrepared = false;
                if (request.InputBytes != 0) { using JsonDocument closeInput = JsonDocument.Parse(ReadInput(request)); retainPrepared = closeInput.RootElement.GetProperty("retainPrepared").GetBoolean(); }
                Complete(runtime.CloseSettingsEdit(request.Generation, unchecked((ulong)request.Integer), retainPrepared), ref request, ref reply);
                break;
            case BridgeOp.OPEN_OBSERVATION:
                ManagedRuntimeResult opened = runtime.OpenObservation(request.Generation, request.InstanceId, ReadInput(request), out ulong observation);
                reply.Integer = unchecked((long)observation);
                Complete(opened, ref request, ref reply);
                break;
            case BridgeOp.READ_OBSERVATION:
                using (JsonDocument page = JsonDocument.Parse(ReadInput(request))) {
                    JsonElement options = page.RootElement;
                    ManagedRuntimeResult readObservation = runtime.ReadObservation(request.Generation, request.InstanceId, unchecked((ulong)request.Integer),
                        options.GetProperty("path").GetString() ?? "", options.GetProperty("start").GetInt64(), options.GetProperty("count").GetInt32(),
                        options.GetProperty("wireBytes").GetInt32(), out string observed);
                    if (readObservation.Success) WriteOutput(ref request, ref reply, observed);
                    else Complete(readObservation, ref request, ref reply);
                }
                break;
            case BridgeOp.CLOSE_OBSERVATION:
                Complete(runtime.CloseObservation(request.Generation, request.InstanceId, unchecked((ulong)request.Integer)), ref request, ref reply);
                break;
            case BridgeOp.DRAW_GIZMOS_QUERY:
                ManagedRuntimeResult draw = runtime.DrawGizmos(request.Generation, request.InstanceId, request.OwnerId, request.CallId,
                    (ScriptCallback)request.Index, checked((int)request.Flags), out byte[] commands);
                if (draw.Success) WriteBytes(ref request, ref reply, commands);
                else Complete(draw, ref request, ref reply);
                break;
            case BridgeOp.CAPTURE_SETTINGS_ADOPTION:
            {
                ManagedRuntimeResult adoptionCaptured = runtime.CaptureSettingsAdoption(request.Generation, request.InstanceId, ReadInput(request), out ulong captureId, out _);
                reply.Integer = unchecked((long)captureId); Complete(adoptionCaptured, ref request, ref reply); break;
            }
            case BridgeOp.PREPARE_SETTINGS_ADOPTION:
            {
                ManagedRuntimeResult imported = runtime.PrepareSettingsAdoption(request.Generation, ReadInput(request), out ulong adoptionToken);
                reply.Integer = unchecked((long)adoptionToken); Complete(imported, ref request, ref reply); break;
            }
            case BridgeOp.MANAGE_SETTINGS_ADOPTION:
            {
                string input = ReadInput(request); using JsonDocument document = JsonDocument.Parse(input);
                if (document.RootElement.TryGetProperty("action", out JsonElement action) && action.GetString() == "discard") {
                    Complete(runtime.CloseSettingsAdoption(unchecked((ulong)request.Integer)), ref request, ref reply); break;
                }
                ManagedRuntimeResult read = runtime.ReadSettingsAdoption(unchecked((ulong)request.Integer), input, out string page);
                Complete(read, ref request, ref reply); if (read.Success) WriteOutput(ref request, ref reply, page); break;
            }
            case BridgeOp.PREPARE_SETTINGS:
                ManagedRuntimeResult settingsPrepared = runtime.PrepareSettings(request.Generation, ReadInput(request), out ulong settingsToken);
                reply.Integer = unchecked((long)settingsToken);
                Complete(settingsPrepared, ref request, ref reply);
                break;
            case BridgeOp.COMMIT_SETTINGS:
            case BridgeOp.RESTORE_SETTINGS:
                Complete(runtime.PublishSettings(request.Generation, unchecked((ulong)request.Integer), request.Op == BridgeOp.RESTORE_SETTINGS,
                    request.InputBytes == 0 ? "" : ReadInput(request)), ref request, ref reply);
                break;
            case BridgeOp.DISCARD_SETTINGS:
                Complete(runtime.DiscardSettings(request.Generation, unchecked((ulong)request.Integer)), ref request, ref reply);
                break;
            case BridgeOp.GET_PREPARED_SETTINGS:
            {
                ManagedRuntimeResult previewRead = runtime.ReadPreparedSettings(request.Generation, unchecked((ulong)request.Integer), out string settingsPreview);
                Complete(previewRead, ref request, ref reply);
                if (previewRead.Success) WriteOutput(ref request, ref reply, settingsPreview);
                break;
            }
            case BridgeOp.GET_PREPARED_REVIEW:
                ManagedRuntimeResult reviewRead = runtime.ReadPreparedReview(request.Generation, unchecked((ulong)request.Integer), ReadInput(request), out string settingsReview);
                if (reviewRead.Success) WriteOutput(ref request, ref reply, settingsReview);
                else Complete(reviewRead, ref request, ref reply);
                break;
            case BridgeOp.VALIDATE_FIELDS:
            case BridgeOp.WRITE_FIELDS:
            {
                if (request.Flags > (uint)SettingsValidationPurpose.AUTHORING) { reply.Status = BridgeStatus.INVALID_ARGUMENT; break; }
                string input = ReadInput(request);
                if (request.Flags == (uint)SettingsValidationPurpose.AUTHORING) {
                    using JsonDocument envelope = JsonDocument.Parse(input);
                    JsonElement root = envelope.RootElement;
                    if (root.ValueKind != JsonValueKind.Object || root.EnumerateObject().Count() != 2
                        || !root.TryGetProperty("fields", out JsonElement fields) || fields.ValueKind != JsonValueKind.Object
                        || !root.TryGetProperty("rebind", out JsonElement rebind) || rebind.ValueKind != JsonValueKind.Array) {
                        reply.Status = BridgeStatus.INVALID_ARGUMENT; break;
                    }
                    string[] paths = rebind.EnumerateArray().Select(static (JsonElement path) => path.GetString() ?? throw new InvalidDataException("Reference paths must be strings.")).ToArray();
                    Complete(runtime.ConfigureFields(request.Generation, request.InstanceId, fields.GetRawText(), request.Op == BridgeOp.VALIDATE_FIELDS,
                        SettingsValidationPurpose.AUTHORING, paths), ref request, ref reply);
                } else
                    Complete(runtime.ConfigureFields(request.Generation, request.InstanceId, input, request.Op == BridgeOp.VALIDATE_FIELDS,
                        (SettingsValidationPurpose)request.Flags), ref request, ref reply);
                break;
            }
            case BridgeOp.BIND_OWNER:
                string binding = ReadInput(request);
                int bindingSeparator = binding.IndexOf('\n');
                if (bindingSeparator < 0 || binding.IndexOf('\n', bindingSeparator + 1) >= 0) { reply.Status = BridgeStatus.INVALID_ARGUMENT; break; }
                Complete(runtime.BindOwner(request.Generation, request.InstanceId, request.OwnerId, unchecked((ulong)request.Integer), request.CallId,
                    binding[..bindingSeparator], binding[(bindingSeparator + 1)..]), ref request, ref reply);
                break;
            case BridgeOp.VALIDATE_REFERENCES:
                Complete(runtime.ValidateReferences(request.Generation, request.InstanceId), ref request, ref reply);
                break;
            case BridgeOp.RESOLVE_SAVED_COMPONENT_REFERENCES:
                Complete(runtime.ResolveSavedComponentReferences(request.Generation, request.InstanceId), ref request, ref reply);
                break;
            case BridgeOp.DISCOVER_SOURCES:
                WriteOutput(ref request, ref reply, ScriptManifest.SourcesJson(ScriptManifest.ReadSources(ReadInput(request))));
                break;
            case BridgeOp.GET_DIAGNOSTICS:
                if (runtime.GetCatalog(request.Generation) is null) reply.Status = BridgeStatus.STALE_GENERATION;
                else WriteOutput(ref request, ref reply, runtime.GetDiagnostics(request.Generation));
                break;
            case BridgeOp.CAPTURE_RELOAD:
                if (request.Flags > 1) { reply.Status = BridgeStatus.INVALID_ARGUMENT; break; }
                ManagedRuntimeResult captured = runtime.CaptureReload(request.Generation, request.InstanceId, out string snapshot, includeRuntimeCaches: request.Flags == 0);
                Complete(captured, ref request, ref reply);
                if (captured.Success) WriteOutput(ref request, ref reply, snapshot);
                break;
            case BridgeOp.RESTORE_RELOAD:
                Complete(runtime.RestoreReload(request.Generation, request.InstanceId, ReadInput(request)), ref request, ref reply);
                break;
            case BridgeOp.INVOKE_JOB:
            case BridgeOp.INVOKE_EVENT:
                if (request.Op == BridgeOp.INVOKE_EVENT && request.Flags == 2) {
                    Complete(runtime.InvokeOwned(request.Generation, request.InstanceId, request.OwnerId, request.CallId,
                        unchecked((ulong)request.Integer), true, "", 0, false,
                        (SequenceEvent)DecodeEngineNotification(request, ScriptCallback.ON_SEQUENCE_EVENT)), ref request, ref reply);
                    break;
                }
                if (request.Flags > 1) { reply.Status = BridgeStatus.INVALID_ARGUMENT; break; }
                Complete(runtime.InvokeOwned(request.Generation, request.InstanceId, request.OwnerId, request.CallId,
                    unchecked((ulong)request.Integer), request.Op == BridgeOp.INVOKE_EVENT, ReadInput(request), request.X,
                    request.Flags != 0), ref request, ref reply);
                break;
            case BridgeOp.CANCEL_OWNER:
                if (request.Flags > 1) { reply.Status = BridgeStatus.INVALID_ARGUMENT; break; }
                Complete(request.Flags == 0
                    ? runtime.CancelOwnedJobs(request.Generation, request.InstanceId)
                    : runtime.InvalidateOwnedAssets(request.Generation, request.InstanceId), ref request, ref reply);
                break;
            case BridgeOp.RELEASE_INSTANCE:
                Complete(runtime.ReleaseInstance(request.Generation, request.InstanceId), ref request, ref reply);
                break;
            case BridgeOp.RESUME_INSTANCE:
                Complete(runtime.ResumeInstance(request.Generation, request.InstanceId, request.OwnerId), ref request, ref reply);
                break;
            case BridgeOp.RETIRE_GENERATION:
                Complete(runtime.RetireGeneration(request.Generation), ref request, ref reply);
                break;
            case BridgeOp.RETAIN_GENERATION:
                Complete(runtime.RetainGeneration(request.Generation), ref request, ref reply);
                break;
            case BridgeOp.RELEASE_GENERATION:
                Complete(runtime.ReleaseGeneration(request.Generation), ref request, ref reply);
                break;
            case BridgeOp.SHUTDOWN:
                if (ScriptApiRuntime.Current is not null) { reply.Status = BridgeStatus.INVALID_SCOPE; break; }
                runtime.Dispose(); s_runtime = null; s_nativeAddress = 0;
                if (ManagedDiagnostics.Enabled) ManagedDiagnostics.Checkpoint("native-shutdown");
                break;
            default: reply.Status = BridgeStatus.UNSUPPORTED; break;
        }
    }

    static void FieldOperation(ManagedScriptRuntime runtime, ref BridgeRequest request, ref BridgeReply reply)
    {
        if (ScriptApiRuntime.Current is not null) { reply.Status = BridgeStatus.INVALID_SCOPE; return; }
        if (!runtime.TryInstance(request.Generation, request.InstanceId, out ManagedScriptRuntime.ScriptInstance? record))
        { reply.Status = BridgeStatus.STALE_GENERATION; return; }
        if (request.Op == BridgeOp.GET_FIELD)
        {
            reply.Index = (uint)record.Type.Fields.Count;
            if (request.Index >= record.Type.Fields.Count) { reply.Status = BridgeStatus.NOT_FOUND; return; }
            ScriptFieldDescriptor metadata = record.Type.Fields[(int)request.Index];
            reply.ValueKind = metadata.Kind;
            reply.Flags = (metadata.IsSetting ? BridgeAbi.FIELD_SETTING : 0) | (metadata.IsInspectable ? BridgeAbi.FIELD_OBSERVATION : 0)
                | (metadata.Visible ? 0 : BridgeAbi.FIELD_HIDDEN) | (metadata.Range is not null ? BridgeAbi.FIELD_RANGE : 0)
                | (metadata.RequiredReference ? BridgeAbi.FIELD_REQUIRED : 0);
            if (metadata.IsReloadState) reply.Flags |= BridgeAbi.FIELD_RELOAD_STATE;
            /// @note Legacy scalar storage exposes approximate float bounds; the recursive schema is the exact editing contract.
            if (metadata.Range is ValueRangeAttribute range && metadata.ElementKind is ScriptFieldKind.FLOAT or ScriptFieldKind.INT) {
                var bounds = ManagedPrimitiveCodec.Range(metadata.ElementType, range);
                reply.X = Convert.ToSingle(bounds.Minimum, System.Globalization.CultureInfo.InvariantCulture);
                reply.Y = Convert.ToSingle(bounds.Maximum, System.Globalization.CultureInfo.InvariantCulture);
            }
            if (request.Flags == 3) WriteBytes(ref request, ref reply, ScriptFieldCodec.EncodeArray(metadata.PreviousKeys.ToArray(), ScriptFieldKind.STRING));
            else WriteOutput(ref request, ref reply, request.Flags switch
            {
                0 => metadata.Key,
                1 => metadata.Label,
                2 => ScriptFieldCodec.SchemaJson(metadata),
                _ => throw new ArgumentException("Unknown field metadata selector.")
            });
            return;
        }
        ReadOnlySpan<byte> input = new(request.Input, (int)request.InputBytes);
        int separator = input.IndexOf((byte)0);
        string key = s_utf8.GetString(separator < 0 ? input : input[..separator]);
        ScriptFieldDescriptor? field = record.Type.Fields.FirstOrDefault((ScriptFieldDescriptor candidate) => candidate.Key == key);
        if (field is null) { reply.Status = BridgeStatus.NOT_FOUND; return; }
        reply.ValueKind = field.Kind;
        if (field.Kind == ScriptFieldKind.NONE) { reply.Status = BridgeStatus.UNSUPPORTED; return; }
        if (request.Op == BridgeOp.READ_FIELD)
        {
            if (request.Flags == 2) {
                WriteOutput(ref request, ref reply, ManagedScriptRuntime.ObservationSummary(record, field));
                return;
            }
            object? value = field.Read(record.Instance);
            if (field.Kind == ScriptFieldKind.STRUCTURED)
            {
                if (request.Flags > 1) { reply.Status = BridgeStatus.INVALID_ARGUMENT; return; }
                if (request.Flags == 1) {
                    ulong[] identities = StructuredFieldCodec.ReferenceIdentities(value, field.ValueType, record.Binding.GameObject.SceneLifetime);
                    byte[] ids = new byte[checked(4 + identities.Length * 8)];
                    BinaryPrimitives.WriteUInt32LittleEndian(ids, (uint)identities.Length);
                    for (int index = 0; index < identities.Length; index++)
                        BinaryPrimitives.WriteUInt64LittleEndian(ids.AsSpan(4 + index * 8, 8), identities[index]);
                    WriteBytes(ref request, ref reply, ids);
                } else WriteOutput(ref request, ref reply, StructuredFieldCodec.Encode(value, field.ValueType));
                return;
            }
            if (value is null)
            {
                if (field.Kind is ScriptFieldKind.STRING or ScriptFieldKind.GAME_OBJECT || field.IsArray) reply.Flags |= BridgeAbi.NULL_VALUE;
                else reply.Status = BridgeStatus.INVALID_ARGUMENT;
                return;
            }
            if (field.IsArray)
            {
                if (request.Flags == 1 && field.Kind == ScriptFieldKind.GAME_OBJECT_ARRAY)
                {
                    GameObject[] array = (GameObject[])value;
                    byte[] ids = new byte[checked(4 + array.Length * 8)];
                    BinaryPrimitives.WriteUInt32LittleEndian(ids, (uint)array.Length);
                    for (int index = 0; index < array.Length; index++)
                        BinaryPrimitives.WriteUInt64LittleEndian(ids.AsSpan(4 + index * 8, 8), array[index]?.ObjectId ?? 0);
                    WriteBytes(ref request, ref reply, ids);
                }
                else WriteBytes(ref request, ref reply, ScriptFieldCodec.EncodeArray((Array)value, field.ElementKind));
                return;
            }
            switch (field.Kind)
            {
                case ScriptFieldKind.FLOAT: reply.X = (float)value; break;
                case ScriptFieldKind.INT: reply.Integer = Convert.ToInt32(value); break;
                case ScriptFieldKind.BOOL: reply.Integer = (bool)value ? 1 : 0; break;
                case ScriptFieldKind.STRING: WriteOutput(ref request, ref reply, (string)value); break;
                case ScriptFieldKind.VECTOR3:
                    Vector3 vector = (Vector3)value; reply.X = vector.X; reply.Y = vector.Y; reply.Z = vector.Z; break;
                case ScriptFieldKind.QUATERNION:
                    Quaternion quaternion = (Quaternion)value; reply.X = quaternion.X; reply.Y = quaternion.Y; reply.Z = quaternion.Z; reply.W = quaternion.W; break;
                case ScriptFieldKind.ASSET_REFERENCE:
                    WriteOutput(ref request, ref reply, ((IAssetReference)value).Reference ?? ""); break;
                case ScriptFieldKind.GAME_OBJECT:
                    GameObject reference = (GameObject)value; reply.Integer = unchecked((long)reference.ObjectId);
                    WriteOutput(ref request, ref reply, reference.Guid); break;
            }
            return;
        }
        if (!field.IsSetting || request.Index != (uint)field.Kind) { reply.Status = BridgeStatus.INVALID_ARGUMENT; return; }
        object? next;
        if (field.IsArray)
        {
            if ((request.Flags & BridgeAbi.NULL_VALUE) != 0 || separator < 0) { reply.Status = BridgeStatus.INVALID_ARGUMENT; return; }
            next = ScriptFieldCodec.DecodeArray(input[(separator + 1)..], field, (string guid) => runtime.ResolveGameObject(record, guid));
        }
        else switch (field.Kind)
        {
            case ScriptFieldKind.FLOAT: next = request.X; break;
            case ScriptFieldKind.INT:
                if (request.Integer is < int.MinValue or > int.MaxValue) { reply.Status = BridgeStatus.INVALID_ARGUMENT; return; }
                next = field.ValueType.IsEnum ? Enum.ToObject(field.ValueType, (int)request.Integer) : (object)(int)request.Integer;
                break;
            case ScriptFieldKind.BOOL:
                if (request.Integer is not (0 or 1)) { reply.Status = BridgeStatus.INVALID_ARGUMENT; return; }
                next = request.Integer != 0; break;
            case ScriptFieldKind.STRING:
                if ((request.Flags & BridgeAbi.NULL_VALUE) != 0 || separator < 0) { reply.Status = BridgeStatus.INVALID_ARGUMENT; return; }
                next = s_utf8.GetString(input[(separator + 1)..]); break;
            case ScriptFieldKind.VECTOR3: next = new Vector3(request.X, request.Y, request.Z); break;
            case ScriptFieldKind.QUATERNION: next = new Quaternion(request.X, request.Y, request.Z, request.W); break;
            case ScriptFieldKind.ASSET_REFERENCE:
                if (separator < 0) { reply.Status = BridgeStatus.INVALID_ARGUMENT; return; }
                string assetReference = s_utf8.GetString(input[(separator + 1)..]);
                next = assetReference.Length == 0 ? Activator.CreateInstance(field.ValueType) : Activator.CreateInstance(field.ValueType, assetReference);
                break;
            case ScriptFieldKind.STRUCTURED:
                if (separator < 0) { reply.Status = BridgeStatus.INVALID_ARGUMENT; return; }
                next = StructuredFieldCodec.Decode(s_utf8.GetString(input[(separator + 1)..]), field.ValueType, (string guid) => runtime.ResolveGameObject(record, guid));
                break;
            case ScriptFieldKind.GAME_OBJECT:
                if ((request.Flags & BridgeAbi.NULL_VALUE) != 0) next = null;
                else
                {
                    if (separator < 0) { reply.Status = BridgeStatus.INVALID_ARGUMENT; return; }
                    next = runtime.ResolveGameObject(record, s_utf8.GetString(input[(separator + 1)..]));
                }
                break;
            default: reply.Status = BridgeStatus.UNSUPPORTED; return;
        }
        if (!field.TryWrite(record.Instance, next)) reply.Status = BridgeStatus.INVALID_ARGUMENT;
        else record.SettingsRevision++;
    }

    static void Complete(ManagedRuntimeResult result, ref BridgeRequest request, ref BridgeReply reply)
    {
        reply.Status = result.Status;
        if (!result.Success && result.Message.Length != 0) WriteMessage(ref request, ref reply, result.Message);
    }
    static string ReadInput(BridgeRequest request) => s_utf8.GetString(new ReadOnlySpan<byte>(request.Input, (int)request.InputBytes));
    static void WriteOutput(ref BridgeRequest request, ref BridgeReply reply, string value)
    {
        int length = s_utf8.GetByteCount(value);
        reply.OutputBytes = (uint)length;
        if (length > request.OutputCapacity) { reply.Status = BridgeStatus.BUFFER_TOO_SMALL; return; }
        s_utf8.GetBytes(value.AsSpan(), new Span<byte>(request.Output, length));
    }
    static void WriteBytes(ref BridgeRequest request, ref BridgeReply reply, ReadOnlySpan<byte> value)
    {
        reply.OutputBytes = (uint)value.Length;
        if (value.Length > request.OutputCapacity) { reply.Status = BridgeStatus.BUFFER_TOO_SMALL; return; }
        value.CopyTo(new Span<byte>(request.Output, value.Length));
    }
    /// @note 診断は UTF8 境界で印付き切り詰めし、失敗 status と最初の原因を保つ。変更操作は再送しない。
    static void WriteMessage(ref BridgeRequest request, ref BridgeReply reply, string value)
    {
        /// @note 例外本文の不正 UTF16 は診断だけで置換する。設定・通常出力の strict codec は変更しない。
        byte[] bytes = Encoding.UTF8.GetBytes(value);
        if (bytes.Length <= request.OutputCapacity)
        {
            reply.OutputBytes = (uint)bytes.Length;
            bytes.CopyTo(new Span<byte>(request.Output, bytes.Length));
            return;
        }
        ReadOnlySpan<byte> marker = "\n[diagnostic truncated]"u8;
        int capacity = checked((int)request.OutputCapacity);
        int markerBytes = Math.Min(capacity, marker.Length);
        int prefix = capacity - markerBytes;
        while (prefix > 0 && (bytes[prefix] & 0xc0) == 0x80) prefix--;
        Span<byte> output = new(request.Output, capacity);
        bytes.AsSpan(0, prefix).CopyTo(output);
        marker[..markerBytes].CopyTo(output[prefix..]);
        reply.OutputBytes = (uint)(prefix + markerBytes);
    }

    sealed partial class NativeBridge : IScriptNativeBridge
    {
        public ComponentReferenceIdentity ResolveComponentReference(ScriptExecutionScope scope, ComponentReferenceIdentity identity)
        {
            int ownerBytes = s_utf8.GetByteCount(identity.OwnerGuid);
            using NativeTransferBuffer storage = new(checked(ownerBytes + 1 + s_utf8.GetByteCount(identity.SavedScriptId)), stackalloc byte[256]);
            Span<byte> input = storage.Bytes;
            s_utf8.GetBytes(identity.OwnerGuid.AsSpan(), input[..ownerBytes]); input[ownerBytes] = (byte)'\n';
            s_utf8.GetBytes(identity.SavedScriptId.AsSpan(), input[(ownerBytes + 1)..]);
            fixed (byte* bytes = input) {
                BridgeRequest request = Request(scope, 0, BridgeOp.RESOLVE_COMPONENT_REFERENCE);
                request.Index = identity.ComponentKind; request.Input = bytes; request.InputBytes = (uint)input.Length;
                string resolved = QueryText(ref request, out BridgeReply reply, allowNotFound: true);
                if (reply.Status == BridgeStatus.NOT_FOUND) return identity with { ObjectId = 0, SceneLifetime = 0, ComponentLifetime = 0, ScriptInstanceId = 0, Generation = 0 };
                int separator = resolved.IndexOf('\n');
                string owner = separator < 0 ? resolved : resolved[..separator];
                string birth = separator < 0 ? "" : resolved[(separator + 1)..];
                if (owner != identity.OwnerGuid || (identity.Kind == "script" && (!Guid.TryParseExact(birth, "D", out Guid parsed) || parsed == Guid.Empty)))
                    throw new InvalidDataException("Component resolution changed its address or omitted the Script lifetime.");
                return identity with { ObjectId = unchecked((ulong)reply.Integer), SceneLifetime = reply.CallId,
                    ComponentLifetime = identity.Kind == "native" ? reply.Generation : 0,
                    ScriptInstanceId = reply.InstanceId, Generation = identity.Kind == "script" ? scope.Generation : 0, ScriptBirth = birth };
            }
        }
        public bool HasScriptEntry(ScriptExecutionScope scope, ulong objectId, string savedScriptId)
        {
            using NativeTransferBuffer storage = new(savedScriptId, stackalloc byte[256]);
            Span<byte> bytes = storage.Bytes;
            fixed (byte* input = bytes) {
                BridgeRequest request = Request(scope, objectId, BridgeOp.COMPONENT_READ);
                request.Index = uint.MaxValue; request.Flags = 2;
                request.Input = input; request.InputBytes = (uint)bytes.Length;
                BridgeReply reply = default;
                int status = m_dispatch(&request, &reply);
                if (status == (int)BridgeStatus.NOT_FOUND && reply.Status == BridgeStatus.NOT_FOUND) return false;
                RequireSuccess(status, reply.Status);
                return reply.Integer switch { 0 => false, 1 => true, _ => throw new InvalidDataException("Invalid Script presence reply.") };
            }
        }
        internal NativeBridge(nint address) { m_dispatch = (delegate* unmanaged[Cdecl]<BridgeRequest*, BridgeReply*, int>)address; }
        readonly delegate* unmanaged[Cdecl]<BridgeRequest*, BridgeReply*, int> m_dispatch;
        public bool IsAlive(ScriptExecutionScope scope, ulong ownerId)
        {
            BridgeRequest request = Request(scope, ownerId, BridgeOp.OBJECT_IS_ALIVE);
            BridgeReply reply = default;
            int status = m_dispatch(&request, &reply);
            if (status == (int)BridgeStatus.NOT_FOUND || status == (int)BridgeStatus.STALE_GENERATION) return false;
            RequireSuccess(status, reply.Status);
            return reply.Integer != 0;
        }
        public float DeltaSeconds(ScriptExecutionScope scope)
        {
            float delta = ReadDeltaSecondsNative(scope);
            if (!float.IsFinite(delta) || delta < 0f) throw new InvalidOperationException("Native delta time must be finite and nonnegative.");
            return delta;
        }
        public ulong ResolveGameObject(ulong generation, ulong instanceId, ulong ownerId, string guid)
        {
            using NativeTransferBuffer storage = new(guid, stackalloc byte[256]);
            Span<byte> bytes = storage.Bytes;
            fixed (byte* input = bytes)
            {
                BridgeRequest request = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.RESOLVE_GUID,
                    Generation = generation, InstanceId = instanceId, OwnerId = ownerId, Input = input, InputBytes = (uint)bytes.Length };
                QueryIdenticalBytes(ref request, bytes, out BridgeReply reply);
                return unchecked((ulong)reply.Integer);
            }
        }
        public ulong[] GetManagedComponents(ScriptExecutionScope scope, ulong objectId)
        {
            List<ulong> ids = new();
            Span<ulong> local = stackalloc ulong[64];
            uint offset = 0;
            while (true) {
                ManagedComponentPage page = ReadManagedComponents(scope, objectId, offset, local);
                foreach (ulong id in local[..page.Written]) ids.Add(id);
                if (page.Complete) return ids.ToArray();
                offset = page.NextOffset;
            }
        }
        public int GetManagedComponents(ScriptExecutionScope scope, ulong objectId, Span<ulong> destination)
        {
            Span<ulong> pageIds = stackalloc ulong[64];
            uint offset = 0; int count = 0;
            while (true) {
                ManagedComponentPage page = ReadManagedComponents(scope, objectId, offset, pageIds);
                count = checked(count + page.Written);
                if (page.Complete) break;
                offset = page.NextOffset;
            }
            if (count > destination.Length) return count;
            offset = 0; int written = 0;
            while (true) {
                ManagedComponentPage page = ReadManagedComponents(scope, objectId, offset, pageIds);
                pageIds[..page.Written].CopyTo(destination[written..]); written += page.Written;
                if (page.Complete) return count;
                offset = page.NextOffset;
            }
        }
        public ManagedComponentPage ReadManagedComponents(ScriptExecutionScope scope, ulong objectId, uint offset, Span<ulong> destination)
        {
            if (destination.IsEmpty) throw new ArgumentException("A component page requires output capacity.", nameof(destination));
            Span<byte> bytes = stackalloc byte[512];
            int capacity = Math.Min(destination.Length, bytes.Length / 8);
            BridgeRequest request = Request(scope, objectId, BridgeOp.MANAGED_COMPONENTS);
            request.Flags = 1; request.Index = offset;
            fixed (byte* output = bytes) {
                request.Output = output; request.OutputCapacity = (uint)(capacity * 8);
                BridgeReply reply = default; int status = m_dispatch(&request, &reply);
                if (status == (int)BridgeStatus.NOT_FOUND && reply.Status == BridgeStatus.NOT_FOUND) return new(0, offset, true);
                RequireSuccess(status, reply.Status);
                if (reply.Index > capacity || reply.OutputBytes != reply.Index * 8 || reply.Flags > 1
                    || reply.Integer < 0 || (ulong)reply.Integer > uint.MaxValue || (uint)reply.Integer < offset
                    || (reply.Flags == 0 && (uint)reply.Integer == offset)) throw new InvalidDataException("Invalid component page reply.");
                for (int index = 0; index < (int)reply.Index; index++)
                    destination[index] = BinaryPrimitives.ReadUInt64LittleEndian(bytes.Slice(index * 8, 8));
                return new((int)reply.Index, (uint)reply.Integer, reply.Flags == 1);
            }
        }
        public string GetObjectName(ScriptExecutionScope scope, ulong objectId)
        {
            BridgeRequest request = Request(scope, objectId, BridgeOp.OBJECT_NAME);
            return QueryText(ref request, out _);
        }
        public GameObject? FindByName(ScriptExecutionScope scope, string name, bool includeInactive)
        {
            using NativeTransferBuffer storage = new(name, stackalloc byte[256]);
            Span<byte> bytes = storage.Bytes;
            fixed (byte* input = bytes)
            {
                BridgeRequest request = Request(scope, 0, BridgeOp.SCENE_FIND_NAME);
                request.Input = input; request.InputBytes = (uint)bytes.Length; request.Flags = includeInactive ? 1U : 0U;
                return QueryObject(ref request);
            }
        }
        public GameObject? FindChild(ScriptExecutionScope scope, ulong objectId, ReadOnlySpan<string> names, bool includeInactive)
        {
            int count = checked(4 + names.Length * 4);
            foreach (string name in names) if (name is not null) count = checked(count + s_utf8.GetByteCount(name));
            using NativeTransferBuffer storage = new(count, stackalloc byte[256]);
            Span<byte> bytes = storage.Bytes;
            BinaryPrimitives.WriteUInt32LittleEndian(bytes, (uint)names.Length); int offset = 4;
            foreach (string name in names) {
                if (name is null) { BinaryPrimitives.WriteUInt32LittleEndian(bytes[offset..], uint.MaxValue); offset += 4; }
                else WriteObjectQueryText(name, bytes, ref offset);
            }
            fixed (byte* input = bytes)
            {
                BridgeRequest request = Request(scope, objectId, BridgeOp.FIND_CHILD_PATH);
                request.Input = input; request.InputBytes = (uint)bytes.Length; request.Flags = includeInactive ? 1U : 0U;
                return QueryObject(ref request);
            }
        }
        GameObject? QueryObject(ref BridgeRequest request)
        {
            Span<byte> local = stackalloc byte[256]; byte[]? rented = null;
            try {
                int length = QueryBorrowed(ref request, local, ref rented, out BridgeReply reply, allowNotFound: true);
                if (reply.Status == BridgeStatus.NOT_FOUND) return null;
                if (reply.Integer == 0 || reply.CallId == 0) throw new InvalidDataException("Object lookup returned an invalid lifetime identity.");
                ReadOnlySpan<byte> bytes = rented is null ? local[..length] : rented.AsSpan(0, length);
                return new GameObject(unchecked((ulong)reply.Integer), reply.CallId, ScriptFieldCodec.ValidateGuid(s_utf8.GetString(bytes)));
            } finally { if (rented is not null) ArrayPool<byte>.Shared.Return(rented, clearArray: true); }
        }
        /// @note Decode while the local/pool lease is alive; only the required returned string becomes owned managed storage.
        string QueryText(ref BridgeRequest request, out BridgeReply reply, bool allowNotFound = false)
        {
            Span<byte> local = stackalloc byte[256]; byte[]? rented = null;
            try {
                int length = QueryBorrowed(ref request, local, ref rented, out reply, allowNotFound);
                return s_utf8.GetString(rented is null ? local[..length] : rented.AsSpan(0, length));
            } finally { if (rented is not null) ArrayPool<byte>.Shared.Return(rented, clearArray: true); }
        }
        /// @note Identity echoes are compared before returning the lease; no response array is required.
        void QueryIdenticalBytes(ref BridgeRequest request, ReadOnlySpan<byte> expected, out BridgeReply reply)
        {
            Span<byte> local = stackalloc byte[256]; byte[]? rented = null;
            try {
                int length = QueryBorrowed(ref request, local, ref rented, out reply);
                ReadOnlySpan<byte> returned = rented is null ? local[..length] : rented.AsSpan(0, length);
                if (!returned.SequenceEqual(expected)) throw new InvalidDataException("GUID resolution returned a different reference.");
            } finally { if (rented is not null) ArrayPool<byte>.Shared.Return(rented, clearArray: true); }
        }
        /// @note 借用出力は同期呼出内だけ有効。呼び手は rented を finally で返し、pool の記憶域を外へ公開しない。
        int QueryBorrowed(ref BridgeRequest request, Span<byte> local, ref byte[]? rented, out BridgeReply reply, bool allowNotFound = false)
        {
            for (int attempt = 0; attempt < 2; attempt++)
            {
                Span<byte> output = rented is null ? local : rented;
                fixed (byte* buffer = output)
                {
                    request.Output = buffer; request.OutputCapacity = (uint)output.Length;
                    BridgeReply response = default;
                    int status;
                    fixed (BridgeRequest* pointer = &request) status = m_dispatch(pointer, &response);
                    reply = response;
                    if (allowNotFound && status == (int)BridgeStatus.NOT_FOUND && reply.Status == BridgeStatus.NOT_FOUND) return 0;
                    if (status == (int)BridgeStatus.BUFFER_TOO_SMALL && reply.Status == BridgeStatus.BUFFER_TOO_SMALL && reply.OutputBytes <= 16U * 1024U * 1024U)
                    {
                        if (attempt != 0 || reply.OutputBytes <= output.Length)
                            throw new InvalidDataException("Native read-only query reported an inconsistent capacity.");
                        rented = ArrayPool<byte>.Shared.Rent((int)reply.OutputBytes);
                        continue;
                    }
                    RequireSuccess(status, reply.Status);
                    if (reply.OutputBytes > output.Length) throw new InvalidDataException("Native output exceeds its borrowed buffer.");
                    return (int)reply.OutputBytes;
                }
            }
            throw new InvalidOperationException("Native read-only query changed its capacity twice.");
        }
        BridgeReply Call(ref BridgeRequest request)
        {
            BridgeReply reply = default;
            fixed (BridgeRequest* pointer = &request) RequireSuccess(m_dispatch(pointer, &reply), reply.Status, request.Op, unchecked((ulong)request.Integer));
            return reply;
        }
        static BridgeRequest Request(ScriptExecutionScope scope, ulong ownerId, BridgeOp op)
            => new() { AbiVersion = BridgeAbi.VERSION, Op = op, Generation = scope.Generation, InstanceId = scope.InstanceId, OwnerId = scope.OwnerId, CallId = scope.CallId, Integer = unchecked((long)ownerId) };
        static void RequireSuccess(int result, BridgeStatus status, BridgeOp? operation = null, ulong target = 0)
        {
            if (result != 0 || status != BridgeStatus.OK)
            {
                string context = operation is BridgeOp op ? $" {op} (target=0x{target:x})" : "";
                throw new InvalidOperationException($"Native Script API{context} rejected the operation: {(BridgeStatus)result}/{status}.");
            }
        }
    }
}
