/// @file    RenderDisplayServices.cs
/// @brief   Play の runtime 描画設定と host 表示要求を扱う型付きサービス。
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
namespace Fbzz;
public enum RenderQuality { LOW = 0, MEDIUM = 1, HIGH = 2, ULTRA = 3 }
public readonly record struct DisplayResolution(int Width, int Height);
public sealed class ScriptRender
{
    readonly ScriptBinding m_owner;
    internal ScriptRender(ScriptBinding owner) { m_owner = owner; }
    NativeServiceResult Call(uint index, bool read, ReadOnlySpan<float> values = default, long integer = 0)
        => RuntimeServiceCall.Invoke(m_owner, 198, index, read, integer, values);
    void Set(uint index, float value) { if (!float.IsFinite(value)) throw new ArgumentOutOfRangeException(nameof(value)); Call(index, false, [value]); }
    /// @note 利用不可 context の getter は既定値を偽造しない。全設定は Play の専用 copy にだけ作用する。
    [ApiContract(Read = "query", RunModes = new[] { "Edit", "Play" }, Summary = "Reports whether a real Play render-settings copy is registered; unavailable getters do not fabricate defaults.")]
    public bool IsAvailable => Call(0, true).Flag;
    [ApiContract(Read = "query", Write = "live", Units = "multiplier", RunModes = new[] { "Play" }, Summary = "Reads or changes final brightness multiplier, clamped to0.1..4 by native settings.")]
    public float Brightness { get => Call(1, true).Scalar; set => Set(2, value); }
    [ApiContract(Read = "query", Write = "live", Units = "multiplier", RunModes = new[] { "Play" }, Summary = "Changes the0..2 bloom preference multiplier independently of enabling the bloom pass.")]
    public float BloomScale { get => Call(3, true).Scalar; set => Set(4, value); }
    [ApiContract(Read = "query", Write = "live", Units = "ratio", RunModes = new[] { "Play" }, Summary = "Changes internal game render scale0.5..2; output and UI remain at the viewport size.")]
    public float RenderScale { get => Call(5, true).Scalar; set => Set(6, value); }
    [ApiContract(Read = "query", Units = "pixels", RunModes = new[] { "Play" }, Summary = "Resolves internal width from the actual game viewport and runtime scale, not the Editor window.")]
    public int RenderWidth => Call(7, true).Int32;
    [ApiContract(Read = "query", Units = "pixels", RunModes = new[] { "Play" })]
    public int RenderHeight => Call(8, true).Int32;
    [ApiContract(Read = "query", Write = "live", RunModes = new[] { "Play" })]
    public RenderQuality Quality {
        get => (RenderQuality)Call(9, true).Int32;
        set { if ((uint)value > 3) throw new ArgumentOutOfRangeException(nameof(value)); Set(10, (int)value); }
    }
    [ApiContract(Read = "query", Write = "live", RunModes = new[] { "Play" })]
    public bool ShadowsEnabled { get => Call(11, true).Flag; set => Set(12, value ? 1 : 0); }
    [ApiContract(Read = "query", Write = "live", Units = "pixels", RunModes = new[] { "Play" }, Summary = "Changes atlas resolution using an exact integer; native clamps512..8192 without power-of-two rounding.")]
    public int ShadowResolution { get => Call(13, true).Int32; set { if (value <= 0) throw new ArgumentOutOfRangeException(nameof(value)); Call(14, false, integer: value); } }
    [ApiContract(Read = "query", Write = "live", Units = "count", RunModes = new[] { "Play" }, Summary = "Changes cascade count using an exact integer; every int clamps to1..4 without float transport.")]
    public int ShadowCascades { get => Call(15, true).Int32; set => Call(16, false, integer: value); }
    [ApiContract(Read = "query", Write = "live", RunModes = new[] { "Play" })]
    public bool SSR { get => Call(17, true).Flag; set => Set(18, value ? 1 : 0); }
    [ApiContract(Read = "query", Write = "live", RunModes = new[] { "Play" })]
    public bool GTAO { get => Call(19, true).Flag; set => Set(20, value ? 1 : 0); }
    [ApiContract(Read = "query", Write = "live", RunModes = new[] { "Play" })]
    public bool SSAO { get => Call(21, true).Flag; set => Set(22, value ? 1 : 0); }
    [ApiContract(Read = "query", Write = "live", RunModes = new[] { "Play" })]
    public bool TAA { get => Call(23, true).Flag; set => Set(24, value ? 1 : 0); }
    [ApiContract(Read = "query", Write = "live", RunModes = new[] { "Play" })]
    public bool FXAA { get => Call(25, true).Flag; set => Set(26, value ? 1 : 0); }
    [ApiContract(Read = "query", Write = "live", RunModes = new[] { "Play" })]
    public bool Bloom { get => Call(27, true).Flag; set => Set(28, value ? 1 : 0); }
    [ApiContract(Read = "query", Write = "live", RunModes = new[] { "Play" })]
    public bool MotionBlur { get => Call(29, true).Flag; set => Set(30, value ? 1 : 0); }
    [ApiContract(Read = "query", Write = "live", RunModes = new[] { "Play" })]
    public bool VolumetricLight { get => Call(31, true).Flag; set => Set(32, value ? 1 : 0); }
    [ApiContract(Read = "query", Write = "live", RunModes = new[] { "Play" })]
    public bool ContactShadow { get => Call(33, true).Flag; set => Set(34, value ? 1 : 0); }
}
public sealed class ScriptDisplay
{
    readonly ScriptBinding m_owner;
    internal ScriptDisplay(ScriptBinding owner) { m_owner = owner; }
    NativeServiceResult Call(uint index, bool read, long integer = 0, ReadOnlySpan<float> values = default)
        => RuntimeServiceCall.Invoke(m_owner, 199, index, read, integer, values);
    [ApiContract(Read = "query", ReadRunModes = new[] { "Edit", "Play" }, Summary = "Reports whether a live standalone window host accepts game display changes; false in the Editor.")]
    public bool CanChangeWindow => Call(0, true).Flag;
    [ApiContract(Read = "query", Units = "pixels", ReadRunModes = new[] { "Edit", "Play" }, Summary = "Reads actual host window client width; zero before window creation.")]
    public int ClientWidth => Call(1, true).Int32;
    [ApiContract(Read = "query", Units = "pixels", ReadRunModes = new[] { "Edit", "Play" })]
    public int ClientHeight => Call(2, true).Int32;
    [ApiContract(Read = "query", Units = "pixels", ReadRunModes = new[] { "Edit", "Play" })]
    public DisplayResolution MonitorSize {
        get { NativeServiceResult result = Call(3, true); return new(result.Int32, checked(int.Parse(result.Text[0], System.Globalization.CultureInfo.InvariantCulture))); }
    }
    [ApiContract(Read = "query", ReadRunModes = new[] { "Edit", "Play" }, Summary = "Reads the actual host window mode rather than a synthetic remembered request.")]
    public bool IsFullscreen => Call(4, true).Flag;
    [ApiContract(Read = "query", ReadRunModes = new[] { "Edit", "Play" })]
    public bool VSync => Call(5, true).Flag;
    /// @return Editor は false。true は所有 host の安全な frame 境界で適用する要求を受理したこと。
    [ApiContract(Call = "live", RunModes = new[] { "Play" }, Failure = "false_if_editor_or_host_unavailable; accepted_intent_dropped_if_scene_lifetime_ends; invalid_scope_throws", Summary = "Queues borderless fullscreen/windowed for the next safe host frame; Editor window changes are unavailable.")]
    public bool TrySetFullscreen(bool value) => Call(6, false, value ? 1 : 0).Flag;
    [ApiContract(Call = "live", ParameterUnits = new[] { "width=pixels", "height=pixels" }, RunModes = new[] { "Play" }, Failure = "false_if_editor_or_host_unavailable; no_resize_inside_callback; invalid_dimensions_throws")]
    public bool TrySetResolution(int width, int height) {
        if (width <= 0 || height <= 0) throw new ArgumentOutOfRangeException(nameof(width));
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.InvokeService(scope, m_owner.GameObject.ObjectId, 199, 7, width, [], [height.ToString(System.Globalization.CultureInfo.InvariantCulture)], null, false).Flag;
    }
    [ApiContract(Call = "live", RunModes = new[] { "Play" }, Failure = "false_if_editor_or_host_unavailable", Summary = "Queues standalone presentation VSync; does not change the Editor user's presentation preference.")]
    public bool TrySetVSync(bool value) => Call(8, false, value ? 1 : 0).Flag;
    /// @note NativeBridge の scoped snapshot/page decoder に委譲し、短い caller buffer は Required/Written=0・出力未変更。
    [ApiContract(Call = "read", RunModes = new[] { "Edit", "Play" }, Failure = "insufficient_capacity_required_written0_output_unchanged; malformed_snapshot_throws_output_unchanged", Summary = "Captures actual monitor modes once and copies the complete ordered result to caller storage; short output and unused tail remain unchanged.")]
    public CollectionReadResult GetResolutions(Span<DisplayResolution> buffer) {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.ReadDisplayResolutions(scope, m_owner.GameObject.ObjectId, buffer);
    }
}
