/// @file    Script.cs
/// @brief   保持可能な非所有参照と現在の Host callback で検証する Script API。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Runtime.CompilerServices;

namespace Fbzz;

/// @note 容量不足時は Written=0、呼出元の出力は未変更。Required は全件数で打ち切らない。
public readonly record struct CollectionReadResult(int Required, int Written)
{
    public bool Complete => Required == Written;
}
internal readonly record struct ManagedComponentPage(int Written, uint NextOffset, bool Complete);

/// @note Script 実体はコード世代、native facade は対象の寿命に属する。
public abstract class Component
{
    public override bool Equals(object? other) => ReferenceEquals(this, other);
    public override int GetHashCode() => RuntimeHelpers.GetHashCode(this);
    public static bool operator ==(Component? left, Component? right)
        => ReferenceEquals(left, right) || left is not null && left.Equals(right);
    public static bool operator !=(Component? left, Component? right) => !(left == right);
    internal ScriptBinding? Binding { get; set; }
    public virtual GameObject GameObject => RequireBinding().GameObject;
    public Transform Transform => GameObject.Transform;
    public virtual bool IsAlive
    {
        get
        {
            if (Binding is not ScriptBinding binding || !binding.Active) return false;
            ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
            return binding.Generation == scope.Generation && binding.GameObject.SceneLifetime == scope.SceneLifetime
                && scope.Bridge.IsAlive(scope, binding.GameObject.ObjectId);
        }
    }
    [ApiContract(Failure = "null_when_no_matching_component; unsupported_type_or_invalid_scope_throws", Call = "read", AccessCondition = "native_component_generic_argument", ConditionalCall = "query")]
    public T? GetComponent<T>() where T : Component => GameObject.GetComponent<T>();
    [ApiContract(Failure = "empty_when_no_matching_component; ScriptEntry_order; unsupported_type_or_invalid_scope_throws", Call = "read", AccessCondition = "native_component_generic_argument", ConditionalCall = "query")]
    public T[] GetComponents<T>() where T : Component => GameObject.GetComponents<T>();
    /// @return 不足時は Required と Written=0、output は未変更。成功時は全件を書き、未使用の末尾を保持する。
    [ApiContract(Call = "read", AccessCondition = "native_component_generic_argument", ConditionalCall = "query", Summary = "Copies every matching component into caller storage. After type-cache warmup no temporary collections are allocated; insufficient capacity reports Required and Written=0 without changing output.", Failure = "insufficient_capacity_returns_required_and_written_zero_without_output_changes")]
    public CollectionReadResult GetComponents<T>(Span<T?> output) where T : Component => GameObject.GetComponents(output);
    internal ScriptBinding RequireBinding()
    {
        ScriptBinding binding = Binding ?? throw new InvalidOperationException("Engine API is unavailable during script construction.");
        ScriptApiRuntime.RequireScope(binding);
        return binding;
    }
}

/// @brief Override only the synchronous lifecycle callbacks needed by this Script.
public abstract partial class Script : Component
{
    static readonly ScriptGizmos s_gizmos = new();
    public ScriptGizmos Gizmos => s_gizmos;
    public ScriptTime Time => RequireBinding().Time;
    public ScriptInput Input => RequireBinding().Input;
    public ScriptScene Scene => RequireBinding().Scene;
    public ScriptObjects Objects => RequireBinding().Objects;
    public ScriptPhysics Physics => RequireBinding().Physics;
    public ScriptApplication App => RequireBinding().App;
    public ScriptSave Save => RequireBinding().Save;
    public ScriptConfig Config => RequireBinding().Config;
    public ScriptRender Render => RequireBinding().Render;
    public ScriptDisplay Display => RequireBinding().Display;
    public ScriptAssets Assets => RequireBinding().Assets;
    [ApiContract(Read = "subscription")]
    public ScriptCursor Cursor => RequireBinding().Cursor;
    public ScriptAudio Audio => GameObject.Audio;
    public UI UI => GameObject.UI;
    [ApiContract(Read = "subscription")]
    public ScriptJobs Jobs => RequireBinding().Jobs;
    [ApiContract(Read = "subscription", Summary = "Gets this script event service; Connect may use it to register subscriptions.")]
    public ScriptEvents Events => RequireBinding().Events;
    public ScriptDataAssets DataAssets => RequireBinding().DataAssets;

    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Lifetime = "anchor_exact_current_scene_entity; spawned_object_has_independent_lifetime", Summary = "Instantiates one prefab root at a live anchor's world pose captured within the synchronous callback. Later anchor changes do not move the spawned object.", Failure = "missing_prefab_or_capacity_returns_null; expired_foreign_scene_anchor_or_scope_throws_before_creation")]
    public GameObject? InstantiateAt(string prefab, GameObject anchor) => Scene.InstantiateAt(prefab, anchor);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "localOffset=m" }, ParameterCoordinates = new[] { "localOffset=anchor_local_axes_with_signed_world_scale" }, Summary = "Captures the anchor pose with a local offset transformed by the Engine's accumulated signed world scale and rotation; the spawned object does not follow the anchor.", Failure = "missing_prefab_or_capacity_returns_null; invalid_offset_expired_foreign_scene_anchor_or_scope_throws_before_creation")]
    public GameObject? InstantiateAt(string prefab, GameObject anchor, Vector3 localOffset) => Scene.InstantiateAt(prefab, anchor, localOffset);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "localOffset=m", "rotationOffset=unit_quaternion_xyzw" }, ParameterCoordinates = new[] { "localOffset=anchor_local_axes_with_signed_world_scale", "rotationOffset=anchor_local_rotation" }, Summary = "Captures position and anchorWorldRotation * rotationOffset during the synchronous callback; the spawned object keeps its independent pose.", Failure = "missing_prefab_or_capacity_returns_null; nonfinite_or_nonunit_offset_expired_foreign_scene_anchor_or_scope_throws_before_creation")]
    public GameObject? InstantiateAt(string prefab, GameObject anchor, Vector3 localOffset, Quaternion rotationOffset)
        => Scene.InstantiateAt(prefab, anchor, localOffset, rotationOffset);
}

/// @note GUID は保存用、ObjectId は解決時の Scene / Entity 寿命に属する。同じ GUID の再生成で旧参照を復活させない。
public sealed partial class GameObject
{
    internal GameObject(ulong objectId, ulong sceneLifetime, string guid)
    { ObjectId = objectId; SceneLifetime = sceneLifetime; Guid = guid; Transform = new Transform(this); }
    internal ulong ObjectId { get; }
    internal ulong SceneLifetime { get; }
    internal string Guid { get; }
    public Transform Transform { get; }
    UI? m_ui;
    ScriptAudio? m_audio;
    Dictionary<uint, Component>? m_nativeComponents;
    /// @note この所有者を対象に UI サービスを照会する。個々の widget の寿命は型付き Component で扱う。
    public UI UI => m_ui ??= new UI(this);
    public ScriptAudio Audio => m_audio ??= new ScriptAudio(this);
    public string Name => ScriptApiRuntime.RequireScope().Bridge.GetObjectName(ScriptApiRuntime.RequireScope(), ObjectId);
    [ApiContract(Failure = "false_for_unresolved_or_expired_identity; invalid_callback_scope_throws")]
    public bool IsAlive
   {
        get
        {
            ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
            return scope.Bridge.IsAlive(scope, ObjectId);
        }
    }
    /// @return ScriptEntry 順の最初の型一致。未検出・Missing・失効対象は null。
    [ApiContract(Failure = "null_when_no_matching_component; unsupported_type_or_invalid_scope_throws", Call = "read", AccessCondition = "native_component_generic_argument", ConditionalCall = "query")]
    public T? GetComponent<T>() where T : Component
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        Type type = typeof(T);
        if (type == typeof(Component)) throw new NotSupportedException("Heterogeneous Component lookup is not exposed.");
        if (type == typeof(Transform)) return IsAlive ? (T)(Component)Transform : null;
        uint kind = NativeComponentFactory.GetKind(type);
        if (kind != uint.MaxValue)
            return (T?)NativeComponent(type, kind, scope);
        if (!typeof(Script).IsAssignableFrom(type)) throw new NotSupportedException("This native Component facade is not exposed.");
        return (T?)scope.Components.GetComponent(scope, this, type);
    }
    /// @note 通常配列を返す。enabled / 開始済み / faulted は取得条件にしない。
    [ApiContract(Failure = "empty_when_no_matching_component; ScriptEntry_order; unsupported_type_or_invalid_scope_throws", Call = "read", AccessCondition = "native_component_generic_argument", ConditionalCall = "query")]
    public T[] GetComponents<T>() where T : Component
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        if (typeof(T) == typeof(Component)) throw new NotSupportedException("Heterogeneous Component lookup is not exposed.");
        if (typeof(T) == typeof(Transform)) return IsAlive ? [(T)(Component)Transform] : [];
        uint kind = NativeComponentFactory.GetKind(typeof(T));
        if (kind != uint.MaxValue)
        {
            T? value = (T?)NativeComponent(typeof(T), kind, scope);
            return value is null ? [] : [value];
        }
        if (!typeof(Script).IsAssignableFrom(typeof(T))) throw new NotSupportedException("This native Component facade is not exposed.");
        Component[] values = scope.Components.GetComponents(scope, this, typeof(T));
        T[] typed = new T[values.Length];
        for (int index = 0; index < values.Length; index++) typed[index] = (T)values[index];
        return typed;
    }
    /// @note 型照合 cache の初回構築後は集合の一時配列を確保しない。全件の count と copy に user callback を挟まない。
    /// @return 不足時は Required と Written=0、output は未変更。成功時は全件を書き、未使用の末尾を保持する。
    [ApiContract(Call = "read", AccessCondition = "native_component_generic_argument", ConditionalCall = "query", Summary = "Copies every matching component into caller storage. After type-cache warmup no temporary collections are allocated; insufficient capacity reports Required and Written=0 without changing output.", Failure = "insufficient_capacity_returns_required_and_written_zero_without_output_changes")]
    public CollectionReadResult GetComponents<T>(Span<T?> output) where T : Component
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        Type type = typeof(T);
        if (type == typeof(Component)) throw new NotSupportedException("Heterogeneous Component lookup is not exposed.");
        if (type == typeof(Transform) || NativeComponentFactory.GetKind(type) != uint.MaxValue)
        {
            T? value = GetComponent<T>();
            int count = value is null ? 0 : 1;
            if (output.Length < count) return new(count, 0);
            if (count != 0) output[0] = value;
            return new(count, count);
        }
        if (!typeof(Script).IsAssignableFrom(type)) throw new NotSupportedException("This native Component facade is not exposed.");
        return scope.Components.GetComponents(scope, this, output);
    }
    Component? NativeComponent(Type type, uint kind, ScriptExecutionScope scope)
    {
        if (SceneLifetime != scope.SceneLifetime) throw new InvalidOperationException("The Component belongs to another scene.");
        ulong lifetime = scope.Bridge.ReadComponentLifetime(scope, ObjectId, kind);
        if (lifetime == 0) { m_nativeComponents?.Remove(kind); return null; }
        m_nativeComponents ??= new();
        if (m_nativeComponents.TryGetValue(kind, out Component? existing)
            && NativeComponentFactory.Lifetime(existing) == lifetime) return existing;
        Component? value = NativeComponentFactory.Create(type, this, lifetime);
        if (value is not null) m_nativeComponents[kind] = value;
        return value;
    }
    /// @note 各 names は直下の実体名。空経路・空要素・複数一致を拒否し、未検出は false と null。
    [ApiContract(Failure = "false_and_null_when_missing; empty_path_empty_segment_or_ambiguous_match_throws", Call = "read")]
    public bool TryFindChild(ReadOnlySpan<string> names, [NotNullWhen(true)] out GameObject? child, bool includeInactive = true)
    {
        if (names.IsEmpty) throw new ArgumentException("Child path must contain at least one name.", nameof(names));
        foreach (string name in names) ArgumentException.ThrowIfNullOrEmpty(name);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        child = scope.Bridge.FindChild(scope, ObjectId, names, includeInactive);
        return child is not null;
    }

    /// @note Play の同期 callback で既存破棄キューへ要求する。delay は秒、寿命は Cleanup まで維持する。
    /// @return 失効した対象は false。要求済みでも即時に参照を失効させない。
    [ApiContract(ParameterUnits = new[] { "delay=s" }, Failure = "false_for_expired_target; invalid_delay_or_scope_throws; destruction_is_queued_until_Cleanup")]
    public bool Destroy(float delay = 0f)
    {
        if (!float.IsFinite(delay) || delay < 0f) throw new ArgumentOutOfRangeException(nameof(delay));
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        if (SceneLifetime != scope.SceneLifetime) throw new InvalidOperationException("The target belongs to another scene.");
        return scope.Bridge.DestroyObject(scope, ObjectId, delay);
    }
}

/// @note 自分と同じ Scene を検索する。現在の callback とサービス所有者の寿命を照合する。
public sealed partial class ScriptScene
{
    internal ScriptScene(ScriptBinding owner) { m_owner = owner; }
    readonly ScriptBinding m_owner;
    /// @return 有効な一致が一個なら true。零個は false / null、複数一致は診断付きの拒否。
    [ApiContract(Failure = "false_and_null_when_missing; ambiguous_match_or_invalid_name_scope_throws", Call = "read")]
    public bool TryFind(string name, [NotNullWhen(true)] out GameObject? result, bool includeInactive = false)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        result = scope.Bridge.FindByName(scope, name, includeInactive);
        return result is not null;
    }

    /// @return 容量不足なら null。作成は Play の同期 callback だけ許可する。
    [ApiContract(Failure = "null_for_capacity_or_missing_prefab; invalid_arguments_or_scope_throws; constructor_has_no_live_scope; lifecycle_runs_from_ScriptSystem")]
    public GameObject? Create(string name = "GameObject")
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.CreateObject(scope, name, Vector3.ZERO, Quaternion.IDENTITY, false);
    }

    [ApiContract(ParameterUnits = new[] { "position=m" }, ParameterCoordinates = new[] { "position=world" }, Failure = "null_for_capacity_or_missing_prefab; invalid_arguments_or_scope_throws; constructor_has_no_live_scope; lifecycle_runs_from_ScriptSystem")]
    public GameObject? Create(string name, Vector3 position, Quaternion rotation)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        if (!position.IsFinite || !rotation.IsFinite || !float.IsFinite(rotation.Length) || rotation.Length < Mathf.EPSILON)
            throw new ArgumentOutOfRangeException(nameof(position));
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.CreateObject(scope, name, position, rotation, true);
    }

    /// @note prefab は guid: 参照またはプロジェクト相対の .prefab path。single-root の既存 Prefab 復元を使う。
    /// @return アセット不在・容量不足なら null。Awake / Start は既存 ScriptSystem が駆動する。
    [ApiContract(Failure = "null_for_capacity_or_missing_prefab; invalid_arguments_or_scope_throws; constructor_has_no_live_scope; lifecycle_runs_from_ScriptSystem")]
    public GameObject? Instantiate(string prefab)
    {
        ArgumentException.ThrowIfNullOrEmpty(prefab);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.InstantiateObject(scope, prefab, Vector3.ZERO, Quaternion.IDENTITY, false);
    }

    /// @note position はワールド [m]。rotation は有限の非退化値を正規化して使用する。
    [ApiContract(ParameterUnits = new[] { "position=m" }, ParameterCoordinates = new[] { "position=world" }, Failure = "null_for_capacity_or_missing_prefab; invalid_arguments_or_scope_throws; constructor_has_no_live_scope; lifecycle_runs_from_ScriptSystem")]
    public GameObject? Instantiate(string prefab, Vector3 position, Quaternion rotation)
    {
        ArgumentException.ThrowIfNullOrEmpty(prefab);
        if (!position.IsFinite || !rotation.IsFinite || !float.IsFinite(rotation.Length) || rotation.Length < Mathf.EPSILON)
            throw new ArgumentOutOfRangeException(nameof(position));
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.InstantiateObject(scope, prefab, position, rotation, true);
    }

    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Lifetime = "anchor_exact_current_scene_entity; spawned_object_has_independent_lifetime", Summary = "Captures a live anchor's world pose within this synchronous callback and instantiates a prefab at the captured pose. Subsequent anchor motion does not affect the new object.", Failure = "missing_prefab_or_capacity_returns_null; expired_foreign_scene_anchor_or_scope_throws_before_creation")]
    public GameObject? InstantiateAt(string prefab, GameObject anchor) => InstantiateAt(prefab, anchor, Vector3.ZERO, Quaternion.IDENTITY);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "localOffset=m" }, ParameterCoordinates = new[] { "localOffset=anchor_local_axes_with_signed_world_scale" }, Failure = "missing_prefab_or_capacity_returns_null; invalid_offset_expired_foreign_scene_anchor_or_scope_throws_before_creation")]
    public GameObject? InstantiateAt(string prefab, GameObject anchor, Vector3 localOffset)
        => InstantiateAt(prefab, anchor, localOffset, Quaternion.IDENTITY);
    /// @note World pose reads finish before the existing Prefab creation boundary suspends managed Engine access.
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "localOffset=m", "rotationOffset=unit_quaternion_xyzw" }, ParameterCoordinates = new[] { "localOffset=anchor_local_axes_with_signed_world_scale", "rotationOffset=anchor_local_rotation" }, Summary = "Captures the pose within the synchronous callback using existing reads; position includes signed ancestor scale, and rotation is anchorWorldRotation * rotationOffset. No live-follow relationship is added.", Failure = "missing_prefab_or_capacity_returns_null; nonfinite_or_nonunit_offset_expired_foreign_scene_anchor_or_scope_throws_before_creation")]
    public GameObject? InstantiateAt(string prefab, GameObject anchor, Vector3 localOffset, Quaternion rotationOffset)
    {
        ArgumentException.ThrowIfNullOrEmpty(prefab);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        CapturedPlacementPose pose = PlacementPoseCapture.Capture(scope, anchor, localOffset, rotationOffset);
        return scope.Bridge.InstantiateObject(scope, prefab, pose.Position, pose.Rotation, true);
    }

    /// @note count は root 数ではなく子孫を含む Entity 枠数。容量を予約する操作ではない。
    [ApiContract(Call = "read")]
    public bool CanCreate(int count = 1)
    {
        if (count < 0) throw new ArgumentOutOfRangeException(nameof(count));
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.CanCreate(scope, count);
    }

    public bool Destroy(GameObject? target, float delay = 0f)
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        if (!float.IsFinite(delay) || delay < 0f) throw new ArgumentOutOfRangeException(nameof(delay));
        if (target is null) return false;
        if (target.SceneLifetime != scope.SceneLifetime) throw new InvalidOperationException("The target belongs to another scene.");
        return scope.Bridge.DestroyObject(scope, target.ObjectId, delay);
    }

    /// @note 全入力を検証してから固有対象を破棄キューへ要求する。null・失効・重複はスキップ数へ含める。
    public BatchResult DestroyMany(ReadOnlySpan<GameObject?> objects, float delay = 0f)
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        if (!float.IsFinite(delay) || delay < 0f) return new BatchResult(BatchStatus.INVALID_ARGUMENT, objects.Length, 0, 0);
        foreach (GameObject? target in objects)
            if (target is not null && target.SceneLifetime != scope.SceneLifetime)
                return new BatchResult(BatchStatus.SCENE_MISMATCH, objects.Length, 0, 0);
        return scope.Bridge.DestroyObjects(scope, objects, delay);
    }
}

/// @note 各操作で現在の callback と対象寿命を照合する。取得時の権限は保持しない。
public sealed class Transform : Component, IEquatable<Transform>
{
    internal Transform(GameObject owner) { m_owner = owner; }
    readonly GameObject m_owner;
    public bool Equals(Transform? other) => other is not null && m_owner.Equals(other.m_owner);
    public override bool Equals(object? other) => other is Transform transform && Equals(transform);
    public override int GetHashCode() => HashCode.Combine(m_owner, 0U);
    public override GameObject GameObject => m_owner;
    public override bool IsAlive => m_owner.IsAlive;
    [ApiContract(Units = "m", Coordinate = "local_parent_space", Failure = "invalid_scope_lifetime_or_nonfinite_write_throws; local_is_authoritative; write_synchronizes_parent_and_descendants")]
    public Vector3 LocalPosition
   {
        get { ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(); return scope.Bridge.ReadPosition(scope, m_owner.ObjectId, false); }
        set { ValidateFinite(value); ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(); scope.Bridge.WritePosition(scope, m_owner.ObjectId, value, false); }
    }
    [ApiContract(Units = "m", Coordinate = "world", Failure = "invalid_scope_lifetime_or_nonfinite_write_throws; local_is_authoritative; write_synchronizes_parent_and_descendants")]
    public Vector3 WorldPosition
   {
        get { ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(); return scope.Bridge.ReadPosition(scope, m_owner.ObjectId, true); }
        set { ValidateFinite(value); ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(); scope.Bridge.WritePosition(scope, m_owner.ObjectId, value, true); }
    }
    /// @note ローカルの正本。有限で長さ EPSILON 以上の値だけを書き、入力の成分を保持する。子孫のワールド姿勢へ即時反映する。
    [ApiContract(Units = "unitless_quaternion_xyzw", Coordinate = "local", Failure = "nonfinite_or_length_below_EPSILON_write_throws; write_synchronizes_descendants")]
    public Quaternion LocalRotation
   {
        get { ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(); return scope.Bridge.ReadRotation(scope, m_owner.ObjectId, false); }
        set
        {
            float length = value.Length;
            if (!value.IsFinite || !float.IsFinite(length) || length < Mathf.EPSILON)
                throw new ArgumentOutOfRangeException(nameof(value), "Rotation must be finite and nondegenerate.");
            ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
            scope.Bridge.WriteLocalRotation(scope, m_owner.ObjectId, value);
        }
    }
    /// @note 親子同期済みのワールド回転。ローカル設定は LocalRotation を使う。
    [ApiContract(Units = "unitless_quaternion_xyzw", Coordinate = "world")]
    public Quaternion WorldRotation
   {
        get { ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(); return scope.Bridge.ReadRotation(scope, m_owner.ObjectId, true); }
    }
    /// @note 零・負の成分も許可するローカル拡縮。親と子孫のワールド姿勢へ即時反映する。
    [ApiContract(Units = "ratio", Coordinate = "local", Failure = "invalid_scope_lifetime_or_nonfinite_write_throws; zero_and_negative_components_allowed")]
    public Vector3 LocalScale
   {
        get { ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(); return scope.Bridge.ReadLocalScale(scope, m_owner.ObjectId); }
        set { ValidateFinite(value); ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(); scope.Bridge.WriteLocalScale(scope, m_owner.ObjectId, value); }
    }
    [ApiContract(Units = "unit_direction", Coordinate = "world")]
    public Vector3 Forward => ReadDirection(0);
    [ApiContract(Units = "unit_direction", Coordinate = "world")]
    public Vector3 Right => ReadDirection(1);
    [ApiContract(Units = "unit_direction", Coordinate = "world")]
    public Vector3 Up => ReadDirection(2);
    Vector3 ReadDirection(uint direction)
    { ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(); return scope.Bridge.ReadDirection(scope, m_owner.ObjectId, direction); }

    /// @note degrees は [deg]。C++ proxy と同じ delta * LocalRotation の順で回す。長さ EPSILON 未満の軸は native の no-op。
    [ApiContract(ParameterUnits = new[] { "axis=unit_direction", "degrees=deg" }, ParameterCoordinates = new[] { "axis=local_axes" })]
    public void Rotate(Vector3 axis, float degrees)
    {
        ValidateFinite(axis);
        if (!float.IsFinite(degrees)) throw new ArgumentOutOfRangeException(nameof(degrees), "Angle must be finite.");
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        scope.Bridge.Rotate(scope, m_owner.ObjectId, axis, degrees);
    }
    /// @note worldTarget はワールド位置 [m]。重なる対象は native の退化閾値で回転を維持する。
    [ApiContract(ParameterUnits = new[] { "worldTarget=m" }, ParameterCoordinates = new[] { "worldTarget=world" })]
    public void LookAt(Vector3 worldTarget)
    {
        ValidateFinite(worldTarget);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        scope.Bridge.LookAt(scope, m_owner.ObjectId, worldTarget);
    }
    /// @note delta はローカル軸に沿ったメートル単位の変位。
    [ApiContract(ParameterUnits = new[] { "delta=m" }, ParameterCoordinates = new[] { "delta=local_axes" })]
    public void TranslateLocal(Vector3 delta)
    {
        ValidateFinite(delta);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        scope.Bridge.TranslateLocal(scope, m_owner.ObjectId, delta);
    }
    /// @note delta はワールド軸のメートル単位。既存 world setter で親子を即時同期する。
    [ApiContract(ParameterUnits = new[] { "delta=m" }, ParameterCoordinates = new[] { "delta=world_axes" })]
    public void TranslateWorld(Vector3 delta) { ValidateFinite(delta); WorldPosition += delta; }
    /// @note destination はワールド位置、距離は非負のメートル。時間は内部で掛けない。
    [ApiContract(ParameterUnits = new[] { "destination=m", "maxDistance=m", "stopDistance=m" }, ParameterCoordinates = new[] { "destination=world" })]
    public void MoveTowardsWorld(Vector3 destination, float maxDistance, float stopDistance = 0f)
    {
        ValidateFinite(destination);
        if (!float.IsFinite(maxDistance) || maxDistance < 0f || !float.IsFinite(stopDistance) || stopDistance < 0f)
            throw new ArgumentOutOfRangeException(nameof(maxDistance), "Distances must be finite and nonnegative.");
        Vector3 current = WorldPosition;
        double x = (double)destination.X - current.X;
        double y = (double)destination.Y - current.Y;
        double z = (double)destination.Z - current.Z;
        double distance = Math.Sqrt(x * x + y * y + z * z);
        if (distance <= stopDistance || maxDistance == 0f) return;
        double scale = Math.Min(maxDistance, distance - stopDistance) / distance;
        WorldPosition = new Vector3((float)(current.X + x * scale), (float)(current.Y + y * scale), (float)(current.Z + z * scale));
    }
    static void ValidateFinite(Vector3 value)
    {
        if (!value.IsFinite) throw new ArgumentOutOfRangeException(nameof(value), "Transform values must be finite.");
    }
}

public sealed class ScriptTime
{
    internal ScriptTime(ScriptBinding owner) { m_owner = owner; }
    readonly ScriptBinding m_owner;
    /// @note Update / LateUpdate はフレーム差、FixedUpdate と物理通知は固定時間差。それ以外の callback は拒否する。
    [ApiContract(Units = "s", Coordinate = "not_applicable", Failure = "exception_outside_update_or_physics_notification; frame_delta_for_Update_LateUpdate; fixed_delta_for_FixedUpdate_and_collision_trigger", Read = "time")]
    public float DeltaSeconds
   {
        get
        {
            ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
            if (scope.Callback is not (ScriptCallback.UPDATE or ScriptCallback.FIXED_UPDATE or ScriptCallback.LATE_UPDATE
                or ScriptCallback.ON_COLLISION_ENTER or ScriptCallback.ON_COLLISION_STAY or ScriptCallback.ON_COLLISION_EXIT
                or ScriptCallback.ON_TRIGGER_ENTER or ScriptCallback.ON_TRIGGER_STAY or ScriptCallback.ON_TRIGGER_EXIT))
                throw new InvalidOperationException("DeltaSeconds requires an update or physics callback.");
            return scope.Bridge.DeltaSeconds(scope);
        }
    }
}

public sealed class ScriptInput
{
    internal ScriptInput(ScriptBinding owner) { m_owner = owner; }
    readonly ScriptBinding m_owner;
    public bool GetKey(KeyCode key) => Read(key, ScriptInputValue.HELD);
    public bool GetKeyDown(KeyCode key) => Read(key, ScriptInputValue.DOWN);
    public bool GetKeyUp(KeyCode key) => Read(key, ScriptInputValue.UP);
    /// @return 現在の設定に登録されたボタンの非所有ハンドル。未登録名は名前付き例外。
    /// @note callback 内で取得・使用する。cache する場合は Connect で取得し、使用時に最新の設定を再検証する。
    [ApiContract(Summary = "Resolve a button name in the current input settings.", Call = "input", Purity = ApiPurity.ENGINE,
        Failure = "unknown_button_name_throws_with_discovery_hint; invalid_scope_throws", Lifetime = "non_owning_action_name; current_callback_revalidates_mapping",
        ParameterHelp = new[] { "name=Case-sensitive button name in the current InputActionMap." })]
    public ButtonAction Button(string name)
    {
        if (!HasButton(name)) throw InputActionAccess.Unknown(name, false);
        return new ButtonAction(name);
    }
    /// @return 現在の設定に登録された軸の非所有ハンドル。未登録名は名前付き例外。
    /// @note callback 内で取得・使用する。cache する場合は Connect で取得し、使用時に最新の設定を再検証する。
    [ApiContract(Summary = "Resolve an axis name in the current input settings.", Call = "input", Purity = ApiPurity.ENGINE,
        Failure = "unknown_axis_name_throws_with_discovery_hint; invalid_scope_throws", Lifetime = "non_owning_action_name; current_callback_revalidates_mapping",
        ParameterHelp = new[] { "name=Case-sensitive axis name in the current InputActionMap." })]
    public AxisAction Axis(string name)
    {
        if (!HasAxis(name)) throw InputActionAccess.Unknown(name, true);
        return new AxisAction(name);
    }
    /// @note 押下状態に依存せず、現在の InputActionMap に同名ボタンがあれば true。未登録名は false。
    [ApiContract(Summary = "Check whether the current input settings define a button.", Call = "input", Purity = ApiPurity.ENGINE,
        Failure = "unknown_button_name_returns_false; empty_name_or_invalid_scope_throws")]
    public bool HasButton(string name) => HasAction(name, ScriptInputQuery.BUTTON_EXISTS);
    /// @note 入力値に依存せず、現在の InputActionMap に同名軸があれば true。未登録名は false。
    [ApiContract(Summary = "Check whether the current input settings define an axis.", Call = "input", Purity = ApiPurity.ENGINE,
        Failure = "unknown_axis_name_returns_false; empty_name_or_invalid_scope_throws")]
    public bool HasAxis(string name) => HasAction(name, ScriptInputQuery.AXIS_EXISTS);
    /// @return 実行中の設定に登録されたボタン名の snapshot。登録順で返し、後の設定変更には追従しない。
    [ApiContract(Summary = "Read the current button names in registration order.", Call = "input", Purity = ApiPurity.ENGINE,
        Failure = "invalid_scope_or_malformed_snapshot_throws", Lifetime = "managed_snapshot_of_current_mapping")]
    public string[] GetButtonNames() => ReadNames(ScriptInputQuery.BUTTON_NAMES);
    /// @return 実行中の設定に登録された軸名の snapshot。登録順で返し、後の設定変更には追従しない。
    [ApiContract(Summary = "Read the current axis names in registration order.", Call = "input", Purity = ApiPurity.ENGINE,
        Failure = "invalid_scope_or_malformed_snapshot_throws", Lifetime = "managed_snapshot_of_current_mapping")]
    public string[] GetAxisNames() => ReadNames(ScriptInputQuery.AXIS_NAMES);
    /// @return 登録順の全件を返す。不足時は Required と Written=0、output は未変更。未使用の末尾は保持する。
    [ApiContract(Summary = "Copy all button names in registration order into caller storage using bounded transport packets.", Call = "input", Purity = ApiPurity.ENGINE,
        Failure = "insufficient_capacity_returns_required_and_written_zero_without_output_changes", Lifetime = "managed_snapshot_of_current_mapping")]
    public CollectionReadResult GetButtonNames(Span<string?> output) => ReadNames(ScriptInputQuery.BUTTON_NAMES, output);
    /// @return 登録順の全件を返す。不足時は Required と Written=0、output は未変更。未使用の末尾は保持する。
    [ApiContract(Summary = "Copy all axis names in registration order into caller storage using bounded transport packets.", Call = "input", Purity = ApiPurity.ENGINE,
        Failure = "insufficient_capacity_returns_required_and_written_zero_without_output_changes", Lifetime = "managed_snapshot_of_current_mapping")]
    public CollectionReadResult GetAxisNames(Span<string?> output) => ReadNames(ScriptInputQuery.AXIS_NAMES, output);
    bool HasAction(string name, ScriptInputQuery query)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.HasInputAction(scope, name, (uint)query);
    }
    string[] ReadNames(ScriptInputQuery query)
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.ReadInputNames(scope, (uint)query);
    }
    CollectionReadResult ReadNames(ScriptInputQuery query, Span<string?> output)
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.ReadInputNames(scope, (uint)query, output);
    }
    bool Read(KeyCode key, ScriptInputValue state)
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.ReadKey(scope, key, (uint)state);
    }
}

internal readonly record struct CapturedPlacementPose(Vector3 Position, Quaternion Rotation);

/// @note No user callback or mutation is invoked between the synchronous pose reads and creation.
/// @see Docs/design/script-transform-contract.md Accumulated signed scale and synchronized world pose.
internal static class PlacementPoseCapture
{
    const int MAX_ANCESTORS = 4096;
    internal static CapturedPlacementPose Capture(ScriptExecutionScope scope, GameObject anchor,
        Vector3 localOffset, Quaternion rotationOffset)
    {
        ArgumentNullException.ThrowIfNull(anchor);
        if (!localOffset.IsFinite || !rotationOffset.IsFinite
            || !float.IsFinite(rotationOffset.LengthSquared) || MathF.Abs(rotationOffset.LengthSquared - 1f) > 0.002f)
            throw new ArgumentOutOfRangeException(nameof(localOffset), "Offsets must be finite and rotationOffset must be a unit quaternion.");
        RequireObject(scope, anchor);
        Vector3 position = scope.Bridge.ReadPosition(scope, anchor.ObjectId, true);
        Quaternion rotation = scope.Bridge.ReadRotation(scope, anchor.ObjectId, true);
        if (!position.IsFinite || !rotation.IsFinite || !float.IsFinite(rotation.Length) || rotation.Length < Mathf.EPSILON)
            throw new InvalidOperationException("The anchor has an invalid world pose.");
        rotation = rotation.Normalized();
        if (localOffset.X != 0f || localOffset.Y != 0f || localOffset.Z != 0f)
        {
            List<Vector3> localScales = new();
            HashSet<ulong> visited = new();
            for (GameObject? current = anchor; current is not null; current = scope.Bridge.ReadParent(scope, current.ObjectId))
            {
                if (localScales.Count >= MAX_ANCESTORS || !visited.Add(current.ObjectId))
                    throw new InvalidOperationException("The anchor hierarchy is cyclic or too deep.");
                RequireObject(scope, current);
                Vector3 scale = scope.Bridge.ReadLocalScale(scope, current.ObjectId);
                if (!scale.IsFinite) throw new InvalidOperationException("The anchor hierarchy has invalid scale.");
                localScales.Add(scale);
            }
            Vector3 worldScale = Vector3.ONE;
            for (int index = localScales.Count - 1; index >= 0; --index)
            {
                Vector3 local = localScales[index];
                worldScale = new(worldScale.X * local.X, worldScale.Y * local.Y, worldScale.Z * local.Z);
            }
            position += rotation * new Vector3(localOffset.X * worldScale.X, localOffset.Y * worldScale.Y, localOffset.Z * worldScale.Z);
        }
        Quaternion finalRotation = (rotation * rotationOffset).Normalized();
        if (!position.IsFinite || !finalRotation.IsFinite)
            throw new InvalidOperationException("The captured placement pose is not finite.");
        return new(position, finalRotation);
    }
    static void RequireObject(ScriptExecutionScope scope, GameObject value)
    {
        if (value.SceneLifetime != scope.SceneLifetime || value.ObjectId == 0 || !scope.Bridge.IsAlive(scope, value.ObjectId))
            throw new InvalidOperationException("The anchor is unresolved, expired, or belongs to another scene.");
    }
}

internal sealed class ScriptBinding
{
    internal ScriptBinding(ulong generation, ulong instanceId, ulong ownerId, string savedScriptId = "", string typeKey = "")
    {
        Generation = generation; InstanceId = instanceId; OwnerId = ownerId; SavedScriptId = savedScriptId; TypeKey = typeKey;
        GameObject = new GameObject(0, 0, ""); Time = new ScriptTime(this); Input = new ScriptInput(this); Scene = new ScriptScene(this); Objects = new ScriptObjects(this); Physics = new ScriptPhysics(this);
        Jobs = new ScriptJobs(this); Events = new ScriptEvents(this); DataAssets = new ScriptDataAssets(this);
        App = new ScriptApplication(this); Cursor = new ScriptCursor(this);
        Save = new ScriptSave(this); Config = new ScriptConfig(this);
        Render = new ScriptRender(this); Display = new ScriptDisplay(this);
        Assets = new ScriptAssets(this);
    }
    internal ulong Generation { get; }
    internal ulong InstanceId { get; }
    internal ulong OwnerId { get; }
    internal string SavedScriptId { get; }
    internal string TypeKey { get; }
    internal string ScriptBirth { get; set; } = "";
    internal bool Active { get; set; } = true;
    internal GameObject GameObject { get; set; }
    internal ScriptTime Time { get; }
    internal ScriptInput Input { get; }
    internal ScriptScene Scene { get; }
    internal ScriptRender Render { get; }
    internal ScriptDisplay Display { get; }
    internal ScriptAssets Assets { get; }
    internal ScriptObjects Objects { get; }
    internal ScriptPhysics Physics { get; }
    internal ScriptApplication App { get; }
    internal ScriptCursor Cursor { get; }
    internal ScriptSave Save { get; }
    internal ScriptConfig Config { get; }
    internal ScriptJobs Jobs { get; }
    internal ScriptEvents Events { get; }
    internal ScriptDataAssets DataAssets { get; }
}

internal interface IScriptNativeBridge
{
    long CreateRuntimeTexture(ScriptExecutionScope scope, int width, int height, RuntimeTextureFormat format, out ulong ownerObject)
    { ownerObject = 0; throw new NotSupportedException(); }
    bool UpdateRuntimeTexture(ScriptExecutionScope scope, ulong ownerObject, long texture, int x, int y, int width, int height,
        int rowPitch, ReadOnlySpan<byte> pixels) => throw new NotSupportedException();
    ComponentReferenceIdentity ResolveComponentReference(ScriptExecutionScope scope, ComponentReferenceIdentity identity) => throw new NotSupportedException();
    bool HasScriptEntry(ScriptExecutionScope scope, ulong objectId, string savedScriptId) => throw new NotSupportedException();
    bool HasInputAction(ScriptExecutionScope scope, string action, uint query) => throw new NotSupportedException();
    string[] ReadInputNames(ScriptExecutionScope scope, uint query) => throw new NotSupportedException();
    CollectionReadResult ReadInputNames(ScriptExecutionScope scope, uint query, Span<string?> output)
    {
        string[] names = ReadInputNames(scope, query);
        if (output.Length < names.Length) return new(names.Length, 0);
        for (int index = 0; index < names.Length; ++index) output[index] = names[index];
        return new(names.Length, names.Length);
    }
    bool ReadComponent(ScriptExecutionScope scope, ulong objectId, uint kind) => throw new NotSupportedException();
    ulong ReadComponentLifetime(ScriptExecutionScope scope, ulong objectId, uint kind) => throw new NotSupportedException();
    long ReadObject(ScriptExecutionScope scope, ulong objectId, uint selector) => throw new NotSupportedException();
    string ReadObjectText(ScriptExecutionScope scope, ulong objectId, uint selector) => throw new NotSupportedException();
    void WriteObject(ScriptExecutionScope scope, ulong objectId, uint selector, long value, string text) => throw new NotSupportedException();
    bool ReadScriptEnabled(ScriptExecutionScope scope, ulong instanceId) => throw new NotSupportedException();
    void WriteScriptEnabled(ScriptExecutionScope scope, ulong instanceId, bool value) => throw new NotSupportedException();
    ulong AddComponent(ScriptExecutionScope scope, ulong objectId, uint kind, string typeName) => throw new NotSupportedException();
    bool RemoveComponent(ScriptExecutionScope scope, ulong objectId, uint kind, ulong instanceId) => throw new NotSupportedException();
    GameObject[] QueryObjects(ScriptExecutionScope scope, ulong root, uint scopeKind, string name, string tag, string prefabPart, bool includeInactive) => throw new NotSupportedException();
    ObjectQueryResult QueryObjectsInto(ScriptExecutionScope scope, ulong root, uint scopeKind, string name, string tag, string prefabPart, bool includeInactive, Span<GameObject?> output) => throw new NotSupportedException();
    void OwnObjects(ScriptExecutionScope scope, ulong instanceId, ReadOnlySpan<GameObject> objects) => throw new NotSupportedException();
    GameObject? ReadParent(ScriptExecutionScope scope, ulong objectId) => throw new NotSupportedException();
    bool SetParent(ScriptExecutionScope scope, ulong objectId, GameObject? parent) => throw new NotSupportedException();
    BatchResult SpawnMany(ScriptExecutionScope scope, string reference, bool prefab, ReadOnlySpan<SpawnPose> poses, Span<GameObject?> output) => throw new NotSupportedException();
    NativeServiceResult InvokeService(ScriptExecutionScope scope, ulong objectId, uint service, uint method, long integer,
        ReadOnlySpan<float> values, ReadOnlySpan<string> text, GameObject? related, bool readOnly) => throw new NotSupportedException();
    void WriteStoreString(ScriptExecutionScope scope, ulong objectId, uint service, string key, string value)
        => InvokeService(scope, objectId, service, 15, 0, [], [key, value], null, false);
    void WriteStoreDocument(ScriptExecutionScope scope, ulong objectId, uint service, string key, Type type, object? value) => throw new NotSupportedException();
    bool ReadStoreDocument(ScriptExecutionScope scope, ulong objectId, uint service, string key, Type type, object? defaults, out object? value)
    { value = null; throw new NotSupportedException(); }
    bool ReadMaterialValues(ScriptExecutionScope scope, ulong objectId, int slot, string property, Span<double> output, out CollectionReadResult capacity)
    { capacity = default; throw new NotSupportedException(); }
    double[]? ReadMaterialValues(ScriptExecutionScope scope, ulong objectId, int slot, string property) => throw new NotSupportedException();
    bool WriteMaterialValues(ScriptExecutionScope scope, ulong objectId, int slot, string property, ReadOnlySpan<double> values) => throw new NotSupportedException();
    CollectionReadResult ReadBlendWeights(ScriptExecutionScope scope, ulong objectId, Span<BlendWeight> output) => throw new NotSupportedException();
    Quaternion ReadPhysicsRotation(ScriptExecutionScope scope, ulong objectId) => throw new NotSupportedException();
    void WritePhysicsRotation(ScriptExecutionScope scope, ulong objectId, Quaternion value) => throw new NotSupportedException();
    bool RaycastFiltered(ScriptExecutionScope scope, Vector3 origin, Vector3 direction, float maxDistance, PhysicsQueryOptions options, out RaycastHit hit) => throw new NotSupportedException();
    bool SphereCastFiltered(ScriptExecutionScope scope, Vector3 center, float radius, Vector3 direction, float maxDistance, PhysicsQueryOptions options, out RaycastHit hit) => throw new NotSupportedException();
    RaycastHit[] RaycastAllFiltered(ScriptExecutionScope scope, Vector3 origin, Vector3 direction, float maxDistance, PhysicsQueryOptions options) => throw new NotSupportedException();
    GameObject[] OverlapSphereFiltered(ScriptExecutionScope scope, Vector3 center, float radius, PhysicsQueryOptions options) => throw new NotSupportedException();
    bool IsAlive(ScriptExecutionScope scope, ulong objectId);
    Vector3 ReadPosition(ScriptExecutionScope scope, ulong objectId, bool world);
    void WritePosition(ScriptExecutionScope scope, ulong objectId, Vector3 value, bool world);
    void TranslateLocal(ScriptExecutionScope scope, ulong objectId, Vector3 delta);
    Quaternion ReadRotation(ScriptExecutionScope scope, ulong objectId, bool world);
    void WriteLocalRotation(ScriptExecutionScope scope, ulong objectId, Quaternion value);
    Vector3 ReadLocalScale(ScriptExecutionScope scope, ulong objectId);
    void WriteLocalScale(ScriptExecutionScope scope, ulong objectId, Vector3 value);
    Vector3 ReadDirection(ScriptExecutionScope scope, ulong objectId, uint direction);
    void Rotate(ScriptExecutionScope scope, ulong objectId, Vector3 axis, float degrees);
    void LookAt(ScriptExecutionScope scope, ulong objectId, Vector3 worldTarget);
    bool ReadKey(ScriptExecutionScope scope, KeyCode key, uint state);
    float ReadAction(ScriptExecutionScope scope, string action, uint state);
    float DeltaSeconds(ScriptExecutionScope scope);
    ulong ResolveGameObject(ulong generation, ulong instanceId, ulong ownerId, string guid);
    ulong[] GetManagedComponents(ScriptExecutionScope scope, ulong objectId);
    /// @return 必要な ID 数。容量不足時は destination を変更しない。
    int GetManagedComponents(ScriptExecutionScope scope, ulong objectId, Span<ulong> destination)
    {
        ulong[] values = GetManagedComponents(scope, objectId);
        if (values.Length <= destination.Length) values.AsSpan().CopyTo(destination);
        return values.Length;
    }
    ManagedComponentPage ReadManagedComponents(ScriptExecutionScope scope, ulong objectId, uint offset, Span<ulong> destination)
    {
        if (destination.IsEmpty) throw new ArgumentException("A component page requires output capacity.", nameof(destination));
        ulong[] values = GetManagedComponents(scope, objectId);
        if (offset > values.Length) throw new ArgumentOutOfRangeException(nameof(offset));
        int count = Math.Min(destination.Length, values.Length - (int)offset);
        values.AsSpan((int)offset, count).CopyTo(destination);
        uint next = checked(offset + (uint)count);
        return new(count, next, next == values.Length);
    }
    string GetObjectName(ScriptExecutionScope scope, ulong objectId);
    GameObject? FindByName(ScriptExecutionScope scope, string name, bool includeInactive);
    GameObject? FindChild(ScriptExecutionScope scope, ulong objectId, ReadOnlySpan<string> names, bool includeInactive);
    BatchResult TranslateWorld(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects, Vector3 displacement, bool activeOnly);
    BatchResult ReadWorldPositions(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects, Span<Vector3> positions, Span<ObjectStatus> statuses);
    BatchResult SetWorldPositions(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects, ReadOnlySpan<Vector3> positions, Span<ObjectStatus> statuses);
    GameObject? CreateObject(ScriptExecutionScope scope, string name, Vector3 position, Quaternion rotation, bool usePose);
    GameObject? InstantiateObject(ScriptExecutionScope scope, string prefab, Vector3 position, Quaternion rotation, bool usePose);
    bool CanCreate(ScriptExecutionScope scope, int count);
    bool DestroyObject(ScriptExecutionScope scope, ulong objectId, float delay);
    BatchResult DestroyObjects(ScriptExecutionScope scope, ReadOnlySpan<GameObject?> objects, float delay);
    Vector3 ReadPhysicsVector(ScriptExecutionScope scope, ulong objectId, uint selector);
    float ReadPhysicsScalar(ScriptExecutionScope scope, ulong objectId, uint selector);
    bool ReadPhysicsFlag(ScriptExecutionScope scope, ulong objectId, uint selector);
    void WritePhysicsVector(ScriptExecutionScope scope, ulong objectId, uint selector, Vector3 value);
    void WritePhysicsScalar(ScriptExecutionScope scope, ulong objectId, uint selector, float value);
    void WritePhysicsFlag(ScriptExecutionScope scope, ulong objectId, uint selector, bool value);
    void PhysicsBodyCommand(ScriptExecutionScope scope, ulong objectId, uint command, Vector3 value, Vector3 point, uint axes);
    Vector3 ReadWorldGravity(ScriptExecutionScope scope);
    bool LayersCollide(ScriptExecutionScope scope, int a, int b);
    bool Raycast(ScriptExecutionScope scope, Vector3 origin, Vector3 direction, float maxDistance, out RaycastHit hit);
    bool SphereCast(ScriptExecutionScope scope, Vector3 center, float radius, Vector3 direction, float maxDistance, out RaycastHit hit);
    RaycastHit[] RaycastAll(ScriptExecutionScope scope, Vector3 origin, Vector3 direction, float maxDistance);
    CollectionReadResult ReadRaycastAll(ScriptExecutionScope scope, Vector3 origin, Vector3 direction, float maxDistance,
        Span<RaycastHit> output, PhysicsQueryOptions? options) => throw new NotSupportedException();
    GameObject[] OverlapSphere(ScriptExecutionScope scope, Vector3 center, float radius);
    CollectionReadResult ReadOverlapSphere(ScriptExecutionScope scope, Vector3 center, float radius,
        Span<GameObject> output, PhysicsQueryOptions? options) => throw new NotSupportedException();
    CollectionReadResult ReadDisplayResolutions(ScriptExecutionScope scope, ulong objectId, Span<DisplayResolution> output) => throw new NotSupportedException();
    bool ConnectJointChain(ScriptExecutionScope scope, ulong objectId, ReadOnlySpan<GameObject> targets, float distance, int iterations) => throw new NotSupportedException();
}

internal interface IScriptComponentLookup
{
    Component[] GetComponents(ScriptExecutionScope scope, GameObject target, Type type);
    Component? GetComponent(ScriptExecutionScope scope, GameObject target, Type type)
    {
        Component[] values = GetComponents(scope, target, type);
        return values.Length == 0 ? null : values[0];
    }
    CollectionReadResult GetComponents<T>(ScriptExecutionScope scope, GameObject target, Span<T?> output) where T : Component
    {
        Component[] values = GetComponents(scope, target, typeof(T));
        if (output.Length < values.Length) return new(values.Length, 0);
        for (int index = 0; index < values.Length; index++) output[index] = (T)values[index];
        return new(values.Length, values.Length);
    }
}

/// @note An immutable value carries one non-reusable callback identity; retaining a copy never grants authority to a later callback.
internal readonly struct ScriptExecutionScope
{
    internal ScriptExecutionScope(IScriptNativeBridge bridge, IScriptComponentLookup components, ulong generation, ulong instanceId, ulong ownerId, ulong callId, ulong sceneLifetime, ScriptCallback callback)
    { Bridge = bridge; Components = components; Generation = generation; InstanceId = instanceId; OwnerId = ownerId; CallId = callId; SceneLifetime = sceneLifetime; Callback = callback; ThreadId = Environment.CurrentManagedThreadId; Identity = ScriptApiRuntime.NextScopeIdentity(); }
    internal ulong Identity { get; }
    internal IScriptNativeBridge Bridge { get; }
    internal IScriptComponentLookup Components { get; }
    internal ulong Generation { get; }
    internal ulong InstanceId { get; }
    internal ulong OwnerId { get; }
    internal ulong CallId { get; }
    internal ulong SceneLifetime { get; }
    internal ScriptCallback Callback { get; }
    internal int ThreadId { get; }
    internal GizmoDrawContext? Gizmos { get; init; }
}

internal static class ScriptApiRuntime
{
    [ThreadStatic] internal static ScriptExecutionScope? Current;
    [ThreadStatic] static ulong s_lastScopeIdentity;
    /// @note The per-thread identity is never reset, reused or silently wrapped, including after generation reload.
    internal static ulong NextScopeIdentity()
    {
        if (s_lastScopeIdentity == ulong.MaxValue) throw new InvalidOperationException("Host callback scope identities are exhausted.");
        return ++s_lastScopeIdentity;
    }
    /// @note Compare the complete immutable callback witness; equal user/native IDs alone cannot make a newly constructed scope current.
    internal static bool IsSameScope(ScriptExecutionScope? first, ScriptExecutionScope? second)
    {
        if (first is not ScriptExecutionScope left) return second is null;
        return second is ScriptExecutionScope right && left.Identity != 0 && left.Identity == right.Identity
            && left.ThreadId == right.ThreadId && left.Generation == right.Generation && left.InstanceId == right.InstanceId
            && left.OwnerId == right.OwnerId && left.CallId == right.CallId && left.SceneLifetime == right.SceneLifetime
            && left.Callback == right.Callback && ReferenceEquals(left.Bridge, right.Bridge)
            && ReferenceEquals(left.Components, right.Components) && ReferenceEquals(left.Gizmos, right.Gizmos);
    }
    internal static ScriptExecutionScope RequireScope(ScriptBinding? receiver = null)
    {
        ScriptExecutionScope scope = Current ?? throw new InvalidOperationException("Engine API requires a live Host callback.");
        if (scope.Identity == 0 || scope.ThreadId != Environment.CurrentManagedThreadId || scope.CallId == 0)
            throw new InvalidOperationException("Engine API requires the current callback thread.");
        if (ScriptLifecycleContract.GetExecutionPolicy(scope.Callback) == "no_engine_api")
            throw new InvalidOperationException($"{scope.Callback} does not permit Engine API access; update managed data only.");
        if (receiver is not null && (!receiver.Active || receiver.Generation != scope.Generation || receiver.GameObject.SceneLifetime != scope.SceneLifetime))
            throw new InvalidOperationException("The receiver belongs to a retired generation or another scene.");
        return scope;
    }
}
