/// @file    AssetLeases.cs
/// @brief   実Scriptが所有するasync利用権と、別所有のSceneManager先読み状態。
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
using System.Collections.Generic;
using System.Globalization;
namespace Fbzz;

public readonly struct AssetLoadStatus
{
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "unknown_state_error_or_quality_throws; null_diagnostic_throws")]
    public AssetLoadStatus(AssetLoadState state, AssetLoadError error, uint publishedRevision, bool updateFailed,
        int residentQuality, int desiredQuality, ulong residentBytes, string diagnostic)
    {
        if ((uint)state > 8 || (uint)error > 8 || (uint)residentQuality > 255 || (uint)desiredQuality > 255)
            throw new ArgumentOutOfRangeException(nameof(state));
        ArgumentNullException.ThrowIfNull(diagnostic);
        State = state; Error = error; PublishedRevision = publishedRevision; UpdateFailed = updateFailed;
        ResidentQuality = residentQuality; DesiredQuality = desiredQuality; ResidentBytes = residentBytes; Diagnostic = diagnostic;
    }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")] public AssetLoadState State { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")] public AssetLoadError Error { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")] public uint PublishedRevision { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")] public bool UpdateFailed { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")] public int ResidentQuality { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")] public int DesiredQuality { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "bytes")] public ulong ResidentBytes { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")] public string Diagnostic { get; }
}
/// @note 非保存handle。disable/fault/requirementsBlocked/destroy/reloadでnative解放とmanaged失効を行う。completion callbackは持たない。
public sealed class AssetLease : IDisposable
{
    readonly ScriptBinding m_owner;
    readonly ulong m_token, m_generation, m_instance, m_object, m_scene;
    bool m_released;
    internal AssetLease(ScriptBinding owner, ulong token)
    { m_owner = owner; m_token = token; m_generation = owner.Generation; m_instance = owner.InstanceId; m_object = owner.GameObject.ObjectId; m_scene = owner.GameObject.SceneLifetime; }
    NativeServiceResult Call(uint index, bool read, ReadOnlySpan<string> text = default)
    {
        ScriptExecutionScope scope = ScriptAssets.OwnerScope(m_owner);
        if (scope.Generation != m_generation || scope.InstanceId != m_instance || scope.SceneLifetime != m_scene || m_owner.GameObject.ObjectId != m_object)
            throw new InvalidOperationException("Asset lease owner generation or Scene has expired.");
        return scope.Bridge.InvokeService(scope, m_object, (uint)ScriptServiceKind.SERVICE_ASSETS, index,
            unchecked((long)m_token), [], text, null, read);
    }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Summary = "Reports local release or retired Script binding; performs no native query.")]
    public bool IsReleased => m_released || !m_owner.Active;
    [ApiContract(RunModes = new[] { "Play" }, Read = "read", Failure = "released_or_retired_returns_none; malformed_reply_or_wrong_scope_throws", Summary = "Reads the actual async lease. UpdateFailed can coexist with READY while the previously published asset remains usable.")]
    public AssetLoadStatus Status => IsReleased ? new(AssetLoadState.NONE, AssetLoadError.NONE, 0, false, 0, 0, 0, "released") : ParseStatus(Call(1, true));
    [ApiContract(RunModes = new[] { "Play" }, Read = "read")] public bool IsReady => !IsReleased && Status.State == AssetLoadState.READY;
    [ApiContract(RunModes = new[] { "Play" }, Read = "read")] public bool IsFailed => !IsReleased && Status.State == AssetLoadState.FAILED;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "released_returns_false; wrong_scope_throws")]
    public bool Retry() => !IsReleased && Call(3, false).Flag;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "quality=quality_0_to_255" }, Failure = "released_or_invalid_quality_throws_without_changes")]
    public void SetQuality(int quality)
    {
        if ((uint)quality > 255) throw new ArgumentOutOfRangeException(nameof(quality));
        if (IsReleased) throw new ObjectDisposedException(nameof(AssetLease));
        Call(2, false, [quality.ToString(CultureInfo.InvariantCulture)]);
    }
    /// @return 既解放/世代退役はfalse。自動解放後のOnDestroyでもnative authorityを要求しない。
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "released_or_retired_returns_false_without_native_call; active_lease_requires_own_live_callback")]
    public bool Release()
    {
        if (IsReleased) { m_released = true; return false; }
        bool released = Call(4, false).Flag; Invalidate(); return released;
    }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "idempotent_after_auto_cleanup_in_OnDestroy; active_lease_requires_own_live_callback")]
    public void Dispose() => Release();
    internal void Invalidate() { m_released = true; m_owner.Assets.Remove(this); }
    internal static uint Unsigned(string? text)
    { ulong value = Wide(text); return value <= uint.MaxValue ? (uint)value : throw new InvalidOperationException("Asset count exceeds UInt32."); }
    static ulong Wide(string? text)
    {
        if (string.IsNullOrEmpty(text) || (text.Length > 1 && text[0] == '0') || !ulong.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out ulong value))
            throw new InvalidOperationException("Invalid canonical asset unsigned count.");
        return value;
    }
    static int Integral(float value, int maximum)
    {
        if (!float.IsFinite(value) || value < 0 || value > maximum || value != MathF.Truncate(value))
            throw new InvalidOperationException("Invalid asset state, error, boolean or quality.");
        return (int)value;
    }
    internal static AssetLoadStatus ParseStatus(NativeServiceResult result)
    {
        if (result.ValueCount != 5 || result.Text is null || result.Text.Length != 2 || result.Text[1] is null || result.GameObject is not null
            || result.Integer < 0 || (ulong)result.Integer > uint.MaxValue) throw new InvalidOperationException("Invalid asset status reply.");
        return new((AssetLoadState)Integral(result.Value(0), 8), (AssetLoadError)Integral(result.Value(1), 8), (uint)result.Integer,
            Integral(result.Value(2), 1) != 0, Integral(result.Value(3), 255), Integral(result.Value(4), 255), Wide(result.Text[0]), result.Text[1]);
    }
}
public sealed class ScriptAssets
{
    readonly ScriptBinding m_owner;
    readonly HashSet<AssetLease> m_owned = [];
    internal ScriptAssets(ScriptBinding owner) { m_owner = owner; }
    internal static ScriptExecutionScope OwnerScope(ScriptBinding owner)
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(owner);
        if (scope.InstanceId != owner.InstanceId || scope.OwnerId != owner.OwnerId)
            throw new InvalidOperationException("Asset requests and leases belong to the actual calling Script.");
        return scope;
    }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "missing_wrong_type_or_unadmitted_returns_null; wrong_caller_scope_or_empty_reference_throws", Summary = "Acquires a real async Texture lease owned by the exact calling Script; does not synchronously load.")]
    public AssetLease? Request(TextureAssetRef reference, bool prefetch = false) => Request(reference.Reference, 0, prefetch);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "missing_wrong_type_or_unadmitted_returns_null; wrong_caller_scope_or_empty_reference_throws")]
    public AssetLease? Request(MaterialAssetRef reference, bool prefetch = false) => Request(reference.Reference, 1, prefetch);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterHelp = new[] { "guidReference=Model asset GUID or guid: reference; file paths are rejected." }, Failure = "missing_wrong_type_or_unadmitted_returns_null; invalid_GUID_or_scope_throws")]
    public AssetLease? RequestModel(string guidReference, bool prefetch = false) => Request(AssetReferenceGuid.Canonical(guidReference), 2, prefetch);
    AssetLease? Request(string reference, int kind, bool prefetch)
    {
        ScriptExecutionScope scope = OwnerScope(m_owner);
        if (string.IsNullOrWhiteSpace(reference) || !reference.StartsWith("guid:", StringComparison.Ordinal) || reference.Contains('\0')) throw new ArgumentException("An asset GUID reference is required.", nameof(reference));
        NativeServiceResult result = scope.Bridge.InvokeService(scope, m_owner.GameObject.ObjectId, (uint)ScriptServiceKind.SERVICE_ASSETS, 0, kind, [prefetch ? 1 : 0], [reference], null, false);
        if (result.ValueCount != 0 || result.Text is null || result.Text.Length != 0 || result.GameObject is not null) throw new InvalidOperationException("Invalid asset request reply.");
        if (result.Integer == 0) return null;
        AssetLease lease = new(m_owner, unchecked((ulong)result.Integer)); m_owned.Add(lease); return lease;
    }
    /// @note この先読みbundleはSceneManagerが所有する。Script leaseの破棄では他者と共有するbundleを取消しない。
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "unknown_or_nonfile_scene_returns_false; unbound_manager_or_wrong_scope_throws", Summary = "Requests real registered-file SceneManager prefetch; never switches scenes. SceneManager owns the shared bundle until cancellation, Stop or shutdown.")]
    public bool PrefetchScene(string name) { ArgumentException.ThrowIfNullOrEmpty(name); return Invoke(5, false, name).Flag; }
    [ApiContract(RunModes = new[] { "Play" }, Call = "read", Units = "fraction_0_to_1", Summary = "Returns the native SceneManager READY plus FAILED fraction; unknown or empty bundles report zero.")]
    public float GetScenePrefetchProgress(string name)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        NativeServiceResult result = Invoke(6, true, name);
        if (result.ValueCount != 1 || result.Text is null || result.Text.Length != 0 || result.GameObject is not null
            || !float.IsFinite(result.Value(0)) || result.Value(0) < 0 || result.Value(0) > 1)
            throw new InvalidOperationException("Invalid Scene prefetch progress reply.");
        return result.Value(0);
    }
    NativeServiceResult Invoke(uint index, bool read, string name)
    {
        ScriptExecutionScope scope = OwnerScope(m_owner);
        return scope.Bridge.InvokeService(scope, m_owner.GameObject.ObjectId, (uint)ScriptServiceKind.SERVICE_ASSETS, index, 0, [], [name], null, read);
    }
    internal void Remove(AssetLease value) => m_owned.Remove(value);
    internal void InvalidateOwned()
    { foreach (AssetLease lease in new List<AssetLease>(m_owned)) lease.Invalidate(); m_owned.Clear(); }
}
