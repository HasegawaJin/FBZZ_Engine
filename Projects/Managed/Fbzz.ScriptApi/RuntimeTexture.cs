/// @file    RuntimeTexture.cs
/// @brief   Script-owned linear textures with synchronous, atomic rectangle uploads.
/// @author  Hasegawa Jin
/// @date    2026-10-26
using System;

namespace Fbzz;

public enum RuntimeTextureFormat { R8 = 0, RGBA8 = 1 }

/// @note The exact calling Script owns the texture; no asset GUID, serialization or shared-file mutation is involved.
public sealed class RuntimeTexture : IDisposable
{
    readonly ulong m_generation, m_instance, m_owner, m_scene, m_object;
    readonly IScriptNativeBridge m_bridge;
    readonly long m_token;
    bool m_disposed;

    RuntimeTexture(ScriptExecutionScope scope, ulong ownerObject, long token, int width, int height, RuntimeTextureFormat format)
    {
        m_generation = scope.Generation; m_instance = scope.InstanceId; m_owner = scope.OwnerId;
        m_scene = scope.SceneLifetime; m_object = ownerObject; m_bridge = scope.Bridge; m_token = token;
        Width = width; Height = height; Format = format;
    }

    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "pixels")]
    public int Width { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "pixels")]
    public int Height { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public RuntimeTextureFormat Format { get; }
    [ApiContract(RunModes = new[] { "Play" }, Read = "read", Failure = "disposed_retired_or_reset_returns_false; active_owner_requires_current_callback")]
    public bool IsValid
    {
        get
        {
            if (m_disposed) return false;
            ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
            if (scope.Generation != m_generation || scope.SceneLifetime != m_scene) return false;
            RequireOwner(scope);
            return scope.Bridge.InvokeService(scope, m_object, (uint)ScriptServiceKind.SERVICE_ASSETS, 15, m_token, [], [], null, true).Flag;
        }
    }

    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "width=pixels", "height=pixels" },
        Lifetime = "exact_script_owner_generation_scene; dispose_disable_destroy_reload_auto_release",
        Failure = "invalid_dimensions_scope_or_unavailable_backend_throws", Summary = "Creates a zero-cleared, linear R8 UNORM mask owned by the current Script.")]
    public static RuntimeTexture CreateR8(int width, int height) => Create(width, height, RuntimeTextureFormat.R8);

    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "width=pixels", "height=pixels" },
        Lifetime = "exact_script_owner_generation_scene; dispose_disable_destroy_reload_auto_release",
        Failure = "invalid_dimensions_scope_or_unavailable_backend_throws", Summary = "Creates a zero-cleared, linear RGBA8 UNORM texture owned by the current Script.")]
    public static RuntimeTexture CreateRGBA8(int width, int height) => Create(width, height, RuntimeTextureFormat.RGBA8);

    static RuntimeTexture Create(int width, int height, RuntimeTextureFormat format)
    {
        if (width <= 0 || width > 16384 || height <= 0 || height > 16384)
            throw new ArgumentOutOfRangeException(nameof(width), "Texture dimensions must be between 1 and 16384 pixels.");
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        long token = scope.Bridge.CreateRuntimeTexture(scope, width, height, format, out ulong ownerObject);
        if (token <= 0 || ownerObject == 0) throw new InvalidOperationException("The renderer could not create a runtime texture.");
        return new RuntimeTexture(scope, ownerObject, token, width, height, format);
    }

    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "x=pixels", "y=pixels", "width=pixels", "height=pixels", "pixels=UNORM_bytes", "rowPitch=bytes" },
        ParameterCoordinates = new[] { "x=top_left_texture", "y=top_left_texture" },
        Failure = "invalid_rectangle_pitch_or_short_span_throws_without_changes; expired_resource_or_backend_rejection_returns_false",
        Summary = "Copies and validates the complete rectangle, then publishes once; span memory is never retained after the synchronous call.")]
    public bool UpdateRegion(int x, int y, int width, int height, ReadOnlySpan<byte> pixels, int rowPitch = 0)
    {
        ScriptExecutionScope scope = Scope();
        int pixelBytes = Format == RuntimeTextureFormat.R8 ? 1 : 4;
        if (x < 0 || y < 0 || width <= 0 || height <= 0 || width > Width - x || height > Height - y)
            throw new ArgumentOutOfRangeException(nameof(width));
        int rowBytes = checked(width * pixelBytes);
        if (rowPitch == 0) rowPitch = rowBytes;
        if (rowPitch < rowBytes) throw new ArgumentOutOfRangeException(nameof(rowPitch));
        long required = checked((long)(height - 1) * rowPitch + rowBytes);
        if (required > pixels.Length) throw new ArgumentException("The pixel span does not cover the complete rectangle.", nameof(pixels));
        return scope.Bridge.UpdateRuntimeTexture(scope, m_object, m_token, x, y, width, height, rowPitch, pixels[..(int)required]);
    }

    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "repeat_dispose_is_noop; live_texture_requires_own_callback", Summary = "Releases the owned GPU resource deterministically; owner shutdown also releases it automatically.")]
    public void Dispose()
    {
        if (m_disposed) return;
        ScriptExecutionScope scope = ScriptApiRuntime.Current ?? throw new InvalidOperationException("Dispose a live runtime texture in its owner's callback.");
        if (scope.Generation != m_generation) { m_disposed = true; return; }
        RequireOwner(scope);
        if (ScriptLifecycleContract.GetExecutionPolicy(scope.Callback) == "no_engine_api") { m_disposed = true; return; }
        scope.Bridge.InvokeService(scope, m_object, (uint)ScriptServiceKind.SERVICE_ASSETS, 16, m_token, [], [], null, false);
        m_disposed = true;
    }

    internal long Token(ScriptExecutionScope scope) { if (m_disposed) throw new ObjectDisposedException(nameof(RuntimeTexture)); RequireOwner(scope); return m_token; }
    ScriptExecutionScope Scope() { ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(); Token(scope); return scope; }
    void RequireOwner(ScriptExecutionScope scope)
    {
        if (!ReferenceEquals(scope.Bridge, m_bridge) || scope.Generation != m_generation || scope.InstanceId != m_instance
            || scope.OwnerId != m_owner || scope.SceneLifetime != m_scene)
            throw new InvalidOperationException("Runtime textures belong to the exact creating Script, Scene and generation.");
    }
}
