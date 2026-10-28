/// @file    ObjectControls.cs
/// @brief   オブジェクトの有効状態・階層・Component と一括生成を制御する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;

namespace Fbzz;

public abstract partial class Script
{
    /// @note 自動 callback の配送を制御する。通知は既存 ScriptSystem が同期する。
    public bool Enabled
    {
        get { ScriptBinding binding = RequireBinding(); return ScriptApiRuntime.RequireScope(binding).Bridge.ReadScriptEnabled(ScriptApiRuntime.RequireScope(binding), binding.InstanceId); }
        set { ScriptBinding binding = RequireBinding(); ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(binding); scope.Bridge.WriteScriptEnabled(scope, binding.InstanceId, value); }
    }
}

public sealed partial class GameObject : IEquatable<GameObject>
{
    /// @note 同じ Scene 内の同じ寿命 ID を比較する。破棄済み参照を C# null に変換しない。
    public bool Equals(GameObject? other) => other is not null && SceneLifetime == other.SceneLifetime
        && ObjectId == other.ObjectId && (ObjectId != 0 || string.Equals(Guid, other.Guid, StringComparison.Ordinal));
    public override bool Equals(object? other) => other is GameObject gameObject && Equals(gameObject);
    public override int GetHashCode() => ObjectId != 0 ? HashCode.Combine(SceneLifetime, ObjectId) : HashCode.Combine(SceneLifetime, Guid);
    public static bool operator ==(GameObject? left, GameObject? right) => ReferenceEquals(left, right) || left is not null && left.Equals(right);
    public static bool operator !=(GameObject? left, GameObject? right) => !(left == right);
    public bool ActiveSelf => ReadValue(0) != 0;
    public bool ActiveInHierarchy => ReadValue(1) != 0;
    public int Layer
    {
        get => checked((int)ReadValue(2));
        set { if ((uint)value >= 32) throw new ArgumentOutOfRangeException(nameof(value)); WriteValue(2, value, ""); }
    }
    public string Tag
    {
        get { ScriptExecutionScope scope = Scope(); return scope.Bridge.ReadObjectText(scope, ObjectId, 4); }
        set { ArgumentException.ThrowIfNullOrEmpty(value); WriteValue(4, 0, value); }
    }
    public GameObject? Parent { get { ScriptExecutionScope scope = Scope(); return scope.Bridge.ReadParent(scope, ObjectId); } }
    public GameObject[] Children => GetChildren();
    public void SetActive(bool active) => WriteValue(0, active ? 1 : 0, "");
    public void Rename(string name) { ArgumentException.ThrowIfNullOrEmpty(name); WriteValue(3, 0, name); }
    /// @note local Transform を維持する。別 Scene・循環・失効した親を拒否する。
    public bool SetParent(GameObject? parent)
    {
        ScriptExecutionScope scope = Scope();
        if (parent is not null && (parent.SceneLifetime != scope.SceneLifetime || parent.ObjectId == 0))
            throw new InvalidOperationException("The parent is unresolved or belongs to another scene.");
        return scope.Bridge.SetParent(scope, ObjectId, parent);
    }
    [ApiContract(Call = "read")]
    public bool CompareTag(string tag) => Tag == tag;
    [ApiContract(Call = "read")]
    public GameObject[] GetChildren(bool includeInactive = true) => Query(1, "", "", "", includeInactive);
    [ApiContract(Call = "read")]
    public GameObject[] GetDescendants(bool includeInactive = true) => Query(2, "", "", "", includeInactive);
    /// @note prefabSourceId を指定し、この root 配下の実体だけを取得する。
    [ApiContract(Call = "read")]
    public GameObject[] FindPrefabPart(string sourceId, bool includeInactive = true)
    {
        ArgumentException.ThrowIfNullOrEmpty(sourceId);
        return Query(3, "", "", sourceId, includeInactive);
    }
    [ApiContract(Call = "read")]
    public GameObject[] FindDescendants(string name = "", string tag = "", bool includeInactive = true)
        => Query(2, name, tag, "", includeInactive);
    /// @note 容量不足時は output を変更せず必要件数を返す。容量を確保した後は内部転送バッファを再利用する。
    [ApiContract(Call = "read")]
    public ObjectQueryResult ReadDescendants(Span<GameObject?> output, string name = "", string tag = "", bool includeInactive = true)
    {
        ScriptExecutionScope scope = Scope();
        return scope.Bridge.QueryObjectsInto(scope, ObjectId, 2, name, tag, "", includeInactive, output);
    }
    /// @note native は一型一個。除去予約中の同型 Add は Cleanup が完了するまで拒否する。managed Awake/Start は次の Script snapshot から呼ぶ。
    public T AddComponent<T>() where T : Component
    {
        ScriptExecutionScope scope = Scope();
        uint kind = NativeComponentFactory.GetKind(typeof(T));
        if (kind == 0) return (T)(Component)Transform;
        if (kind == uint.MaxValue && (!typeof(Script).IsAssignableFrom(typeof(T)) || typeof(T).IsAbstract))
            throw new NotSupportedException("Only concrete Script types and registered native components can be added.");
        ulong id = scope.Bridge.AddComponent(scope, ObjectId, kind, kind == uint.MaxValue ? typeof(T).FullName! : "");
        T[] components = GetComponents<T>();
        if (kind != uint.MaxValue && components.Length != 0) return components[0];
        foreach (T component in components)
            if (component.Binding?.InstanceId == id) return component;
        throw new InvalidOperationException("The component was not published.");
    }
    /// @note Script は最初の型一致一個を Cleanup で解放する。受付後は自動 callback を停止する。
    public bool RemoveComponent<T>() where T : Component
    {
        ScriptExecutionScope scope = Scope();
        uint kind = NativeComponentFactory.GetKind(typeof(T));
        if (kind == 0) throw new NotSupportedException("Transform cannot be removed.");
        T? component = GetComponent<T>();
        return component is not null && scope.Bridge.RemoveComponent(scope, ObjectId, kind, component.Binding?.InstanceId ?? 0);
    }
    ScriptExecutionScope Scope()
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        if (SceneLifetime != scope.SceneLifetime) throw new InvalidOperationException("The object belongs to another scene.");
        return scope;
    }
    long ReadValue(uint selector) { ScriptExecutionScope scope = Scope(); return scope.Bridge.ReadObject(scope, ObjectId, selector); }
    void WriteValue(uint selector, long value, string text) { ScriptExecutionScope scope = Scope(); scope.Bridge.WriteObject(scope, ObjectId, selector, value, text); }
    GameObject[] Query(uint kind, string name, string tag, string part, bool inactive)
    {
        ScriptExecutionScope scope = Scope();
        return scope.Bridge.QueryObjects(scope, ObjectId, kind, name, tag, part, inactive);
    }
}

/// @note position はワールド [m]、rotation は有限・非退化の Quaternion。
public readonly struct SpawnPose
{
    public SpawnPose(Vector3 position, Quaternion rotation) { Position = position; Rotation = rotation; }
    public Vector3 Position { get; }
    public Quaternion Rotation { get; }
}

public readonly struct ObjectQueryResult
{
    internal ObjectQueryResult(BatchStatus status, int count) { Status = status; RequiredCapacity = count; Count = status == BatchStatus.OK ? count : 0; }
    public BatchStatus Status { get; }
    public int Count { get; }
    public int RequiredCapacity { get; }
}

public sealed partial class ScriptScene
{
    /// @note Scene 全体の Hierarchy 順。空の name/tag は条件を指定しない。
    [ApiContract(Call = "read")]
    public GameObject[] FindAll(string name = "", string tag = "", bool includeInactive = false)
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.QueryObjects(scope, 0, 0, name, tag, "", includeInactive);
    }
    /// @note 直近の成功した検索結果の wrapper を再利用する。別の絞込検索を挟む場合は Equals で寿命 ID を比較する。
    [ApiContract(Call = "read")]
    public ObjectQueryResult ReadObjects(Span<GameObject?> output, string name = "", string tag = "", bool includeInactive = false)
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.QueryObjectsInto(scope, 0, 0, name, tag, "", includeInactive, output);
    }
    /// @note 全 pose と容量を先に検証する。失敗時は Scene と output を未変更にする。
    public BatchResult CreateMany(string name, ReadOnlySpan<SpawnPose> poses, Span<GameObject?> output)
        => Spawn(name, false, poses, output);
    public BatchResult InstantiateMany(string prefab, ReadOnlySpan<SpawnPose> poses, Span<GameObject?> output)
        => Spawn(prefab, true, poses, output);
    BatchResult Spawn(string reference, bool prefab, ReadOnlySpan<SpawnPose> poses, Span<GameObject?> output)
    {
        ArgumentException.ThrowIfNullOrEmpty(reference);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        if (output.Length < poses.Length) return new BatchResult(BatchStatus.BUFFER_TOO_SMALL, poses.Length, 0, 0);
        foreach (SpawnPose pose in poses)
            if (!pose.Position.IsFinite || !pose.Rotation.IsFinite || !float.IsFinite(pose.Rotation.Length) || pose.Rotation.Length < Mathf.EPSILON)
                return new BatchResult(BatchStatus.INVALID_ARGUMENT, poses.Length, 0, 0);
        return scope.Bridge.SpawnMany(scope, reference, prefab, poses, output);
    }
    /// @note pool の全個体は作成元 Script の GO 配下に保持する。所有者の破棄で階層ごと解放される。
    public ObjectPool CreatePool(string prefab, int capacity) => new(m_owner, prefab, capacity);
}

/// @note Rent/Return は Play の同期 callback 内。返却後の参照は保持できるが、貸出権は失う。
public sealed class ObjectPool : IDisposable
{
    readonly ScriptBinding m_owner;
    readonly GameObject[] m_items;
    readonly bool[] m_rented;
    bool m_disposed;
    internal ObjectPool(ScriptBinding owner, string prefab, int capacity)
    {
        if (capacity < 0) throw new ArgumentOutOfRangeException(nameof(capacity));
        m_owner = owner;
        SpawnPose[] poses = new SpawnPose[capacity];
        for (int index = 0; index < capacity; index++) poses[index] = new SpawnPose(Vector3.ZERO, Quaternion.IDENTITY);
        GameObject?[] output = new GameObject?[capacity];
        BatchResult result = owner.Scene.InstantiateMany(prefab, poses, output);
        if (result.Status != BatchStatus.OK) throw new InvalidOperationException("Pool creation failed: " + result.Status);
        m_items = new GameObject[capacity]; m_rented = new bool[capacity];
        try
        {
            for (int index = 0; index < capacity; index++)
            {
                m_items[index] = output[index]!;
                if (!m_items[index].SetParent(owner.GameObject)) throw new InvalidOperationException("Pool parent binding failed.");
                m_items[index].SetActive(false);
            }
            ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(owner);
            scope.Bridge.OwnObjects(scope, owner.InstanceId, m_items);
        }
        catch { owner.Scene.DestroyMany(output); throw; }
    }
    [ApiContract(Read = "any", Purity = ApiPurity.MANAGED, Units = "count", Lifetime = "managed_pool_state", Failure = "stored_capacity_read")]
    public int Capacity => m_items.Length;
    [ApiContract(Call = "live", RunModes = new[] { "Edit", "Play" }, Purity = ApiPurity.ENGINE, Lifetime = "owning_script_generation_and_scene", Summary = "Activates and rents the first live available pool object.", Returns = "Null when no live un-rented object remains.", Failure = "null_when_exhausted; disposed_or_expired_owner_or_invalid_scope_throws")]
    public GameObject? Rent()
    {
        RequireOwner();
        for (int index = 0; index < m_items.Length; index++)
        {
            if (m_rented[index] || !m_items[index].IsAlive) continue;
            m_items[index].SetActive(true); m_rented[index] = true;
            return m_items[index];
        }
        return null;
    }
    [ApiContract(Call = "live", RunModes = new[] { "Edit", "Play" }, Purity = ApiPurity.ENGINE, Lifetime = "owning_script_generation_and_scene", Summary = "Returns a rented object to its owner hierarchy and deactivates it.", Returns = "False for an object not currently rented from this pool or when reparenting fails.", Failure = "false_for_nonmember_not_rented_or_failed_reparent; null_disposed_expired_owner_or_invalid_scope_throws")]
    public bool Return(GameObject item)
    {
        RequireOwner(); ArgumentNullException.ThrowIfNull(item);
        for (int index = 0; index < m_items.Length; index++)
        {
            if (m_items[index].ObjectId != item.ObjectId || m_items[index].SceneLifetime != item.SceneLifetime || !m_rented[index]) continue;
            if (item.IsAlive) {
                if (!item.SetParent(m_owner.GameObject)) return false;
                item.SetActive(false);
            }
            m_rented[index] = false; return true;
        }
        return false;
    }
    [ApiContract(Call = "live", RunModes = new[] { "Play" }, Purity = ApiPurity.ENGINE, Lifetime = "owning_script_generation_and_scene", Summary = "Queues all pool objects for destruction and ends further rentals.", Failure = "already_disposed_is_noop; expired_owner_or_invalid_Play_scope_throws")]
    public void Dispose()
    {
        if (m_disposed) return;
        RequireOwner();
        m_owner.Scene.DestroyMany(m_items);
        m_disposed = true;
    }
    void RequireOwner()
    {
        if (m_disposed) throw new ObjectDisposedException(nameof(ObjectPool));
        ScriptApiRuntime.RequireScope(m_owner);
        if (!m_owner.GameObject.IsAlive) throw new InvalidOperationException("The pool owner has expired.");
    }
}
