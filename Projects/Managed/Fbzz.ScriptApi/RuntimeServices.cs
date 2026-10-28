/// @file    RuntimeServices.cs
/// @brief   所有カーソル要求・終了 intent・実 Scene 遷移と Light/Particle の型付き API。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;

namespace Fbzz;

public enum CursorLockMode { NONE = 0, CONFINED = 1, LOCKED = 2 }
public enum CursorInputOwner { HOST = 0, GAME = 1 }
public enum CursorSuspendReason {
    NONE = 0, NOT_PLAYING, RELEASED, WINDOW_INACTIVE, GAME_VIEW_UNFOCUSED,
    PAUSED, HOST_OVERRIDE, TARGET_UNAVAILABLE, WINDOW_MOVING
}
public enum CursorShape { DEFAULT = 0, CLICKABLE = 1, GRAB = 2, TEXT = 3, AIM = 4, BUSY = 5 }
public enum CursorPriority { CAMERA = 0, GAMEPLAY = 100, UI = 200, MODAL = 300 }
public enum LightType { DIRECTIONAL = 0, POINT = 1, SPOT = 2, AREA = 3, SPHERE = 4, TUBE = 5 }

internal static class RuntimeServiceCall
{
    internal static NativeServiceResult Invoke(ScriptBinding owner, uint service, uint method, bool readOnly,
        long integer = 0, ReadOnlySpan<float> values = default, ReadOnlySpan<string> text = default)
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(owner);
        return scope.Bridge.InvokeService(scope, owner.GameObject.ObjectId, service, method, integer, values, text, null, readOnly);
    }
}

public sealed class ScriptApplication
{
    internal ScriptApplication(ScriptBinding owner) { m_owner = owner; }
    readonly ScriptBinding m_owner;
    [ApiContract(Read = "read", Summary = "Reads the current Play session state; Pause remains in Play.")]
    public bool IsPlaying => RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_APPLICATION, 0, true).Flag;
    [ApiContract(Read = "read")]
    public bool IsRunning => RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_APPLICATION, 1, true).Flag;
    /// @note 実 window の client 幅。Game View viewport と別で、window 未作成は 0。
    [ApiContract(Read = "read", Units = "pixels", Summary = "Reads actual window client width in pixels; zero before window creation, distinct from game viewport width.")]
    public int WindowWidth => RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_APPLICATION, 2, true).Int32;
    [ApiContract(Read = "read", Units = "pixels", Summary = "Reads actual window client height in pixels; zero before window creation, distinct from game viewport height.")]
    public int WindowHeight => RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_APPLICATION, 3, true).Int32;
    /// @note callback 中に Scene を破棄しない。Editor は次 frame に Play を停止、Standalone は終了要求を保持する。
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "edit_or_connect_scope_throws", Summary = "Requests deferred Play stop in Editor or application exit in standalone; never closes the Editor window.")]
    public void Quit() => RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_APPLICATION, 4, false);
}

public sealed class ScriptCursor
{
    internal ScriptCursor(ScriptBinding owner) { m_owner = owner; }
    readonly ScriptBinding m_owner;
    [ApiContract(Read = "read")]
    /// @note Compatibility alias for RequestedVisible; this is game intent, not effective OS visibility.
    public bool Visible => RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_CURSOR, 4, true).Flag;
    [ApiContract(Read = "read")]
    /// @note Compatibility alias for RequestedLockMode.
    public CursorLockMode LockMode => (CursorLockMode)RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_CURSOR, 5, true).Int32;
    [ApiContract(Read = "read", Summary = "Winning requested visibility; Locked can make EffectiveVisible false while this remains true.")]
    public bool RequestedVisible => Visible;
    [ApiContract(Read = "read", Summary = "Winning movement restriction requested by the game; host release preserves this setting.")]
    public CursorLockMode RequestedLockMode => LockMode;
    /// @see https://docs.unity3d.com/ScriptReference/Cursor-lockState.html Locked implies effective hidden visibility.
    [ApiContract(Read = "read", Summary = "Host-applied visibility, including lock, focus and Editor release constraints.")]
    public bool EffectiveVisible => RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_CURSOR, 7, true).Flag;
    [ApiContract(Read = "read")]
    public CursorLockMode EffectiveLockMode => (CursorLockMode)RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_CURSOR, 8, true).Int32;
    [ApiContract(Read = "read", Summary = "Game or host owns gameplay input; independent of pointer visibility and movement.")]
    public CursorInputOwner InputOwner => (CursorInputOwner)RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_CURSOR, 9, true).Int32;
    [ApiContract(Read = "read")]
    public CursorSuspendReason SuspendedReason => (CursorSuspendReason)RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_CURSOR, 10, true).Int32;
    [ApiContract(Read = "read")]
    public bool WindowActive => RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_CURSOR, 11, true).Flag;
    [ApiContract(Read = "read")]
    public CursorShape Shape => (CursorShape)RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_CURSOR, 6, true).Int32;
    /// @note 同じ priority なら後勝ち。disable/fault/destroy/reload/Scene 変更で native owner cleanup が解除する。
    [ApiContract(RunModes = new[] { "Play" }, Call = "subscription", Summary = "Registers a Play cursor request owned by this Script lifetime; Connect may register it. Equal priority uses last-pushed order.", Failure = "edit_or_inactive_owner_or_invalid_policy_throws")]
    public CursorRequest Push(CursorLockMode lockMode, bool visible, int priority = (int)CursorPriority.GAMEPLAY)
    {
        ValidateMode(lockMode);
        NativeServiceResult result = RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_CURSOR, 0, false,
            checked((long)m_owner.OwnerId), [(float)lockMode, visible ? 1f : 0f], [priority.ToString(System.Globalization.CultureInfo.InvariantCulture)]);
        if (result.Integer <= 0) throw new InvalidOperationException("Cursor request returned an invalid lifetime token.");
        return new(m_owner, result.Integer);
    }
    internal static void ValidateMode(CursorLockMode mode)
    { if ((uint)mode > 2) throw new ArgumentOutOfRangeException(nameof(mode)); }
}

/// @note 非所有 request handle。Dispose は現在の同期 callback 内で行い、finalizer から Engine を呼ばない。
public sealed class CursorRequest : IDisposable
{
    internal CursorRequest(ScriptBinding owner, long token) { m_owner = owner; m_token = token; }
    readonly ScriptBinding m_owner;
    long m_token;
    bool HasCurrentOwner()
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        return m_owner.Active && m_owner.Generation == scope.Generation && m_owner.GameObject.SceneLifetime == scope.SceneLifetime;
    }
    [ApiContract(Read = "read", Failure = "false_for_released_or_expired_owner; invalid_callback_scope_throws")]
    public bool IsActive => m_token != 0 && HasCurrentOwner() && Invoke(3, true).Flag;
    /// @return 失効していれば false。順序と priority は保持して policy だけ変更する。
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "false_for_released_or_expired_request; edit_or_connect_scope_throws")]
    public bool Set(CursorLockMode lockMode, bool visible)
    {
        ScriptCursor.ValidateMode(lockMode);
        return m_token != 0 && HasCurrentOwner() && Invoke(1, false, [(float)lockMode, visible ? 1f : 0f]).Flag;
    }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "false_for_released_or_expired_request; invalid_callback_scope_throws")]
    public bool Release()
    {
        if (m_token == 0) return false;
        bool released = HasCurrentOwner() && Invoke(2, false).Flag;
        m_token = 0;
        return released;
    }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void Dispose() => Release();
    NativeServiceResult Invoke(uint method, bool readOnly, ReadOnlySpan<float> values = default)
        => RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_CURSOR, method, readOnly, m_token, values,
            [m_owner.OwnerId.ToString(System.Globalization.CultureInfo.InvariantCulture)]);
}

public sealed partial class ScriptScene
{
    /// @return Nearest upward-facing floor below reference across active Terrain. False returns default outputs.
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", ParameterUnits = new[] { "reference=m", "maxDistance=m" }, ParameterCoordinates = new[] { "reference=world" }, Failure = "false_and_default_for_miss_or_invalid_numeric_arguments; invalid_scope_throws", Summary = "Finds the nearest upward-facing floor below reference Y, including cave floors, without Physics initialization.")]
    public bool TryGetTerrainFloorAt(Vector3 reference, float maxDistance, out Vector3 position, out Vector3 normal)
        => TryGetTerrainSurface(2, reference, maxDistance, out position, out normal);
    /// @return Nearest downward-facing ceiling above reference across active Terrain. False returns default outputs.
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", ParameterUnits = new[] { "reference=m", "maxDistance=m" }, ParameterCoordinates = new[] { "reference=world" }, Failure = "false_and_default_for_miss_or_invalid_numeric_arguments; invalid_scope_throws", Summary = "Finds the nearest downward-facing ceiling above reference Y, including cave roofs, without Physics initialization.")]
    public bool TryGetTerrainCeilingAt(Vector3 reference, float maxDistance, out Vector3 position, out Vector3 normal)
        => TryGetTerrainSurface(3, reference, maxDistance, out position, out normal);
    bool TryGetTerrainSurface(uint method, Vector3 reference, float maxDistance, out Vector3 position, out Vector3 normal)
    {
        ScriptApiRuntime.RequireScope(m_owner);
        position = normal = default;
        if (!reference.IsFinite || !float.IsFinite(maxDistance) || maxDistance < 0) return false;
        NativeServiceResult result = RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_SCENE, method, true,
            values: [reference.X, reference.Y, reference.Z, maxDistance]);
        if (!result.Flag) { if (result.ValueCount != 0) throw new InvalidOperationException("Invalid Terrain miss response."); return false; }
        if (result.ValueCount != 6) throw new InvalidOperationException("Invalid Terrain surface response.");
        position = new(result.Value(0), result.Value(1), result.Value(2));
        normal = new(result.Value(3), result.Value(4), result.Value(5));
        return true;
    }
    /// @note 実 SceneManager の登録名。外部 Scene を bind している間は空。
    [ApiContract(Read = "read", Summary = "Reads the active registered SceneManager name; external scene bindings return empty.")]
    public string RegisteredName => RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_SCENE, 0, true).String;
    /// @return queue 受付なら true、未登録名なら false。factory 完了とは区別する。
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "false_for_unknown_registered_name; edit_or_connect_scope_or_unbound_manager_throws", Summary = "Queues a registered scene for the next frame; the current callback retains its current scene.")]
    public bool TryLoadRegistered(string name)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        if (name.Contains('\0')) throw new ArgumentException("Scene names cannot contain NUL.", nameof(name));
        return RuntimeServiceCall.Invoke(m_owner, (uint)ScriptServiceKind.SERVICE_SCENE, 1, false, text: [name]).Flag;
    }
}

public sealed class Light : NativeComponent
{
    internal Light(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.LIGHT, lifetimeToken) { }
    /// @note native LightComponent の型別 authoring 単位。lux / lumen への変換を行わない。
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "read", Write = "live", Units = "native_type_relative_intensity")]
    public float Intensity { get => Invoke((uint)ScriptServiceKind.SERVICE_LIGHT, 2, true).Scalar; set { Nonnegative(value); Invoke((uint)ScriptServiceKind.SERVICE_LIGHT, 9, false, values: [value]); } }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "read", Write = "live", Units = "linear_rgb")]
    public Vector3 Color { get => Invoke((uint)ScriptServiceKind.SERVICE_LIGHT, 0, true).Vector3; set { if (!value.IsFinite || value.X < 0 || value.Y < 0 || value.Z < 0) throw new ArgumentOutOfRangeException(nameof(value)); Invoke((uint)ScriptServiceKind.SERVICE_LIGHT, 7, false, values: [value.X, value.Y, value.Z]); } }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "read", Write = "live")]
    public LightType Type { get => (LightType)Invoke((uint)ScriptServiceKind.SERVICE_LIGHT, 1, true).Int32; set { if ((uint)value > 5) throw new ArgumentOutOfRangeException(nameof(value)); Invoke((uint)ScriptServiceKind.SERVICE_LIGHT, 8, false, (long)value); } }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "read", Write = "live", Units = "m")]
    public float Range { get => Invoke((uint)ScriptServiceKind.SERVICE_LIGHT, 3, true).Scalar; set { Nonnegative(value); Invoke((uint)ScriptServiceKind.SERVICE_LIGHT, 10, false, values: [value]); } }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "read", Units = "degrees")]
    public float InnerCone => Invoke((uint)ScriptServiceKind.SERVICE_LIGHT, 4, true).Scalar;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "read", Units = "degrees")]
    public float OuterCone => Invoke((uint)ScriptServiceKind.SERVICE_LIGHT, 5, true).Scalar;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "read", Write = "live")]
    public bool Enabled { get => Invoke((uint)ScriptServiceKind.SERVICE_LIGHT, 6, true).Flag; set => Invoke((uint)ScriptServiceKind.SERVICE_LIGHT, 12, false, value ? 1 : 0); }
    /// @note 有限な 0 &lt;= inner &lt;= outer &lt;= 180 の全件検証後に変更する。
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", ParameterUnits = new[] { "inner=degrees", "outer=degrees" })]
    public void SetSpotAngles(float inner, float outer)
    { if (!float.IsFinite(inner) || !float.IsFinite(outer) || inner < 0 || inner > outer || outer > 180) throw new ArgumentOutOfRangeException(nameof(inner)); Invoke((uint)ScriptServiceKind.SERVICE_LIGHT, 11, false, values: [inner, outer]); }
    static void Nonnegative(float value) { if (!float.IsFinite(value) || value < 0) throw new ArgumentOutOfRangeException(nameof(value)); }
}

public sealed partial class ParticleSystem : NativeComponent
{
    internal ParticleSystem(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.PARTICLE_SYSTEM, lifetimeToken) { }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, ReadRunModes = new[] { "Edit", "Play" }, WriteRunModes = new[] { "Play" }, Read = "read", Write = "live", Summary = "Component enabled state, independent of playback. Changes require Play.")]
    public bool Enabled { get => Invoke((uint)ScriptServiceKind.SERVICE_PARTICLE, 0, true).Flag; set => Invoke((uint)ScriptServiceKind.SERVICE_PARTICLE, 10, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "read")]
    public bool IsPlaying => Invoke((uint)ScriptServiceKind.SERVICE_PARTICLE, 1, true).Flag;
    /// @note GPU simulation は CPU 粒子列を持たないため 0。
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "read", Units = "particles", Summary = "Live CPU particle count; GPU simulation reports zero.")]
    public int ParticleCount => Invoke((uint)ScriptServiceKind.SERVICE_PARTICLE, 2, true).Int32;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, ReadRunModes = new[] { "Edit", "Play" }, WriteRunModes = new[] { "Play" }, Read = "read", Write = "live", Units = "particles/s")]
    public float EmissionRate { get => Invoke((uint)ScriptServiceKind.SERVICE_PARTICLE, 3, true).Scalar; set { Nonnegative(value); Invoke((uint)ScriptServiceKind.SERVICE_PARTICLE, 11, false, values: [value]); } }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, ReadRunModes = new[] { "Edit", "Play" }, WriteRunModes = new[] { "Play" }, Read = "read", Write = "live", Units = "s")]
    public float Lifetime { get => Invoke((uint)ScriptServiceKind.SERVICE_PARTICLE, 4, true).Scalar; set { if (!float.IsFinite(value) || value < 0.01f) throw new ArgumentOutOfRangeException(nameof(value)); Invoke((uint)ScriptServiceKind.SERVICE_PARTICLE, 12, false, values: [value]); } }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "read", Units = "s")]
    public float PlaybackTime => Invoke((uint)ScriptServiceKind.SERVICE_PARTICLE, 5, true).Scalar;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Summary = "Starts playback in Play; restart resets existing particles and playback state.")]
    public void Play(bool restart = true) => Invoke((uint)ScriptServiceKind.SERVICE_PARTICLE, 6, false, restart ? 1 : 0);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Summary = "Stops emission in Play; clear=false keeps existing particles alive.")]
    public void Stop(bool clear = false) => Invoke((uint)ScriptServiceKind.SERVICE_PARTICLE, 7, false, clear ? 1 : 0);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "count=particles" }, Failure = "negative_count_or_pending_count_overflow_throws_without_changes")]
    public void Burst(int count) { if (count < 0) throw new ArgumentOutOfRangeException(nameof(count)); Invoke((uint)ScriptServiceKind.SERVICE_PARTICLE, 8, false, count); }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Summary = "Clears existing particles and accumulation in Play without changing playback enabled state.")]
    public void Clear() => Invoke((uint)ScriptServiceKind.SERVICE_PARTICLE, 9, false);
    static void Nonnegative(float value) { if (!float.IsFinite(value) || value < 0) throw new ArgumentOutOfRangeException(nameof(value)); }
}
