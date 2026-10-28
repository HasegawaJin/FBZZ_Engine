/// @file    ScriptStores.cs
/// @brief   実 Application の別 Save/Config ストアへの明示的な保存 API。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;

namespace Fbzz;

/// @note 全操作は Play の同期 callback。setter はメモリだけを変更し、Save/Load だけが disk I/O を行う。
public abstract class ScriptStore
{
    private protected ScriptStore(ScriptBinding owner, uint service) { m_owner = owner; m_service = service; }
    readonly ScriptBinding m_owner;
    readonly uint m_service;
    /// @note 相対 path は実行ファイルの directory 基準。path の変更は値を読み込まない。
    [ApiContract(RunModes = new[] { "Play" }, Read = "read", Write = "live", Summary = "Persistence path; relative paths resolve beside the executable. Setting it does not load data.")]
    public string Path { get => Call(0, true).String; set { ArgumentException.ThrowIfNullOrWhiteSpace(value); if (value.Contains('\0')) throw new ArgumentException("Save path cannot contain NUL.", nameof(value)); Call(1, false, text: [value]); } }
    [ApiContract(RunModes = new[] { "Play" }, Read = "read")]
    public bool IsDirty => Call(2, true).Flag;
    [ApiContract(RunModes = new[] { "Play" }, Call = "read")]
    public bool Has(string key) => KeyCall(3, true, key).Flag;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Summary = "Removes a memory key; persistence changes only after Save.")]
    public void Remove(string key) => KeyCall(4, false, key);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Summary = "Clears memory values only; the disk file is retained until Save.")]
    public void Clear() => Call(5, false);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "false_for_io_failure; previous_disk_and_dirty_state_preserved", Summary = "Explicit synchronous disk write; owned temporary replacement succeeds before clearing dirty state.")]
    public bool Save() => Call(6, false).Flag;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "false_for_unreadable_or_corrupt_file_preserves_values_and_dirty", Summary = "Explicit synchronous load; a genuinely missing file succeeds with empty clean values. It does not reset the other store.")]
    public bool Load() => Call(7, false).Flag;
    [ApiContract(RunModes = new[] { "Play" }, Call = "read", Failure = "missing_or_wrong_type_returns_defaultValue")]
    public bool GetBool(string key, bool defaultValue = false) => KeyCall(8, true, key, defaultValue ? 1 : 0).Flag;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void SetBool(string key, bool value) => KeyCall(9, false, key, value ? 1 : 0);
    [ApiContract(RunModes = new[] { "Play" }, Call = "read", Failure = "missing_or_wrong_type_returns_defaultValue")]
    public int GetInt(string key, int defaultValue = 0) => KeyCall(10, true, key, defaultValue).Int32;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void SetInt(string key, int value) => KeyCall(11, false, key, value);
    [ApiContract(RunModes = new[] { "Play" }, Call = "read", Failure = "missing_or_wrong_type_returns_defaultValue")]
    public float GetFloat(string key, float defaultValue = 0f) => KeyCall(12, true, key, values: [defaultValue]).Scalar;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void SetFloat(string key, float value) => KeyCall(13, false, key, values: [value]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "read", Failure = "missing_or_wrong_type_returns_defaultValue")]
    public string GetString(string key, string defaultValue = "") { ArgumentNullException.ThrowIfNull(defaultValue); NativeServiceResult result = KeyCall(14, true, key); return result.Flag ? result.String : defaultValue; }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void SetString(string key, string value)
    {
        ArgumentNullException.ThrowIfNull(value); ValidateKey(key);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        scope.Bridge.WriteStoreString(scope, m_owner.GameObject.ObjectId, m_service, key, value);
    }
    [ApiContract(RunModes = new[] { "Play" }, Call = "read", Failure = "missing_or_wrong_type_returns_defaultValue")]
    public Vector2 GetVector2(string key, Vector2 defaultValue = default) => KeyCall(16, true, key, values: [defaultValue.X, defaultValue.Y]).Vector2;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void SetVector2(string key, Vector2 value) => KeyCall(17, false, key, values: [value.X, value.Y]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "read", Failure = "missing_or_wrong_type_returns_defaultValue")]
    public Vector3 GetVector3(string key, Vector3 defaultValue = default) => KeyCall(18, true, key, values: [defaultValue.X, defaultValue.Y, defaultValue.Z]).Vector3;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void SetVector3(string key, Vector3 value) => KeyCall(19, false, key, values: [value.X, value.Y, value.Z]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "read", Failure = "missing_or_wrong_type_returns_defaultValue")]
    public Vector4 GetVector4(string key, Vector4 defaultValue = default) => KeyCall(20, true, key, values: [defaultValue.X, defaultValue.Y, defaultValue.Z, defaultValue.W]).Vector4;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void SetVector4(string key, Vector4 value) => KeyCall(21, false, key, values: [value.X, value.Y, value.Z, value.W]);
    /// @note Serializable field schema を保存する。live Scene 参照は再帰的に拒否し、asset GUID は値として保持する。
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "unsupported_scene_bound_or_invalid_value_throws_before_publication; conflict_preserves_old_value", Summary = "Writes a complete portable typed value through an owned staged TOML document. No user getter, constructor or validation callback runs.")]
    public void Write<T>(string key, T value)
    {
        ValidateKey(key); ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        scope.Bridge.WriteStoreDocument(scope, m_owner.GameObject.ObjectId, m_service, key, typeof(T), value);
    }
    /// @note defaults は受動的に複製し、失敗時も変更しない。保存形式・型・schema が違う場合は明示 migration が必要。
    [ApiContract(RunModes = new[] { "Play" }, Call = "read", Failure = "false_and_default_out_for_missing_corrupt_or_incompatible_document; supplied_defaults_unchanged; invalid_scope_or_unsupported_type_throws", Summary = "Reads a detached portable value, preserving caller defaults for omitted fields under the matching schema. Explicit migration is required after schema changes.")]
    public bool TryRead<T>(string key, T defaults, out T value)
    {
        ValidateKey(key); ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        bool found = scope.Bridge.ReadStoreDocument(scope, m_owner.GameObject.ObjectId, m_service, key, typeof(T), defaults, out object? result);
        value = found ? (T)result! : default!; return found;
    }
    NativeServiceResult Call(uint method, bool read, long integer = 0, ReadOnlySpan<float> values = default, ReadOnlySpan<string> text = default)
        => RuntimeServiceCall.Invoke(m_owner, m_service, method, read, integer, values, text);
    NativeServiceResult KeyCall(uint method, bool read, string key, long integer = 0, ReadOnlySpan<float> values = default, ReadOnlySpan<string> text = default)
    { ValidateKey(key); return Call(method, read, integer, values, text.IsEmpty ? [key] : text); }
    static void ValidateKey(string key)
    { ArgumentException.ThrowIfNullOrWhiteSpace(key); if (key.Contains('\0')) throw new ArgumentException("Store key cannot contain NUL.", nameof(key)); }
}

/// @note 進行データ。Config と独立した Application ストアで、Play 停止でディスクの変更を巻き戻さない。
public sealed class ScriptSave : ScriptStore
{ internal ScriptSave(ScriptBinding owner) : base(owner, (uint)ScriptServiceKind.SERVICE_SAVE) { } }
/// @note 環境設定。Save の path 切替・Load/Clear で設定を消去しない。
public sealed class ScriptConfig : ScriptStore
{ internal ScriptConfig(ScriptBinding owner) : base(owner, (uint)ScriptServiceKind.SERVICE_CONFIG) { } }
