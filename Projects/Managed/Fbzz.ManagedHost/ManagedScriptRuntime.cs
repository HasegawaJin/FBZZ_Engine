/// @file    ManagedScriptRuntime.cs
/// @brief   collectible 世代の準備・同期配送・失敗保持・解放待ち上限。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Buffers;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;
using System.Text.Json;
using Fbzz;

namespace Fbzz.ManagedHost;

public sealed record ManagedRuntimeResult(BridgeStatus Status, string Message = "")
{
    public bool Success => Status == BridgeStatus.OK;
}

/// @note 外部の CLR 強参照が残る場合、Unload の即時完了を保証しない。
/// @see https://learn.microsoft.com/en-us/dotnet/standard/assembly/unloadability collectible unload。
public sealed partial class ManagedScriptRuntime : IDisposable, IScriptComponentLookup
{
    const int MAX_PENDING_CONTEXTS = 3;
    const int MAX_COLLECTION_REQUESTS = 10;
    /// @note 成功結果は不変で個体・scope・世代を保持しない。失敗診断と実行 scope は呼び出しごとに分離する。
    static ManagedRuntimeResult CallbackSuccess { get; } = new(BridgeStatus.OK);
    readonly Dictionary<ulong, ScriptGeneration> m_generations = new();
    readonly Dictionary<ulong, ScriptInstance> m_instances = new();
    readonly List<WeakReference> m_pending = new();
    readonly IScriptNativeBridge m_bridge;
    readonly int m_threadId = Environment.CurrentManagedThreadId;
    ulong m_nextInstance = 1;
    int m_collectionRequests;
    ulong m_collectionRequestTotal;
    public ManagedScriptRuntime() : this(new UnavailableBridge()) { }
    internal ManagedScriptRuntime(IScriptNativeBridge bridge) { m_bridge = bridge; }
    public int PendingUnloadCount => m_pending.Count((WeakReference context) => context.IsAlive);
    internal int CollectionRequestCount => m_collectionRequests;
    internal ulong CollectionRequestTotal => m_collectionRequestTotal;
    public ScriptCatalog? GetCatalog(ulong generation) => m_generations.TryGetValue(generation, out ScriptGeneration? value) ? value.Catalog : null;
    public string GetDiagnostics(ulong generation) => m_generations.TryGetValue(generation, out ScriptGeneration? value) ? value.Diagnostics : "";

    /// @note 候補失敗時も、既存の catalog・個体・dispatch は破棄しない。
    public ManagedRuntimeResult LoadCandidate(string assemblyPath, ulong generation, bool internalMode = false)
    {
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE, "Candidate loading requires the idle Host thread.");
        if (generation == 0 || m_generations.ContainsKey(generation) || !Path.IsPathFullyQualified(assemblyPath))
            return new(BridgeStatus.INVALID_ARGUMENT, "Generation and absolute assembly path must be valid and unique.");
        if (!File.Exists(assemblyPath)) return new(BridgeStatus.LOAD_FAILED, "Candidate assembly does not exist.");
        if (!internalMode && !File.Exists(assemblyPath + ".scripts.json"))
            return new(BridgeStatus.LOAD_FAILED, "A published game assembly requires its .scripts.json manifest.");
        CollectRetired();
        if (!HasTransientGenerationCapacity())
            return new(BridgeStatus.PENDING_LIMIT, "Retire an existing candidate before loading another.");
        if (PendingUnloadCount >= MAX_PENDING_CONTEXTS)
        {
            if (m_collectionRequests >= MAX_COLLECTION_REQUESTS)
                return new(BridgeStatus.PENDING_LIMIT, "Pending generations remain after ten background collection requests. Release retained game references; the current generation is unchanged.");
            m_collectionRequests++;
            m_collectionRequestTotal++;
            /// @note 実ロード前の準備要求だけで回収を促す。同期 finalizer 待ちをせず、background 要求でも CLR の pause は残る。
            /// @see https://learn.microsoft.com/en-us/dotnet/api/system.gc.collect#system-gc-collect(system-int32-system-gccollectionmode-system-boolean-system-boolean) blocking=false は background を要求し、非停止を保証しない。
            GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, blocking: false, compacting: false);
            CollectRetired();
            if (PendingUnloadCount >= MAX_PENDING_CONTEXTS)
                return new(BridgeStatus.COLLECTION_PENDING, "Waiting for retired generations to be collected. Retry this unstarted candidate at the next preparation point.");
        }
        return PrepareCandidate(assemblyPath, generation, internalMode);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    ManagedRuntimeResult PrepareCandidate(string path, ulong generation, bool internalMode)
    {
        GameLoadContext? context = null;
        try
        {
            context = new GameLoadContext(path);
            Assembly assembly = context.LoadFromAssemblyPath(path);
            CatalogBuildResult result = ScriptCatalogBuilder.Build(assembly);
            if (!result.Success)
                return Reject(context, string.Join("\n", result.Diagnostics.Select((ScriptDiagnostic value) => $"{value.Code} {value.TypeName}.{value.MemberName}: {value.Message}")));
            ScriptCatalog catalog = result.Catalog!;
            ScriptManifest.Apply(catalog, path, assembly.GetName().Name ?? "", internalMode);
            if (ManagedDiagnostics.Enabled) ManagedDiagnostics.RegisterGame(assembly, generation);
            /// @note Loading/discovery is passive: initializer defaults are evaluated only by an explicit instance creation action.
            foreach (ManagedTypeDescriptor type in catalog.AllTypes)
                foreach (ScriptFieldDescriptor field in type.Fields.Where(field => field.IsSetting))
                    _ = ManagedFieldSchema.Build(field.ValueType);
            string diagnostics = string.Join("\n", result.Diagnostics.Select((ScriptDiagnostic value) => $"{value.Code} {value.TypeName}.{value.MemberName}: {value.Message}"));
            m_generations.Add(generation, new(context, catalog, diagnostics, internalMode));
            return new(BridgeStatus.OK);
        }
        catch (Exception exception)
        {
            if (context is not null) RetireContext(context);
            return new(BridgeStatus.LOAD_FAILED, Describe(exception));
        }
    }

    public ManagedRuntimeResult CreateInstance(ulong generation, string typeName, ulong ownerId, out ulong instanceId)
        => CreateInstance(generation, typeName, ownerId, Guid.NewGuid().ToString("D"), out instanceId);

    /// @note 保存 ID は CLR binding より先に渡し、同じ owner の同一世代で重複を拒否する。
    public ManagedRuntimeResult CreateInstance(ulong generation, string typeName, ulong ownerId, string savedScriptId, out ulong instanceId)
    {
        instanceId = 0;
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE, "Construction requires the idle Host thread.");
        if (!Guid.TryParseExact(savedScriptId, "D", out Guid identity) || identity == Guid.Empty)
            return new(BridgeStatus.INVALID_ARGUMENT, "Saved Script identity must be a nonzero UUID.");
        savedScriptId = identity.ToString("D");
        if (!m_generations.TryGetValue(generation, out ScriptGeneration? generationRecord)) return new(BridgeStatus.STALE_GENERATION);
        ScriptTypeDescriptor? type = generationRecord.Catalog.Types.FirstOrDefault((ScriptTypeDescriptor value)
            => value.PersistentId == typeName || (generationRecord.InternalMode && value.TypeName == typeName));
        if (type is null || ownerId == 0) return new(BridgeStatus.NOT_FOUND);
        if (m_instances.Values.Any((ScriptInstance value) => value.Generation == generation && value.OwnerId == ownerId
            && value.Binding.SavedScriptId == savedScriptId))
            return new(BridgeStatus.INVALID_ARGUMENT, "Saved Script identity is duplicated within its owner.");
        try
        {
            Script instance = type.CreateInstance();
            string? invalid = ValidateSettings(type, instance);
            if (invalid is not null) return new(BridgeStatus.LOAD_FAILED, invalid);
            ulong id = m_nextInstance++;
            ScriptBinding binding = new(generation, id, ownerId, savedScriptId, type.PersistentId);
            instance.Binding = binding;
            Dictionary<ScriptCallback, Delegate> callbacks = new();
            foreach (ScriptCallbackDescriptor callback in type.Callbacks) callbacks.Add(callback.Callback, callback.CreateTypedInvocation(instance));
            m_instances.Add(id, new(generation, ownerId, type, instance, binding, callbacks));
            instanceId = id;
            return new(BridgeStatus.OK);
        }
        catch (Exception exception) { return new(BridgeStatus.LOAD_FAILED, Describe(exception)); }
    }

    public ManagedRuntimeResult TryInvoke(ulong generation, ulong instanceId, ulong ownerId, ulong callId, ScriptCallback callback)
    {
        if (ScriptCatalogBuilder.CallbackArgument(callback) is not null)
            return new(BridgeStatus.INVALID_ARGUMENT, "A typed callback requires its notification snapshot.");
        return TryInvokeCore(generation, instanceId, ownerId, callId, callback, default, false);
    }

    /// @note typed delegate を同期配送し、相手 GUID の検索・callback 中の引数生成・user property の評価を行わない。
    public ManagedRuntimeResult TryInvokeCollision(ulong generation, ulong instanceId, ulong ownerId, ulong callId,
        ScriptCallback callback, Collision collision)
    {
        if (!ScriptCatalogBuilder.IsCollisionCallback(callback) && !ScriptCatalogBuilder.IsTriggerCallback(callback))
            return new(BridgeStatus.INVALID_ARGUMENT, "The callback does not accept collision data.");
        return TryInvokeCore(generation, instanceId, ownerId, callId, callback, collision, true);
    }

    /// @note 値の型を共通 callback 契約と照合し、native 借用文字列を callback 後へ持ち出さない。
    public ManagedRuntimeResult TryInvokeEngineNotification(ulong generation, ulong instanceId, ulong ownerId, ulong callId,
        ScriptCallback callback, object? payload)
    {
        if (payload is null || !ScriptCatalogBuilder.IsEngineNotification(callback) || ScriptCatalogBuilder.CallbackArgument(callback) != payload.GetType())
            return new(BridgeStatus.INVALID_ARGUMENT, "The notification does not match this callback's payload type.");
        return TryInvokeCore(generation, instanceId, ownerId, callId, callback, default, false, payload);
    }

    ManagedRuntimeResult TryInvokeCore(ulong generation, ulong instanceId, ulong ownerId, ulong callId,
        ScriptCallback callback, Collision collision, bool physics, object? notification = null)
    {
        if (callback is ScriptCallback.DRAW_GIZMOS or ScriptCallback.DRAW_GIZMOS_SELECTED)
            return new(BridgeStatus.INVALID_ARGUMENT, "Draw callbacks require a detached settings candidate and bounded gizmo capture.");
        if (Environment.CurrentManagedThreadId != m_threadId || callId == 0) return new(BridgeStatus.INVALID_SCOPE);
        if (!TryInstance(generation, instanceId, out ScriptInstance? record) || record.OwnerId != ownerId) return new(BridgeStatus.STALE_GENERATION);
        if (record.Binding.GameObject.SceneLifetime == 0) return new(BridgeStatus.INVALID_SCOPE, "The Script owner is not attached to a scene.");
        if (physics && collision.Other is GameObject other && (other.ObjectId == 0
            || other.SceneLifetime != record.Binding.GameObject.SceneLifetime))
            return new(BridgeStatus.INVALID_ARGUMENT, "The collision target must belong to the current scene lifetime.");
        if (record.Faulted && callback != ScriptCallback.ON_DESTROY) return new(BridgeStatus.CALLBACK_FAILED, "The script entry is faulted.");
        if (callback is ScriptCallback.ON_DISABLE or ScriptCallback.ON_DESTROY) record.Binding.Assets.InvalidateOwned();
        if (!record.Callbacks.TryGetValue(callback, out Delegate? invoke))
        { if (callback is ScriptCallback.ON_DISABLE or ScriptCallback.ON_DESTROY) ClearOwnedJobs(generation, instanceId); return CallbackSuccess; }
        ScriptExecutionScope? previous = ScriptApiRuntime.Current;
        ManagedCallbackStart diagnosticStart = ManagedDiagnostics.Enabled ? ManagedDiagnostics.BeginCallback() : default;
        bool diagnosticSuccess = false;
        try
        {
            ScriptExecutionScope scope = new(m_bridge, this, generation, instanceId, ownerId, callId, record.Binding.GameObject.SceneLifetime, callback);
            ScriptApiRuntime.Current = scope;
            if (record.Type.RequiredComponentTypes.Count != 0 && (callback is ScriptCallback.AWAKE or ScriptCallback.ON_ENABLE or ScriptCallback.START
                or ScriptCallback.UPDATE or ScriptCallback.FIXED_UPDATE or ScriptCallback.LATE_UPDATE or ScriptCallback.CONNECT
                || ScriptCatalogBuilder.IsCollisionCallback(callback) || ScriptCatalogBuilder.IsTriggerCallback(callback)
                || ScriptCatalogBuilder.IsEngineNotification(callback))) {
                ManagedRuntimeResult requirements = ValidateComponentRequirements(record, scope);
                if (!requirements.Success) { record.Binding.Assets.InvalidateOwned(); return requirements; }
            }
            if (callback == ScriptCallback.CONNECT) m_connecting.Add(instanceId);
            if (ScriptCatalogBuilder.IsCollisionCallback(callback)) ((Action<Collision>)invoke)(collision);
            else if (ScriptCatalogBuilder.IsTriggerCallback(callback)) ((Action<GameObject?>)invoke)(collision.Other);
            else if (callback == ScriptCallback.ON_ANIMATION_EVENT) ((Action<AnimationEvent>)invoke)((AnimationEvent)notification!);
            else if (callback == ScriptCallback.ON_ANIMATOR_MOVE) ((Action<RootMotionInfo>)invoke)((RootMotionInfo)notification!);
            else if (callback == ScriptCallback.ON_SEQUENCE_EVENT) ((Action<SequenceEvent>)invoke)((SequenceEvent)notification!);
            else if (callback == ScriptCallback.ON_SEQUENCE_FINISHED) ((Action<string>)invoke)((string)notification!);
            else ((Action)invoke)();
            if (callback == ScriptCallback.CONNECT && HasPendingConnections(instanceId))
                CommitConnections(scope);
            diagnosticSuccess = true;
            return CallbackSuccess;
        }
        catch (Exception exception)
        {
            record.Faulted = true;
            record.Binding.Assets.InvalidateOwned();
            if (ScriptApiRuntime.Current is ScriptExecutionScope failedScope) CancelFailedJobs(failedScope);
            ClearOwnedJobs(generation, instanceId);
            return new(BridgeStatus.CALLBACK_FAILED, Describe(exception));
        }
        finally
        {
            m_connecting.Remove(instanceId);
            if (callback is ScriptCallback.ON_DISABLE or ScriptCallback.ON_DESTROY) ClearOwnedJobs(generation, instanceId);
            ScriptApiRuntime.Current = previous;
            if (ManagedDiagnostics.Enabled) ManagedDiagnostics.EndCallback(diagnosticStart, generation, instanceId, ownerId, callId, callback, record.Type.TypeName, diagnosticSuccess);
        }
    }

    /// @note Keep the CONNECT-only scan outside callback dispatch so captured parameters cannot allocate a display class for every lifecycle call.
    bool HasPendingConnections(ulong instanceId)
    {
        foreach (OwnedJob job in m_jobs.Values)
            if (job.PendingCaller == instanceId && job.Pending) return true;
        return false;
    }

    public ManagedRuntimeResult ReleaseInstance(ulong generation, ulong instanceId)
    {
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (!TryInstance(generation, instanceId, out ScriptInstance? record)) return new(BridgeStatus.STALE_GENERATION);
        record.Binding.Assets.InvalidateOwned();
        record.Binding.Active = false;
        ReleaseObservations(generation, instanceId);
        RemoveBoundScript(record);
        ClearOwnedJobs(generation, instanceId);
        CloseOwnedSettingsEdits(generation, instanceId);
        foreach (ulong token in m_settingsTransactions.Where(pair => pair.Value.Generation == generation && pair.Value.Targets.Any(target => target.Id == instanceId && !target.Initial)).Select(pair => pair.Key).ToArray())
            DiscardSettings(generation, token);
        foreach (SettingsTarget target in m_settingsTransactions.Values.Where(transaction => transaction.Generation == generation).SelectMany(transaction => transaction.Targets).Where(target => target.Id == instanceId))
            target.Record = null;
        m_instances.Remove(instanceId);
        return new(BridgeStatus.OK);
    }

    /// @note 設定修復後の明示復帰だけが障害ラッチを解除する。callback と消去済み jobs は再送しない。
    public ManagedRuntimeResult ResumeInstance(ulong generation, ulong instanceId, ulong ownerId)
    {
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE, "Resuming requires the idle Host thread.");
        if (!TryInstance(generation, instanceId, out ScriptInstance? record) || record.OwnerId != ownerId)
            return new(BridgeStatus.STALE_GENERATION);
        if (!record.Binding.Active || record.Binding.GameObject.SceneLifetime == 0)
            return new(BridgeStatus.INVALID_SCOPE, "The Script owner is not attached to a live scene.");
        /// @note routine の Dispose が管理フィールドを変える場合もあるため、解除後の値で検証する。
        ClearOwnedJobs(generation, instanceId);
        ManagedRuntimeResult valid = ValidateReferences(generation, instanceId);
        if (!valid.Success) return valid;
        record.Faulted = false;
        return new(BridgeStatus.OK);
    }

    /// @note native の自己 GO 接続だけを更新する。Live 中は現世代・同 Scene の metadata 接続だけ許可し、設定参照・callback を変更しない。
    public ManagedRuntimeResult BindOwner(ulong generation, ulong instanceId, ulong ownerId, ulong objectId, ulong sceneLifetime, string guid, string scriptBirth = "")
    {
        if (Environment.CurrentManagedThreadId != m_threadId) return new(BridgeStatus.INVALID_SCOPE);
        ScriptExecutionScope? scope = ScriptApiRuntime.Current;
        if (scope is ScriptExecutionScope currentScope && (!ReferenceEquals(currentScope.Components, this) || currentScope.ThreadId != m_threadId || currentScope.CallId == 0
            || generation != currentScope.Generation || sceneLifetime != currentScope.SceneLifetime || objectId == 0))
            return new(BridgeStatus.INVALID_SCOPE);
        if (!TryInstance(generation, instanceId, out ScriptInstance? record) || record.OwnerId != ownerId) return new(BridgeStatus.STALE_GENERATION);
        if ((objectId == 0) != (sceneLifetime == 0) || (objectId != 0 && string.IsNullOrWhiteSpace(guid))) return new(BridgeStatus.INVALID_ARGUMENT);
        if (objectId != 0 && BoundScripts(generation, objectId, sceneLifetime) is Dictionary<string, ScriptInstance> existing
            && existing.TryGetValue(record.Binding.SavedScriptId, out ScriptInstance? duplicate) && duplicate != record)
            return new(BridgeStatus.INVALID_ARGUMENT, "Saved Script identity is duplicated within its GameObject.");
        if (scriptBirth.Length != 0 && (!Guid.TryParseExact(scriptBirth, "D", out Guid birth) || birth == Guid.Empty)) return new(BridgeStatus.INVALID_ARGUMENT);
        if (record.Binding.ScriptBirth.Length != 0 && scriptBirth.Length != 0 && record.Binding.ScriptBirth != scriptBirth) return new(BridgeStatus.INVALID_ARGUMENT);
        if (scriptBirth.Length != 0) record.Binding.ScriptBirth = scriptBirth;
        GameObject current = record.Binding.GameObject;
        if (current.ObjectId != objectId || current.SceneLifetime != sceneLifetime || current.Guid != guid) {
            RemoveBoundScript(record);
            record.Binding.GameObject = new GameObject(objectId, sceneLifetime, guid);
            AddBoundScript(record);
        }
        return new(BridgeStatus.OK);
    }

    /// @note current + delta を全件検証し、validateOnly は何も書かない。required 生存条件は明示した検証段階で決める。
    /// @note Authoring/Snapshot も型・数値・範囲を全件検証し、Runtime だけ開始要件の生存を要求する。
    public ManagedRuntimeResult ConfigureFields(ulong generation, ulong instanceId, string json, bool validateOnly,
        SettingsValidationPurpose purpose = SettingsValidationPurpose.RUNTIME, IReadOnlyCollection<string>? rebindReferences = null)
    {
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (!TryInstance(generation, instanceId, out ScriptInstance? record)) return new(BridgeStatus.STALE_GENERATION);
        if (purpose is not (SettingsValidationPurpose.RUNTIME or SettingsValidationPurpose.SNAPSHOT or SettingsValidationPurpose.AUTHORING))
            return new(BridgeStatus.INVALID_ARGUMENT, "Unknown settings validation purpose.");
        try
        {
            HashSet<string> rebind = new(StringComparer.Ordinal);
            if (rebindReferences is not null)
                foreach (string path in rebindReferences) {
                    if (purpose != SettingsValidationPurpose.AUTHORING || path is null || !path.StartsWith("/", StringComparison.Ordinal)
                        || path.Length > SettingsProtocolLimits.MAX_PATH_CHARACTERS || !rebind.Add(path))
                        return new(BridgeStatus.INVALID_ARGUMENT, "Reference rebind paths must be unique Authoring leaf paths.");
                }
            HashSet<string> referenceLeaves = new(StringComparer.Ordinal);
            using JsonDocument document = JsonDocument.Parse(json);
            if (document.RootElement.ValueKind != JsonValueKind.Object) return new(BridgeStatus.INVALID_ARGUMENT, "Settings delta must be a JSON object.");
            HashSet<string> names = new(StringComparer.Ordinal);
            Dictionary<ScriptFieldDescriptor, object?> prepared = new();
            foreach (JsonProperty property in document.RootElement.EnumerateObject())
            {
                ScriptFieldDescriptor? field = record.Type.Fields.FirstOrDefault((ScriptFieldDescriptor value)
                    => value.IsSetting && (value.Key == property.Name || value.PreviousKeys.Contains(property.Name)));
                if (field is null || !names.Add(field.Key)) return new(BridgeStatus.INVALID_ARGUMENT, $"Unknown or duplicate setting: {property.Name}.");
                try {
                    Dictionary<string, GameObject>? retained = purpose == SettingsValidationPurpose.AUTHORING
                        ? StructuredFieldCodec.ReferencePaths(field.Read(record.Instance), field.ValueType) : null;
                    Dictionary<string, GameObject[]>? byGuid = retained?.Values.GroupBy(static (GameObject reference) => reference.Guid, StringComparer.Ordinal)
                        .ToDictionary(static (IGrouping<string, GameObject> group) => group.Key, static (IGrouping<string, GameObject> group) => group.ToArray(), StringComparer.Ordinal);
                    Dictionary<string, HashSet<string>> incomingPaths = new(StringComparer.Ordinal);
                    GameObject ResolveReference(string path, string guid) {
                        string fullPath = StructuredFieldCodec.ChildPath("", field.Key) + path;
                        referenceLeaves.Add(fullPath);
                        if (!incomingPaths.TryGetValue(guid, out HashSet<string>? paths)) incomingPaths.Add(guid, paths = new(StringComparer.Ordinal));
                        paths.Add(path);
                        if (rebind.Contains(fullPath)) return ResolveGameObject(record, guid);
                        if (retained is not null && !rebind.Contains(fullPath) && retained.TryGetValue(path, out GameObject? previous) && previous.Guid == guid)
                            return previous;
                        if (byGuid is not null && byGuid.TryGetValue(guid, out GameObject[]? existing)) {
                            GameObject token = existing[0];
                            if (existing.Any((GameObject reference) => reference.ObjectId != token.ObjectId || reference.SceneLifetime != token.SceneLifetime))
                                throw new InvalidDataException($"Ambiguous retained GUID at {fullPath}; explicitly select the reference with rebindReferences.");
                            return token;
                        }
                        return ResolveGameObject(record, guid);
                    }
                    Dictionary<string, ComponentReferenceIdentity>? retainedComponents = purpose == SettingsValidationPurpose.AUTHORING
                        ? ComponentReferenceCodec.Paths(field.Read(record.Instance), field.ValueType) : null;
                    ComponentReferenceIdentity ResolveComponent(string path, ComponentReferenceIdentity address, Type wrapper) {
                        string fullPath = StructuredFieldCodec.ChildPath("", field.Key) + path;
                        referenceLeaves.Add(fullPath);
                        if (rebind.Contains(fullPath)) return ResolveComponentReferencePrepared(record, address, wrapper);
                        if (retainedComponents is not null && retainedComponents.TryGetValue(path, out ComponentReferenceIdentity? previous)
                            && ComponentReferenceCodec.SameAddress(previous, address)) return previous;
                        ComponentReferenceIdentity[] matches = retainedComponents?.Values.Where(value => ComponentReferenceCodec.SameAddress(value, address)).Distinct().ToArray() ?? [];
                        if (matches.Length > 1) throw new InvalidDataException($"Ambiguous Component lifetime at {fullPath}; supply an explicit rebind.");
                        return matches.Length == 1 ? matches[0] : ResolveComponentReferencePrepared(record, address, wrapper);
                    }
                    object? decoded = ScriptFieldCodec.DecodeJson(property.Value, field, (string guid) => ResolveGameObject(record, guid), ResolveReference, ResolveComponent);
                    if (retained is not null && byGuid is not null)
                        foreach (var group in byGuid) {
                            GameObject token = group.Value[0];
                            if (!group.Value.Any((GameObject reference) => reference.ObjectId != token.ObjectId || reference.SceneLifetime != token.SceneLifetime)
                                || !incomingPaths.TryGetValue(group.Key, out HashSet<string>? nextPaths)) continue;
                            HashSet<string> oldPaths = retained.Where((KeyValuePair<string, GameObject> pair) => pair.Value.Guid == group.Key)
                                .Select(static (KeyValuePair<string, GameObject> pair) => pair.Key).ToHashSet(StringComparer.Ordinal);
                            if (!oldPaths.SetEquals(nextPaths) && nextPaths.Any((string path) => !rebind.Contains(StructuredFieldCodec.ChildPath("", field.Key) + path)))
                                throw new InvalidDataException($"Ambiguous retained GUID layout in {field.Key}; explicitly select each retained reference with rebindReferences.");
                        }
                    prepared.Add(field, decoded);
                }
                catch (Exception exception) { return new(BridgeStatus.INVALID_ARGUMENT, $"{record.Type.TypeName}.{field.Key}: {Describe(exception)}"); }
            }
            if (!rebind.IsSubsetOf(referenceLeaves))
                return new(BridgeStatus.INVALID_ARGUMENT, "Reference rebind paths must identify nonempty reference leaves in the settings delta.");
            foreach (ScriptFieldDescriptor field in record.Type.Fields)
            {
                if (!field.IsSetting) continue;
                object? value = prepared.TryGetValue(field, out object? next) ? next : field.Read(record.Instance);
                if (!field.Validate(value) || (field.Kind == ScriptFieldKind.STRING && value is null))
                    return new(BridgeStatus.INVALID_ARGUMENT, $"CS_SETTING_VALUE {record.Type.TypeName}.{field.Key}: setting value is invalid.");
                if (purpose == SettingsValidationPurpose.RUNTIME
                    && !field.ValidateRequired(value, (GameObject reference) => IsAlivePrepared(record, reference), identity => IsComponentAlivePrepared(record, identity), out string path))
                    return new(BridgeStatus.INVALID_ARGUMENT, $"CS_REQUIRED_REFERENCE {record.Type.TypeName}.{field.Key}{path}: a required reference must be live; required collections must be nonempty with every element live.");
            }
            if (!validateOnly) {
                foreach (KeyValuePair<ScriptFieldDescriptor, object?> write in prepared) write.Key.AssignPrepared(record.Instance, write.Value);
                if (prepared.Count != 0) record.SettingsRevision++;
            }
            return new(BridgeStatus.OK);
        }
        catch (Exception exception) { return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }

    public ManagedRuntimeResult ValidateReferences(ulong generation, ulong instanceId)
    {
        ManagedRuntimeResult settings = ConfigureFields(generation, instanceId, "{}", validateOnly: true, SettingsValidationPurpose.RUNTIME);
        if (!settings.Success) return settings;
        if (!TryInstance(generation, instanceId, out ScriptInstance? record)) return new(BridgeStatus.STALE_GENERATION);
        try { return ValidateComponentRequirements(record); }
        catch (Exception exception) { return new(BridgeStatus.INVALID_ARGUMENT, Describe(exception)); }
    }

    internal GameObject ResolveGameObject(ScriptInstance record, string guid)
    {
        ScriptFieldCodec.ValidateGuid(guid);
        ulong id = m_bridge.ResolveGameObject(record.Generation, record.Binding.InstanceId, record.OwnerId, guid);
        return new GameObject(id, record.Binding.GameObject.SceneLifetime, guid);
    }
    bool IsAlivePrepared(ScriptInstance record, GameObject reference)
    {
        if (reference.ObjectId == 0 || reference.SceneLifetime != record.Binding.GameObject.SceneLifetime) return false;
        ScriptExecutionScope scope = new(m_bridge, this, record.Generation, record.Binding.InstanceId, record.OwnerId, 0, record.Binding.GameObject.SceneLifetime, ScriptCallback.ON_VALIDATE);
        return m_bridge.IsAlive(scope, reference.ObjectId);
    }
    HashSet<ScriptTypeDescriptor> ComponentTypes(ScriptExecutionScope scope, Type type)
    {
        if (ScriptLifecycleContract.GetExecutionPolicy(scope.Callback) == "draw_readonly")
            throw new InvalidOperationException("Gizmo callbacks cannot acquire managed Script instances.");
        if (!ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, scope) || Environment.CurrentManagedThreadId != m_threadId
            || !m_generations.TryGetValue(scope.Generation, out ScriptGeneration? generation)) throw new InvalidOperationException("Component lookup requires the current Host scope.");
        if (type.Assembly != typeof(Script).Assembly && AssemblyLoadContext.GetLoadContext(type.Assembly) != generation.Context)
            throw new InvalidOperationException("The requested CLR type belongs to an old code generation.");
        /// @note CLR 型の強参照は所有世代だけへ閉じ、RetireGeneration と同時に解放する。
        if (!generation.ComponentTypes.TryGetValue(type, out HashSet<ScriptTypeDescriptor>? matches))
        {
            matches = new();
            foreach (ScriptTypeDescriptor candidate in generation.Catalog.Types)
                if (type.IsAssignableFrom(candidate.Type)) matches.Add(candidate);
            generation.ComponentTypes.Add(type, matches);
        }
        return matches;
    }
    bool MatchesComponent(ScriptExecutionScope scope, GameObject target, ulong id, HashSet<ScriptTypeDescriptor> types,
        [System.Diagnostics.CodeAnalysis.NotNullWhen(true)] out ScriptInstance? value)
        => TryInstance(scope.Generation, id, out value) && value.Binding.Active
            && value.Binding.GameObject.ObjectId == target.ObjectId && value.Binding.GameObject.SceneLifetime == scope.SceneLifetime
            && types.Contains(value.Type);

    Component? IScriptComponentLookup.GetComponent(ScriptExecutionScope scope, GameObject target, Type type)
    {
        HashSet<ScriptTypeDescriptor> types = ComponentTypes(scope, type);
        Span<ulong> local = stackalloc ulong[64];
        uint offset = 0;
        while (true) {
            ManagedComponentPage page = m_bridge.ReadManagedComponents(scope, target.ObjectId, offset, local);
            ValidateComponentPage(page, offset, local.Length);
            foreach (ulong id in local[..page.Written])
                if (MatchesComponent(scope, target, id, types, out ScriptInstance? value)) return value.Instance;
            if (page.Complete) return null;
            offset = page.NextOffset;
        }
    }
    Component[] IScriptComponentLookup.GetComponents(ScriptExecutionScope scope, GameObject target, Type type)
    {
        HashSet<ScriptTypeDescriptor> types = ComponentTypes(scope, type);
        List<Component> values = new();
        Span<ulong> ids = stackalloc ulong[64];
        uint offset = 0;
        while (true) {
            ManagedComponentPage page = m_bridge.ReadManagedComponents(scope, target.ObjectId, offset, ids);
            ValidateComponentPage(page, offset, ids.Length);
            foreach (ulong id in ids[..page.Written])
                if (MatchesComponent(scope, target, id, types, out ScriptInstance? value)) values.Add(value.Instance);
            if (page.Complete) break;
            offset = page.NextOffset;
        }
        return values.ToArray();
    }
    CollectionReadResult IScriptComponentLookup.GetComponents<T>(ScriptExecutionScope scope, GameObject target, Span<T?> output) where T : class
    {
        HashSet<ScriptTypeDescriptor> types = ComponentTypes(scope, typeof(T));
        int required = VisitComponents(scope, target, types, Span<T?>.Empty, false);
        if (required > output.Length) return new(required, 0);
        if (required == 0) return new(0, 0);
        /// @note count 後の不正な後続 packet でも caller は未変更。pool は CLR 個体を保持しないよう返却時に消去する。
        T?[] staged = ArrayPool<T?>.Shared.Rent(required);
        try {
            int written = VisitComponents(scope, target, types, staged.AsSpan(0, required), true);
            if (written != required) throw new InvalidDataException("Component membership changed within a synchronous lookup.");
            staged.AsSpan(0, written).CopyTo(output);
            return new(required, written);
        }
        finally { ArrayPool<T?>.Shared.Return(staged, clearArray: true); }
    }
    int VisitComponents<T>(ScriptExecutionScope scope, GameObject target, HashSet<ScriptTypeDescriptor> types,
        Span<T?> output, bool write) where T : Component
    {
        Span<ulong> ids = stackalloc ulong[64];
        uint offset = 0;
        int count = 0;
        while (true) {
            ManagedComponentPage page = m_bridge.ReadManagedComponents(scope, target.ObjectId, offset, ids);
            ValidateComponentPage(page, offset, ids.Length);
            foreach (ulong id in ids[..page.Written])
                if (MatchesComponent(scope, target, id, types, out ScriptInstance? value)) {
                    if (write) {
                        if (count >= output.Length) throw new InvalidDataException("Component membership changed within a synchronous lookup.");
                        output[count] = (T)(Component)value.Instance;
                    }
                    count = checked(count + 1);
                }
            if (page.Complete) return count;
            offset = page.NextOffset;
        }
    }
    static void ValidateComponentPage(ManagedComponentPage page, uint offset, int capacity)
    {
        if (page.Written < 0 || page.Written > capacity || page.NextOffset < offset
            || (!page.Complete && page.NextOffset == offset)) throw new InvalidDataException("Component page has invalid progress.");
    }

    public ManagedRuntimeResult RetireGeneration(ulong generation)
    {
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (m_generationLeases.ContainsKey(generation))
            return new(BridgeStatus.PENDING_LIMIT, "Generation is retained by an Edit or other owner; release its lease before retirement.");
        if (!m_generations.Remove(generation, out ScriptGeneration? record)) return new(BridgeStatus.STALE_GENERATION);
        DiscardGenerationSettings(generation);
        CloseOwnedSettingsEdits(generation);
        ReleaseObservations(generation);
        CloseGenerationDataAssetDocuments(generation);
        foreach (ulong id in m_instances.Where((KeyValuePair<ulong, ScriptInstance> value) => value.Value.Generation == generation).Select((KeyValuePair<ulong, ScriptInstance> value) => value.Key).ToArray())
        { m_instances[id].Binding.Assets.InvalidateOwned(); m_instances[id].Binding.Active = false; RemoveBoundScript(m_instances[id]); ClearOwnedJobs(generation, id); m_instances.Remove(id); }
        RetireContext(record.Context);
        return new(BridgeStatus.OK);
    }

    /// @note weak reference の監視だけを行う。容量不足の回収要求 budget は候補名を替えても共有し、回収進捗でリセットする。
    public void CollectRetired()
    {
        if (!IsPreparationThread()) return;
        int removed = m_pending.RemoveAll((WeakReference context) => !context.IsAlive);
        if (removed != 0 || m_pending.Count < MAX_PENDING_CONTEXTS) m_collectionRequests = 0;
    }

    internal bool TryInstance(ulong generation, ulong instanceId, [System.Diagnostics.CodeAnalysis.NotNullWhen(true)] out ScriptInstance? instance)
    {
        if (m_instances.TryGetValue(instanceId, out instance) && instance.Generation == generation) return true;
        instance = null;
        return false;
    }
    bool IsPreparationThread() => Environment.CurrentManagedThreadId == m_threadId && ScriptApiRuntime.Current is null;
    ManagedRuntimeResult Reject(GameLoadContext context, string message) { RetireContext(context); return new(BridgeStatus.LOAD_FAILED, message); }
    /// @note finalization 前の weak 消失を解放完了と扱わず、復活追跡付き weak reference で監視する。
    /// @see https://learn.microsoft.com/en-us/dotnet/standard/assembly/unloadability Use a custom collectible AssemblyLoadContext。
    void RetireContext(GameLoadContext context) { m_pending.Add(new WeakReference(context, trackResurrection: true)); context.Unload(); }
    static string? ValidateSettings(ScriptTypeDescriptor type, Script instance)
    {
        foreach (ScriptFieldDescriptor field in type.Fields)
        {
            if (!field.IsSetting && !field.IsReloadState && field.Range is null) continue;
            object? value = field.Read(instance);
            if (!field.Validate(value) || (field.IsSetting && field.Kind == ScriptFieldKind.STRING && value is null))
                return $"CS_DEFAULT_VALUE {type.TypeName}.{field.Key}: setting defaults must be finite, declared enum values, and non-null strings.";
        }
        return null;
    }
    internal static string Describe(Exception exception)
    {
        Exception actual = exception;
        if (exception is TargetInvocationException wrapper && wrapper.InnerException is Exception inner) actual = inner;
        string location = actual.StackTrace is string trace ? $"\n{trace}" : "";
        return $"{actual.GetType().FullName}: {actual.Message}{location}";
    }
    public void Dispose()
    {
        if (!IsPreparationThread()) return;
        CloseSettingsAdoptions();
        m_generationLeases.Clear();
        foreach (ulong generation in m_generations.Keys.ToArray()) RetireGeneration(generation);
    }

    sealed record ScriptGeneration(GameLoadContext Context, ScriptCatalog Catalog, string Diagnostics, bool InternalMode)
    {
        internal Dictionary<Type, HashSet<ScriptTypeDescriptor>> ComponentTypes { get; } = new();
    }
    internal sealed class ScriptInstance
    {
        internal ScriptInstance(ulong generation, ulong owner, ScriptTypeDescriptor type, Script instance, ScriptBinding binding, Dictionary<ScriptCallback, Delegate> callbacks)
        { Generation = generation; OwnerId = owner; Type = type; Instance = instance; Binding = binding; Callbacks = callbacks; }
        internal ulong Generation { get; }
        internal ulong OwnerId { get; }
        internal ScriptTypeDescriptor Type { get; }
        internal Script Instance { get; }
        internal ScriptBinding Binding { get; }
        internal Dictionary<ScriptCallback, Delegate> Callbacks { get; }
        internal bool Faulted { get; set; }
        internal ulong SettingsRevision { get; set; } = 1;
    }

    sealed class GameLoadContext : AssemblyLoadContext
    {
        internal GameLoadContext(string path) : base(isCollectible: true) { m_resolver = new AssemblyDependencyResolver(path); }
        readonly AssemblyDependencyResolver m_resolver;
        /// @see https://learn.microsoft.com/en-us/dotnet/core/dependency-loading/understanding-assemblyloadcontext shared assembly identity。
        protected override Assembly? Load(AssemblyName name)
        {
            Assembly api = typeof(Script).Assembly;
            if (name.Name == api.GetName().Name)
            {
                if (name.Version != api.GetName().Version) throw new InvalidOperationException("Game ScriptApi reference does not match the resident API.");
                return api;
            }
            string? path = m_resolver.ResolveAssemblyToPath(name);
            return path is null ? null : LoadFromAssemblyPath(path);
        }
        protected override nint LoadUnmanagedDll(string name)
        {
            string? path = m_resolver.ResolveUnmanagedDllToPath(name);
            return path is null ? 0 : LoadUnmanagedDllFromPath(path);
        }
    }

    sealed class UnavailableBridge : IScriptNativeBridge
    {
        static InvalidOperationException Missing() => new("Native services are not connected.");
        public bool IsAlive(ScriptExecutionScope scope, ulong ownerId) => throw Missing();
        public Vector3 ReadPosition(ScriptExecutionScope scope, ulong ownerId, bool world) => throw Missing();
        public void WritePosition(ScriptExecutionScope scope, ulong ownerId, Vector3 value, bool world) => throw Missing();
        public void TranslateLocal(ScriptExecutionScope scope, ulong ownerId, Vector3 delta) => throw Missing();
        public Quaternion ReadRotation(ScriptExecutionScope scope, ulong objectId, bool world) => throw Missing();
        public void WriteLocalRotation(ScriptExecutionScope scope, ulong objectId, Quaternion value) => throw Missing();
        public Vector3 ReadLocalScale(ScriptExecutionScope scope, ulong objectId) => throw Missing();
        public void WriteLocalScale(ScriptExecutionScope scope, ulong objectId, Vector3 value) => throw Missing();
        public Vector3 ReadDirection(ScriptExecutionScope scope, ulong objectId, uint direction) => throw Missing();
        public void Rotate(ScriptExecutionScope scope, ulong objectId, Vector3 axis, float degrees) => throw Missing();
        public void LookAt(ScriptExecutionScope scope, ulong objectId, Vector3 worldTarget) => throw Missing();
        public BatchResult ReadWorldPositions(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects, Span<Vector3> positions, Span<ObjectStatus> statuses) => throw Missing();
        public BatchResult SetWorldPositions(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects, ReadOnlySpan<Vector3> positions, Span<ObjectStatus> statuses) => throw Missing();
        public BatchResult TranslateWorld(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects, Vector3 displacement, bool activeOnly) => throw Missing();
        public bool ReadKey(ScriptExecutionScope scope, KeyCode key, uint state) => throw Missing();
        public float ReadAction(ScriptExecutionScope scope, string action, uint state) => throw Missing();
        public float DeltaSeconds(ScriptExecutionScope scope) => throw Missing();
        public ulong ResolveGameObject(ulong generation, ulong instanceId, ulong ownerId, string guid) => throw Missing();
        public ulong[] GetManagedComponents(ScriptExecutionScope scope, ulong objectId) => throw Missing();
        public string GetObjectName(ScriptExecutionScope scope, ulong objectId) => throw Missing();
        public GameObject? FindByName(ScriptExecutionScope scope, string name, bool includeInactive) => throw Missing();
        public GameObject? FindChild(ScriptExecutionScope scope, ulong objectId, ReadOnlySpan<string> names, bool includeInactive) => throw Missing();
        public GameObject? CreateObject(ScriptExecutionScope scope, string name, Vector3 position, Quaternion rotation, bool usePose) => throw Missing();
        public GameObject? InstantiateObject(ScriptExecutionScope scope, string prefab, Vector3 position, Quaternion rotation, bool usePose) => throw Missing();
        public bool CanCreate(ScriptExecutionScope scope, int count) => throw Missing();
        public bool DestroyObject(ScriptExecutionScope scope, ulong objectId, float delay) => throw Missing();
        public BatchResult DestroyObjects(ScriptExecutionScope scope, ReadOnlySpan<GameObject?> objects, float delay) => throw Missing();
        public Vector3 ReadPhysicsVector(ScriptExecutionScope scope, ulong objectId, uint field) => throw Missing();
        public float ReadPhysicsScalar(ScriptExecutionScope scope, ulong objectId, uint field) => throw Missing();
        public bool ReadPhysicsFlag(ScriptExecutionScope scope, ulong objectId, uint field) => throw Missing();
        public void WritePhysicsVector(ScriptExecutionScope scope, ulong objectId, uint field, Vector3 value) => throw Missing();
        public void WritePhysicsScalar(ScriptExecutionScope scope, ulong objectId, uint field, float value) => throw Missing();
        public void WritePhysicsFlag(ScriptExecutionScope scope, ulong objectId, uint field, bool value) => throw Missing();
        public void PhysicsBodyCommand(ScriptExecutionScope scope, ulong objectId, uint command, Vector3 value, Vector3 point, uint axes) => throw Missing();
        public Vector3 ReadWorldGravity(ScriptExecutionScope scope) => throw Missing();
        public bool LayersCollide(ScriptExecutionScope scope, int a, int b) => throw Missing();
        public bool Raycast(ScriptExecutionScope scope, Vector3 origin, Vector3 direction, float maxDistance, out RaycastHit hit) => throw Missing();
        public bool SphereCast(ScriptExecutionScope scope, Vector3 center, float radius, Vector3 direction, float maxDistance, out RaycastHit hit) => throw Missing();
        public RaycastHit[] RaycastAll(ScriptExecutionScope scope, Vector3 origin, Vector3 direction, float maxDistance) => throw Missing();
        public GameObject[] OverlapSphere(ScriptExecutionScope scope, Vector3 center, float radius) => throw Missing();
    }
}
