/// @file    Services.cs
/// @brief   既存 native Component の寿命を照合する型付きゲーム用サービス。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;

namespace Fbzz;

/// @note 設定保存にも対応する値型。座標系と単位は各サービスの契約に従う。
public readonly struct Vector2
{
    public Vector2(float x, float y) { X = x; Y = y; }
    public float X { get; }
    public float Y { get; }
    public static Vector2 ZERO => new(0f, 0f);
}

/// @note 色では W が alpha。HDR RGB を丸めない。
public readonly struct Vector4
{
    public Vector4(float x, float y, float z, float w) { X = x; Y = y; Z = z; W = w; }
    public float X { get; }
    public float Y { get; }
    public float Z { get; }
    public float W { get; }
}

/// @note Host bridge 用の値応答。Engine の所有 pointer を含めない。
public readonly struct NativeServiceResult
{
    public NativeServiceResult(float[] values, string[] text, long integer, GameObject? gameObject)
        : this(values.AsSpan(), text, integer, gameObject) { }
    /// @note 小さい値応答は inline に所有し、借用 packet や pool の記憶域を保持しない。
    internal NativeServiceResult(ReadOnlySpan<float> values, string[] text, long integer, GameObject? gameObject, float[]? ownedValues = null)
    {
        ValueCount = values.Length;
        m_values = values.Length > 8 ? ownedValues ?? values.ToArray() : null;
        m_0 = values.Length > 0 ? values[0] : 0f; m_1 = values.Length > 1 ? values[1] : 0f;
        m_2 = values.Length > 2 ? values[2] : 0f; m_3 = values.Length > 3 ? values[3] : 0f;
        m_4 = values.Length > 4 ? values[4] : 0f; m_5 = values.Length > 5 ? values[5] : 0f;
        m_6 = values.Length > 6 ? values[6] : 0f; m_7 = values.Length > 7 ? values[7] : 0f;
        Text = text; Integer = integer; GameObject = gameObject;
    }
    readonly float[]? m_values;
    readonly float m_0, m_1, m_2, m_3, m_4, m_5, m_6, m_7;
    /// @return 呼出元が保持できる通常配列。小さい inline 応答は、この明示的な配列投影時にだけ確保する。
    public float[] Values
    {
        get
        {
            if (m_values is not null) return m_values;
            if (ValueCount == 0) return [];
            float[] result = new float[ValueCount];
            for (int index = 0; index < result.Length; index++) result[index] = Value(index);
            return result;
        }
    }
    internal int ValueCount { get; }
    internal float Value(int index)
    {
        if ((uint)index >= (uint)ValueCount) throw new ArgumentOutOfRangeException(nameof(index));
        if (m_values is not null) return m_values[index];
        return index switch { 0 => m_0, 1 => m_1, 2 => m_2, 3 => m_3, 4 => m_4, 5 => m_5, 6 => m_6, _ => m_7 };
    }
    public string[] Text { get; }
    public long Integer { get; }
    public GameObject? GameObject { get; }
    internal float Scalar => ValueCount == 1 ? Value(0) : throw new InvalidOperationException("Invalid native scalar response.");
    internal bool Flag => Integer switch { 0 => false, 1 => true, _ => throw new InvalidOperationException("Invalid native flag response.") };
    internal int Int32 => checked((int)Integer);
    internal string String => Text.Length == 1 ? Text[0] : throw new InvalidOperationException("Invalid native string response.");
    internal Vector2 Vector2 => ValueCount == 2 ? new(Value(0), Value(1)) : throw new InvalidOperationException("Invalid native Vector2 response.");
    internal Vector3 Vector3 => ValueCount == 3 ? new(Value(0), Value(1), Value(2)) : throw new InvalidOperationException("Invalid native Vector3 response.");
    internal Vector4 Vector4 => ValueCount == 4 ? new(Value(0), Value(1), Value(2), Value(3)) : throw new InvalidOperationException("Invalid native Vector4 response.");
    internal Quaternion Quaternion => ValueCount == 4 ? new(Value(0), Value(1), Value(2), Value(3)) : throw new InvalidOperationException("Invalid native Quaternion response.");
}

/// @note 取得時の Component birth token を保持する。除去・再追加で復活せず、明示的な GetComponent が新しい参照を返す。
public abstract class NativeComponent : Component, IEquatable<NativeComponent>
{
    internal NativeComponent(GameObject owner, uint kind, ulong lifetimeToken = 0)
    { m_owner = owner; m_kind = kind; LifetimeToken = lifetimeToken != 0 ? lifetimeToken : NativeComponentFactory.CaptureLifetime(owner, kind); }
    internal ulong LifetimeToken { get; }
    readonly GameObject m_owner;
    readonly uint m_kind;
    /// @note 純粋な取得時 identity 比較。失効で null 扱いにせず、Engine scope を要求しない。
    public bool Equals(NativeComponent? other) => other is not null && m_kind == other.m_kind
        && LifetimeToken == other.LifetimeToken && m_owner.Equals(other.m_owner);
    public override bool Equals(object? other) => other is NativeComponent component && Equals(component);
    public override int GetHashCode() => HashCode.Combine(m_owner, m_kind, LifetimeToken);
    public override GameObject GameObject => m_owner;
    public override bool IsAlive
    {
        get
        {
            ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
            return m_owner.SceneLifetime == scope.SceneLifetime && LifetimeToken != 0
                && scope.Bridge.ReadComponentLifetime(scope, m_owner.ObjectId, m_kind) == LifetimeToken;
        }
    }
    private protected ScriptExecutionScope Scope()
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        if (m_owner.SceneLifetime != scope.SceneLifetime) throw new InvalidOperationException("The Component belongs to another scene.");
        if (LifetimeToken == 0 || scope.Bridge.ReadComponentLifetime(scope, m_owner.ObjectId, m_kind) != LifetimeToken)
            throw new InvalidOperationException("The Component lifetime has expired; obtain a new Component reference explicitly.");
        return scope;
    }
    private protected UI WidgetUI() { Scope(); return m_owner.UI; }
    protected NativeServiceResult Invoke(uint service, uint method, bool readOnly, long integer = 0,
        ReadOnlySpan<float> values = default, ReadOnlySpan<string> text = default, GameObject? related = null)
    {
        ScriptExecutionScope scope = Scope();
        foreach (float value in values) if (!float.IsFinite(value)) throw new ArgumentOutOfRangeException(nameof(values));
        foreach (string value in text) ArgumentNullException.ThrowIfNull(value);
        if (related is not null && (related.SceneLifetime != scope.SceneLifetime || related.ObjectId == 0))
            throw new InvalidOperationException("The related reference is unresolved or belongs to another scene.");
        return scope.Bridge.InvokeService(scope, m_owner.ObjectId, service, method, integer, values, text, related, readOnly);
    }
}

public enum CameraClearMode { SOLID_COLOR = 0, DEPTH_ONLY = 1 }
public readonly struct Ray
{
    public Ray(Vector3 origin, Vector3 direction) { Origin = origin; Direction = direction; }
    public Vector3 Origin { get; }
    public Vector3 Direction { get; }
    internal static Ray From(NativeServiceResult result)
    {
        if (result.ValueCount != 6) throw new InvalidOperationException("Invalid native Ray response.");
        return new(new(result.Value(0), result.Value(1), result.Value(2)), new(result.Value(3), result.Value(4), result.Value(5)));
    }
}
public readonly struct BlendWeight
{
    public BlendWeight(string name, float weight) { Name = name; Weight = weight; }
    public string Name { get; }
    public float Weight { get; }
}

/// @note Play の同期 callback 内で既存 Animator サービスへ転送する。
public sealed class Animator : NativeComponent
{
    internal Animator(GameObject owner, ulong lifetimeToken = 0) : base(owner, 5, lifetimeToken) { }
    public void SetFloat(string name, float value) => Invoke(180, 0, false, 0, [value], [name], null);
    [ApiContract(Call = "read")]
    public float GetFloat(string name) => Invoke(180, 1, true, 0, [], [name], null).Scalar;
    public void SetInt(string name, int value) => Invoke(180, 2, false, (long)value, [], [name], null);
    [ApiContract(Call = "read")]
    public int GetInt(string name) => Invoke(180, 3, true, 0, [], [name], null).Int32;
    public void SetBool(string name, bool value) => Invoke(180, 4, false, value ? 1 : 0, [], [name], null);
    [ApiContract(Call = "read")]
    public bool GetBool(string name) => Invoke(180, 5, true, 0, [], [name], null).Flag;
    public void SetTrigger(string name) => Invoke(180, 6, false, 0, [], [name], null);
    [ApiContract(Call = "read")]
    public bool IsInState(string name) => Invoke(180, 7, true, 0, [], [name], null).Flag;
    [ApiContract(Call = "read")]
    public float GetNormalizedTime() => Invoke(180, 8, true, 0, [], [], null).Scalar;
    [ApiContract(Call = "read")]
    public string GetCurrentState() => Invoke(180, 9, true, 0, [], [], null).String;
    [ApiContract(Call = "read")]
    public string GetBlendToState() => Invoke(180, 10, true, 0, [], [], null).String;
    [ApiContract(Call = "read")]
    public float GetBlendToNormalizedTime() => Invoke(180, 11, true, 0, [], [], null).Scalar;
    [ApiContract(Call = "read")]
    public float GetSpeed() => Invoke(180, 12, true, 0, [], [], null).Scalar;
    [ApiContract(Call = "read")]
    public bool IsPlaying() => Invoke(180, 13, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public Vector3 GetRootMotionDeltaPosition() => Invoke(180, 14, true, 0, [], [], null).Vector3;
    [ApiContract(Call = "read")]
    public Quaternion GetRootMotionDeltaRotation() => Invoke(180, 15, true, 0, [], [], null).Quaternion;
    [ApiContract(Call = "read")]
    public Vector3 GetRootMotionWorldDelta() => Invoke(180, 16, true, 0, [], [], null).Vector3;
    [ApiContract(Call = "read")]
    public Vector3 GetRootMotionWorldVelocity() => Invoke(180, 17, true, 0, [], [], null).Vector3;
    [ApiContract(Call = "read")]
    public float GetRootMotionDeltaTime() => Invoke(180, 18, true, 0, [], [], null).Scalar;
    [ApiContract(Call = "read")]
    public bool IsRootMotionAppliedByEngine() => Invoke(180, 19, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public int GetRootMotionMode() => Invoke(180, 20, true, 0, [], [], null).Int32;
    [ApiContract(Call = "read")]
    public float GetRootMotionPositionScale() => Invoke(180, 21, true, 0, [], [], null).Scalar;
    [ApiContract(Call = "read")]
    public float GetRootMotionRotationScale() => Invoke(180, 22, true, 0, [], [], null).Scalar;
    [ApiContract(Call = "read")]
    public string GetRootMotionNodeName() => Invoke(180, 23, true, 0, [], [], null).String;
    public BlendWeight[] GetCurrentBlendWeights()
    {
        NativeServiceResult result = Invoke(180, 24, true, 0, [], [], null);
        if (result.ValueCount != result.Text.Length) throw new InvalidOperationException("Invalid blend weights.");
        BlendWeight[] weights = new BlendWeight[result.ValueCount];
        for (int index = 0; index < weights.Length; index++) weights[index] = new(result.Text[index], result.Value(index));
        return weights;
    }
    /// @note 名前の文字列は返す値として確保する。集合の一時配列を作らず、呼出元の capacity を使う。
    /// @return 不足時は Required と Written=0、output は未変更。成功時は全件を既存の順序で書く。
    [ApiContract(Call = "read", Summary = "Copies every blend weight into caller storage. Transfer buffers are reused after pool warmup; returned nonempty names allocate owned strings. Insufficient capacity reports Required and Written=0 without changing output.", Failure = "insufficient_capacity_returns_required_and_written_zero_without_output_changes")]
    public CollectionReadResult GetCurrentBlendWeights(Span<BlendWeight> output)
    {
        ScriptExecutionScope scope = Scope();
        return scope.Bridge.ReadBlendWeights(scope, GameObject.ObjectId, output);
    }
    public void SetSpeed(float speed) => Invoke(180, 25, false, 0, [speed], [], null);
    public void SetLocalTimeScale(float scale) => Invoke(180, 26, false, 0, [scale], [], null);
    public void Play(string stateName) => Invoke(180, 27, false, 0, [], [stateName], null);
    public void SetPlaying(bool playing) => Invoke(180, 28, false, playing ? 1 : 0, [], [], null);
    public void SetLayerWeight(string layerName, float weight) => Invoke(180, 29, false, 0, [weight], [layerName], null);
    [ApiContract(Call = "read")]
    public float GetLayerWeight(string layerName) => Invoke(180, 30, true, 0, [], [layerName], null).Scalar;
    [ApiContract(Call = "read")]
    public string GetLayerState(string layerName) => Invoke(180, 31, true, 0, [], [layerName], null).String;
    [ApiContract(Call = "read")]
    public bool IsLayerInState(string layerName, string stateName) => Invoke(180, 32, true, 0, [], [layerName, stateName], null).Flag;
    public void PlayLayerState(string layerName, string stateName) => Invoke(180, 33, false, 0, [], [layerName, stateName], null);
    public void SetLayerMask(string layerName, string maskPath) => Invoke(180, 34, false, 0, [], [layerName, maskPath], null);
    [ApiContract(Call = "read")]
    public string GetLayerMask(string layerName) => Invoke(180, 35, true, 0, [], [layerName], null).String;
    public void PlaySlot(string layerName, string sourcePath, string clipName, float fadeIn = 0.15f, float fadeOut = 0.15f, float speed = 1f, bool loop = false) => Invoke(180, 36, false, loop ? 1 : 0, [fadeIn, fadeOut, speed], [layerName, sourcePath, clipName], null);
    public void StopSlot(string layerName, float fadeOut = -1f) => Invoke(180, 37, false, 0, [fadeOut], [layerName], null);
    [ApiContract(Call = "read")]
    public bool IsSlotPlaying(string layerName) => Invoke(180, 38, true, 0, [], [layerName], null).Flag;
    [ApiContract(Call = "read")]
    public float GetSlotWeight(string layerName) => Invoke(180, 39, true, 0, [], [layerName], null).Scalar;
    public void SetSlotSpeed(string layerName, float speed) => Invoke(180, 40, false, 0, [speed], [layerName], null);
    [ApiContract(Call = "read")]
    public float GetSlotTime(string layerName) => Invoke(180, 41, true, 0, [], [layerName], null).Scalar;
    public void SetStateSpeed(string stateName, float speed) => Invoke(180, 42, false, 0, [speed], [stateName], null);
    [ApiContract(Call = "read")]
    public float GetStateSpeed(string stateName) => Invoke(180, 43, true, 0, [], [stateName], null).Scalar;
    public void SetMorphWeight(string morphName, float weight) => Invoke(180, 44, false, 0, [weight], [morphName], null);
    [ApiContract(Call = "read")]
    public float GetMorphWeight(string morphName) => Invoke(180, 45, true, 0, [], [morphName], null).Scalar;
    public void SetRootMotionMode(int mode) => Invoke(180, 46, false, (long)mode, [], [], null);
    public void SetRootMotionPositionScale(float scale) => Invoke(180, 47, false, 0, [scale], [], null);
    public void SetRootMotionRotationScale(float scale) => Invoke(180, 48, false, 0, [scale], [], null);
    public void SetRootMotionNodeName(string nodeName) => Invoke(180, 49, false, 0, [], [nodeName], null);
    /// @note Opt-in pose sampling for independent skeletal animation; clocks and events remain per-frame. Complex procedural rigs use full-rate evaluation.
    [ApiContract(Summary = "Configure pose update LOD using nearest enabled scene camera. Distances are world metres and intervals are seconds. Near poses evaluate every frame; defaults are disabled. Clocks/events continue each frame; root motion, external poses, layers, IK and spring bones use full-rate fallback.", Failure = "invalid_or_nonfinite_arguments_leave_settings_unchanged")]
    public void SetUpdateLod(bool enabled, float nearDistance = 20f, float farDistance = 60f, float midInterval = 1f / 30f, float farInterval = 1f / 15f)
        => Invoke(180, 50, false, enabled ? 1 : 0, [nearDistance, farDistance, midInterval, farInterval], [], null);
}

/// @note Play の同期 callback 内で既存 AudioSource サービスへ転送する。
public sealed partial class AudioSource : NativeComponent
{
    internal AudioSource(GameObject owner, ulong lifetimeToken = 0) : base(owner, 6, lifetimeToken) { }
    public void Play() => Invoke(181, 0, false, 0, [], [], null);
    public void Stop() => Invoke(181, 1, false, 0, [], [], null);
    public void Pause() => Invoke(181, 2, false, 0, [], [], null);
    public void Resume() => Invoke(181, 3, false, 0, [], [], null);
    [ApiContract(Call = "read")]
    public bool IsPlaying() => Invoke(181, 4, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public float GetVolume() => Invoke(181, 5, true, 0, [], [], null).Scalar;
    [ApiContract(Call = "read")]
    public float GetPitch() => Invoke(181, 6, true, 0, [], [], null).Scalar;
    [ApiContract(Call = "read")]
    public bool GetLoop() => Invoke(181, 7, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool IsPaused() => Invoke(181, 8, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public float GetSpatialBlend() => Invoke(181, 9, true, 0, [], [], null).Scalar;
    [ApiContract(Call = "read")]
    public string GetBus() => Invoke(181, 10, true, 0, [], [], null).String;
    public void Play(string clipPath) => Invoke(181, 12, false, 0, [], [clipPath], null);
    public void SetVolume(float volume) => Invoke(181, 14, false, 0, [volume], [], null);
    public void SetLoop(bool loop) => Invoke(181, 15, false, loop ? 1 : 0, [], [], null);
    public void SetPitch(float pitch) => Invoke(181, 16, false, 0, [pitch], [], null);
    public void SetSpatialBlend(float blend) => Invoke(181, 17, false, 0, [blend], [], null);
    public void Set3DDistances(float minDistance, float maxDistance, float rolloff = 1f) => Invoke(181, 18, false, 0, [minDistance, maxDistance, rolloff], [], null);
    public void PlayOneShot(string clipPath, float volumeScale = 1f) => Invoke(181, 19, false, 0, [volumeScale], [clipPath], null);
    public void FadeTo(float gain, float seconds) => Invoke(181, 23, false, 0, [gain, seconds], [], null);
    public void FadeOutAndStop(float seconds) => Invoke(181, 24, false, 0, [seconds], [], null);
    public void SetBus(string bus) => Invoke(181, 25, false, 0, [], [bus], null);
    public void PlayClip(SynthClip clip) => Invoke(181, 35, false, clip.Handle);
    public void PlayClipAsSource(SynthClip clip) => Invoke(181, 36, false, clip.Handle);
    public void PlaySynth(SynthSpec spec) => Invoke(181, 37, false, spec.Packed, spec.Values);
    public void PlaySynth(SynthPreset preset, uint seed = 0)
    { ScriptAudio.CheckPreset(preset); Invoke(181, 38, false, ScriptAudio.Pack(preset, seed)); }
}

/// @note 2D 音・合成音資源・所有 loop を扱う GameObject サービス。主 AudioSource Component を要求しない。
public sealed class ScriptAudio
{
    internal ScriptAudio(GameObject owner) { m_owner = owner; }
    readonly GameObject m_owner;
    NativeServiceResult Invoke(uint service, uint method, bool readOnly, long integer = 0,
        ReadOnlySpan<float> values = default, ReadOnlySpan<string> text = default, GameObject? related = null)
        => OwnerServiceCall.Invoke(m_owner, service, method, readOnly, integer, values, text, related);
    [ApiContract(Call = "read")]
    public bool IsBGMPlaying() => Invoke(181, 11, true, 0, [], [], null).Flag;
    public bool Preload(string clipPath) => Invoke(181, 13, false, 0, [], [clipPath], null).Flag;
    public void PlayAtPoint(string clipPath, Vector3 position, float volume = 1f) => Invoke(181, 20, false, 0, [position.X, position.Y, position.Z, volume], [clipPath], null);
    public void PlayBGM(string clipPath, bool loop = true, float fadeSeconds = 0f) => Invoke(181, 21, false, loop ? 1 : 0, [fadeSeconds], [clipPath], null);
    public void StopBGM(float fadeSeconds = 0f) => Invoke(181, 22, false, 0, [fadeSeconds], [], null);
    public void SetBusVolume(string bus, float volume) => Invoke(181, 26, false, 0, [volume], [bus], null);
    [ApiContract(Call = "read")]
    public float GetBusVolume(string bus) => Invoke(181, 27, true, 0, [], [bus], null).Scalar;
    public void SetBusLowPass(string bus, float normalizedCutoff) => Invoke(181, 28, false, 0, [normalizedCutoff], [bus], null);
    public SynthSpec MakeSpec(SynthPreset preset, uint seed = 0)
    { CheckPreset(preset); return SynthSpec.From(Invoke(181, 29, true, Pack(preset, seed))); }
    public SynthSpec MutateSpec(SynthSpec spec, float amount, uint seed)
    {
        if (!float.IsFinite(amount) || amount < 0f || amount > 1f) throw new ArgumentOutOfRangeException(nameof(amount));
        float[] values = new float[20]; spec.Values.CopyTo(values, 0); values[19] = amount;
        return SynthSpec.From(Invoke(181, 30, true, spec.Packed, values, [seed.ToString(System.Globalization.CultureInfo.InvariantCulture)]));
    }
    public bool LoadSpec(string path, out SynthSpec spec)
    {
        NativeServiceResult result = Invoke(181, 31, true, text: [path]);
        if (result.ValueCount == 0) { spec = default; return false; }
        spec = SynthSpec.From(result); return true;
    }
    public SynthClip Synthesize(SynthSpec spec) => new(Invoke(181, 32, false, spec.Packed, spec.Values).Integer);
    public SynthClip Synthesize(SynthPreset preset, uint seed = 0)
    { CheckPreset(preset); return new(Invoke(181, 33, false, Pack(preset, seed)).Integer); }
    public void ReleaseClip(SynthClip clip) => Invoke(181, 34, false, clip.Handle);
    public void PlaySynth2D(SynthSpec spec, string bus = "UI") => Invoke(181, 39, false, spec.Packed, spec.Values, [bus]);
    public void PlayClip2D(SynthClip clip, string bus = "UI") => Invoke(181, 40, false, clip.Handle, text: [bus]);
    public void PlayClipAtPoint(SynthClip clip, Vector3 position, float volume = 1f) => Invoke(181, 41, false, clip.Handle, [position.X, position.Y, position.Z, volume]);
    public bool UpdateLoop(AudioLoop loop, string path, float volume, float pitch = 1f, AudioLoopSettings settings = default)
    {
        ArgumentNullException.ThrowIfNull(loop);
        AudioLoopSettings effective = settings.Effective;
        NativeServiceResult result = Invoke(181, 42, false, loop.Handle,
            [volume, pitch, effective.SpatialBlend, effective.MinDistance, effective.MaxDistance], [path, effective.Label, effective.Bus]);
        if (!result.Flag) return false;
        if (result.ValueCount != 0 || result.Text.Length != 1 || !long.TryParse(result.Text[0], System.Globalization.NumberStyles.None,
            System.Globalization.CultureInfo.InvariantCulture, out long handle)) throw new InvalidOperationException("Invalid AudioLoop response.");
        loop.Handle = handle; loop.Owner = this; return true;
    }
    public bool StopLoop(AudioLoop loop) { ArgumentNullException.ThrowIfNull(loop); return Invoke(181, 43, false, loop.Handle).Flag; }
    public void ReleaseLoop(AudioLoop loop)
    { ArgumentNullException.ThrowIfNull(loop); Invoke(181, 44, false, loop.Handle); loop.Handle = 0; loop.Owner = null; }
    internal bool LoopIsValid(AudioLoop loop) => Invoke(181, 45, true, loop.Handle).Flag;
    internal static long Pack(SynthPreset preset, uint seed) => unchecked((long)(((ulong)preset << 32) | seed));
    internal static void CheckPreset(SynthPreset preset) { if ((uint)preset > 6) throw new ArgumentOutOfRangeException(nameof(preset)); }
}

/// @note 所有者の widget を各操作で解決するサービス。型付き Component の保持寿命とは区別する。
public sealed class UI
{
    internal UI(GameObject owner) { m_owner = owner; }
    readonly GameObject m_owner;
    NativeServiceResult Invoke(uint service, uint method, bool readOnly, long integer = 0,
        ReadOnlySpan<float> values = default, ReadOnlySpan<string> text = default, GameObject? related = null)
        => OwnerServiceCall.Invoke(m_owner, service, method, readOnly, integer, values, text, related);
    [ApiContract(Call = "read")]
    public bool WasClicked() => Invoke(182, 0, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool WasHoverEnter() => Invoke(182, 1, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool WasHoverExit() => Invoke(182, 2, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool IsHovered() => Invoke(182, 3, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool IsPressed() => Invoke(182, 4, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool IsInteractable() => Invoke(182, 5, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool IsImageEnabled() => Invoke(182, 6, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool IsTextEnabled() => Invoke(182, 7, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool IsPointerOverridden() => Invoke(182, 8, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool IsPointerOverUI() => Invoke(182, 9, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public int GetButtonPhase() => Invoke(182, 10, true, 0, [], [], null).Int32;
    [ApiContract(Call = "read")]
    public Vector2 GetCanvasMousePosition() => Invoke(182, 11, true, 0, [], [], null).Vector2;
    [ApiContract(Call = "read")]
    public float GetImageFillAmount() => Invoke(182, 12, true, 0, [], [], null).Scalar;
    [ApiContract(Call = "read")]
    public Vector4 GetImageColor() => Invoke(182, 13, true, 0, [], [], null).Vector4;
    [ApiContract(Call = "read")]
    public string GetText() => Invoke(182, 14, true, 0, [], [], null).String;
    [ApiContract(Call = "read")]
    public Vector4 GetTextColor() => Invoke(182, 15, true, 0, [], [], null).Vector4;
    [ApiContract(Call = "read")]
    public GameObject? GetFocus() => Invoke(182, 16, true, 0, [], [], null).GameObject;
    public void SetButtonInteractable(bool interactable) => Invoke(182, 17, false, interactable ? 1 : 0, [], [], null);
    public void SetImageColor(Vector4 color) => Invoke(182, 18, false, 0, [color.X, color.Y, color.Z, color.W], [], null);
    public void SetText(string text) => Invoke(182, 19, false, 0, [], [text], null);
    public void SetTextColor(Vector4 color) => Invoke(182, 20, false, 0, [color.X, color.Y, color.Z, color.W], [], null);
    public void SetCanvasSortOrder(int order) => Invoke(182, 21, false, (long)order, [], [], null);
    public void SetPointer(Vector2 position, bool pressed) => Invoke(182, 22, false, pressed ? 1 : 0, [position.X, position.Y], [], null);
    public void ClearPointer() => Invoke(182, 23, false, 0, [], [], null);
    public void SetImageSpriteRect(float x, float y, float width, float height, float textureWidth, float textureHeight) => Invoke(182, 24, false, 0, [x, y, width, height, textureWidth, textureHeight], [], null);
    public void SetImageTexture(string path) => Invoke(182, 25, false, 0, [], [path], null);
    public void SetImageFillAmount(float amount) => Invoke(182, 26, false, 0, [amount], [], null);
    public void SetImageEnabled(bool enabled) => Invoke(182, 27, false, enabled ? 1 : 0, [], [], null);
    public void SetTextEnabled(bool enabled) => Invoke(182, 28, false, enabled ? 1 : 0, [], [], null);
    [ApiContract(Call = "read")]
    public float GetSliderValue() => Invoke(182, 29, true, 0, [], [], null).Scalar;
    public void SetSliderValue(float value) => Invoke(182, 30, false, 0, [value], [], null);
    [ApiContract(Call = "read")]
    public bool WasSliderChanged() => Invoke(182, 31, true, 0, [], [], null).Flag;
    public void SetSliderRange(float minimum, float maximum) => Invoke(182, 32, false, 0, [minimum, maximum], [], null);
    public void SetSliderInteractable(bool interactable) => Invoke(182, 33, false, interactable ? 1 : 0, [], [], null);
    [ApiContract(Call = "read")]
    public bool IsToggleOn() => Invoke(182, 34, true, 0, [], [], null).Flag;
    public void SetToggleOn(bool isOn) => Invoke(182, 35, false, isOn ? 1 : 0, [], [], null);
    [ApiContract(Call = "read")]
    public bool WasToggleChanged() => Invoke(182, 36, true, 0, [], [], null).Flag;
    public void SetToggleInteractable(bool interactable) => Invoke(182, 37, false, interactable ? 1 : 0, [], [], null);
    [ApiContract(Call = "read")]
    public Vector2 GetScrollPosition() => Invoke(182, 38, true, 0, [], [], null).Vector2;
    public void SetScrollPosition(Vector2 position) => Invoke(182, 39, false, 0, [position.X, position.Y], [], null);
    public void SetScrollContentSize(Vector2 size) => Invoke(182, 40, false, 0, [size.X, size.Y], [], null);
    [ApiContract(Call = "read")]
    public bool WasScrollChanged() => Invoke(182, 41, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public string GetFieldText() => Invoke(182, 42, true, 0, [], [], null).String;
    public void SetFieldText(string text) => Invoke(182, 43, false, 0, [], [text], null);
    [ApiContract(Call = "read")]
    public bool WasFieldChanged() => Invoke(182, 44, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool WasSubmitted() => Invoke(182, 45, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool IsFieldFocused() => Invoke(182, 46, true, 0, [], [], null).Flag;
    public void SetFieldFocused(bool focused) => Invoke(182, 47, false, focused ? 1 : 0, [], [], null);
    [ApiContract(Call = "read")]
    public float GetGroupAlpha() => Invoke(182, 48, true, 0, [], [], null).Scalar;
    public void SetGroupAlpha(float alpha) => Invoke(182, 49, false, 0, [alpha], [], null);
    public void SetGroupInteractable(bool interactable) => Invoke(182, 50, false, interactable ? 1 : 0, [], [], null);
    public void SetGroupBlocksRaycasts(bool blocks) => Invoke(182, 51, false, blocks ? 1 : 0, [], [], null);
    public void SetFocus() => Invoke(182, 52, false, 0, [], [], null);
    [ApiContract(Call = "read")]
    public bool IsFocused() => Invoke(182, 53, true, 0, [], [], null).Flag;
    public void ClearFocus() => Invoke(182, 54, false, 0, [], [], null);
    [ApiContract(Call = "read")]
    public bool IsDragging() => Invoke(182, 55, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool WasDropped() => Invoke(182, 56, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public GameObject? GetDropTarget() => Invoke(182, 57, true, 0, [], [], null).GameObject;
    [ApiContract(Call = "read")]
    public bool WasReceived() => Invoke(182, 58, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public GameObject? GetReceivedFrom() => Invoke(182, 59, true, 0, [], [], null).GameObject;
    [ApiContract(Call = "read")]
    public string GetReceivedPayload() => Invoke(182, 60, true, 0, [], [], null).String;
    [ApiContract(Call = "read")]
    public bool IsDropHovered() => Invoke(182, 61, true, 0, [], [], null).Flag;
    public void SetVisibleCharacters(int count) => Invoke(182, 62, false, (long)count, [], [], null);
    [ApiContract(Call = "read")]
    public int GetVisibleCharacters() => Invoke(182, 63, true, 0, [], [], null).Int32;
    [ApiContract(Call = "read")]
    public int GetCharacterCount() => Invoke(182, 64, true, 0, [], [], null).Int32;
    public void SetMaterial(string path) => Invoke(182, 65, false, 0, [], [path], null);
    public void SetMaterialFloat(string parameter, float value) => Invoke(182, 66, false, 0, [value], [parameter], null);
    public void SetMaterialVector2(string parameter, float x, float y) => Invoke(182, 67, false, 0, [x, y], [parameter], null);
    public void SetMaterialVector4(string parameter, Vector4 value) => Invoke(182, 68, false, 0, [value.X, value.Y, value.Z, value.W], [parameter], null);
    public void SetMaterialColor(string parameter, Vector4 color) => Invoke(182, 69, false, 0, [color.X, color.Y, color.Z, color.W], [parameter], null);
    public void SetMaterialTexture(string slot, string texturePath) => Invoke(182, 70, false, 0, [], [slot, texturePath], null);
    public void ClearMaterialOverride(string parameter) => Invoke(182, 71, false, 0, [], [parameter], null);
    public void ClearMaterialOverrides() => Invoke(182, 72, false, 0, [], [], null);
    [ApiContract(Call = "read")]
    public bool HasMaterialOverride(string parameter) => Invoke(182, 73, true, 0, [], [parameter], null).Flag;
    [ApiContract(Call = "read")]
    public GameObject? Find(string childName) => Invoke(182, 74, true, 0, [], [childName], null).GameObject;
    public bool TryGetCanvasSize(out Vector2 size)
    {
        NativeServiceResult result = Invoke(182, 75, true, 0, [], [], null);
        if (!result.Flag) { size = default; return false; }
        size = result.Vector2; return true;
    }
    [ApiContract(Call = "read", Summary = "World/camera-space UI occlusion against opaque scene meshes.")]
    public CanvasDepthMode GetCanvasDepthMode() => (CanvasDepthMode)Invoke(182, 76, true, 0, [], [], null).Integer;
    public void SetCanvasDepthMode(CanvasDepthMode mode) => Invoke(182, 77, false, (long)mode, [], [], null);
    [ApiContract(Call = "read", Units = "m")]
    public float GetCanvasSurfaceOffset() => Invoke(182, 78, true, 0, [], [], null).Scalar;
    [ApiContract(ParameterUnits = new[] { "metres=m" }, Summary = "Offset toward the canvas front (-local Z); valid range -1..1 metres.")]
    public void SetCanvasSurfaceOffset(float metres) => Invoke(182, 79, false, 0, [metres], [], null);
}

/// @note pool や Type を保持せず、現在の callback と所有者寿命を同期検証する。
internal static class OwnerServiceCall
{
    internal static NativeServiceResult Invoke(GameObject owner, uint service, uint method, bool readOnly, long integer,
        ReadOnlySpan<float> values, ReadOnlySpan<string> text, GameObject? related)
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        if (owner.SceneLifetime != scope.SceneLifetime || !scope.Bridge.IsAlive(scope, owner.ObjectId))
            throw new InvalidOperationException("The service owner lifetime has expired.");
        foreach (float value in values) if (!float.IsFinite(value)) throw new ArgumentOutOfRangeException(nameof(values));
        foreach (string value in text) ArgumentNullException.ThrowIfNull(value);
        if (related is not null && related.SceneLifetime != scope.SceneLifetime)
            throw new InvalidOperationException("The reference belongs to another scene.");
        return scope.Bridge.InvokeService(scope, owner.ObjectId, service, method, integer, values, text, related, readOnly);
    }
}
public sealed class Camera : NativeComponent
{
    internal Camera(GameObject owner, ulong lifetimeToken = 0) : base(owner, 8, lifetimeToken) { }
    public void SetAsMain() => Invoke(183, 0, false, 0, [], [], null);
    [ApiContract(ParameterUnits = new[] { "fovY=deg" })]
    public void SetFOV(float fovY) => Invoke(183, 1, false, 0, [fovY], [], null);
    public void SetAspectRatio(float aspectRatio) => Invoke(183, 2, false, 0, [aspectRatio], [], null);
    [ApiContract(ParameterUnits = new[] { "nearZ=m", "farZ=m" }, Summary = "Sets the perspective near and far clipping planes.", Failure = "requires_finite_positive_near_and_far_greater_than_near; invalid_scope_or_component_throws")]
    public void SetNearFar(float nearZ, float farZ) => Invoke(183, 3, false, 0, [nearZ, farZ], [], null);
    public void SetCullingMask(uint mask) => Invoke(183, 4, false, (long)mask, [], [], null);
    public void SetBackgroundColor(Vector4 color) => Invoke(183, 5, false, 0, [color.X, color.Y, color.Z, color.W], [], null);
    public void SetClearMode(CameraClearMode mode) => Invoke(183, 6, false, (long)mode, [], [], null);
    [ApiContract(Units = "xy_px_z_DirectX_depth", ParameterUnits = new[] { "worldPosition=m" }, Coordinate = "top_left_screen_pixels", ParameterCoordinates = new[] { "worldPosition=world" }, Call = "read")]
    public Vector3 WorldToScreenPoint(Vector3 worldPosition) => Invoke(183, 7, true, 0, [worldPosition.X, worldPosition.Y, worldPosition.Z], [], null).Vector3;
    [ApiContract(Units = "m", Coordinate = "world", ParameterCoordinates = new[] { "screenPosition=top_left_screen_pixels_with_DirectX_depth" }, ParameterUnits = new[] { "screenPosition=xy_px_z_DirectX_depth" }, Call = "read")]
    public Vector3 ScreenToWorldPoint(Vector3 screenPosition) => Invoke(183, 8, true, 0, [screenPosition.X, screenPosition.Y, screenPosition.Z], [], null).Vector3;
    [ApiContract(Units = "deg", Coordinate = "not_applicable", Call = "read")]
    public float GetFOV() => Invoke(183, 9, true, 0, [], [], null).Scalar;
    [ApiContract(Units = "m", Coordinate = "not_applicable", Call = "read")]
    public float GetNearZ() => Invoke(183, 10, true, 0, [], [], null).Scalar;
    [ApiContract(Units = "m", Coordinate = "not_applicable", Call = "read")]
    public float GetFarZ() => Invoke(183, 11, true, 0, [], [], null).Scalar;
    [ApiContract(Call = "read")]
    public float GetAspectRatio() => Invoke(183, 12, true, 0, [], [], null).Scalar;
    public uint GetCullingMask() => checked((uint)Invoke(183, 13, true, 0, [], [], null).Integer);
    [ApiContract(Call = "read")]
    public Vector4 GetBackgroundColor() => Invoke(183, 14, true, 0, [], [], null).Vector4;
    public CameraClearMode GetClearMode() => (CameraClearMode)Invoke(183, 15, true, 0, [], [], null).Int32;
    [ApiContract(Call = "read")]
    public bool IsMain() => Invoke(183, 16, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool IsVisible(Vector3 worldPosition) => Invoke(183, 17, true, 0, [worldPosition.X, worldPosition.Y, worldPosition.Z], [], null).Flag;
    public Ray ScreenPointToRay(float x, float y) => Ray.From(Invoke(183, 18, true, 0, [x, y], [], null));
    [ApiContract(ParameterUnits = new[] { "worldPosition=m", "viewport=xy_ratio_z_DirectX_depth" }, ParameterCoordinates = new[] { "worldPosition=world", "viewport=top_left_viewport_uv_and_DirectX_depth" }, Summary = "Converts world position to viewport coordinates.", Returns = "False when the native conversion cannot produce a point; the output is set to zero.", Failure = "false_sets_viewport_to_zero; invalid_scope_lifetime_component_or_nonfinite_argument_throws", Call = "read")]
    public bool TryWorldToViewportPoint(Vector3 worldPosition, out Vector3 viewport)
    {
        NativeServiceResult result = Invoke(183, 19, true, 0, [worldPosition.X, worldPosition.Y, worldPosition.Z], [], null);
        if (!result.Flag) { viewport = default; return false; }
        viewport = result.Vector3; return true;
    }
    [ApiContract(ParameterUnits = new[] { "uv=ratio", "depth=m", "worldPosition=m" }, ParameterCoordinates = new[] { "uv=top_left_viewport_uv", "worldPosition=world" }, Summary = "Unprojects viewport UV at a forward distance in metres, not DirectX depth.", Returns = "False when the native conversion cannot produce a point; the output is set to zero.", Failure = "false_sets_worldPosition_to_zero; invalid_scope_lifetime_component_or_nonfinite_argument_throws", Call = "read")]
    public bool TryViewportToWorldPoint(Vector2 uv, float depth, out Vector3 worldPosition)
    {
        NativeServiceResult result = Invoke(183, 20, true, 0, [uv.X, uv.Y, depth], [], null);
        if (!result.Flag) { worldPosition = default; return false; }
        worldPosition = result.Vector3; return true;
    }
}

/// @note 位置は world m、速度は m/s、角速度は degree/s。Warp は子孫まで即時同期し、実 body がある場合は FixedUpdate のみ。
public sealed class NavMeshAgent : NativeComponent
{
    internal NavMeshAgent(GameObject owner, ulong lifetimeToken = 0) : base(owner, 9, lifetimeToken) { }
    public void SetDestination(Vector3 worldPosition) => Invoke(184, 0, false, 0, [worldPosition.X, worldPosition.Y, worldPosition.Z], [], null);
    public void CancelPath() => Invoke(184, 1, false, 0, [], [], null);
    public void Pause() => Invoke(184, 2, false, 0, [], [], null);
    public void Resume() => Invoke(184, 3, false, 0, [], [], null);
    [ApiContract(ParameterUnits = new[] { "worldPosition=m" }, ParameterCoordinates = new[] { "worldPosition=world" }, Failure = "finite_world_position_required; target_with_enabled_RigidBody_requires_FixedUpdate; bodyless_target_accepts_live_Play_callback")]
    public void Warp(Vector3 worldPosition) => Invoke(184, 4, false, 0, [worldPosition.X, worldPosition.Y, worldPosition.Z], [], null);
    public void SetTarget(GameObject target, float repathInterval = 0.5f) => Invoke(184, 5, false, 0, [repathInterval], [], target);
    public void ClearTarget() => Invoke(184, 6, false, 0, [], [], null);
    [ApiContract(Call = "read")]
    public bool IsFollowingTarget() => Invoke(184, 7, true, 0, [], [], null).Flag;
    public void BakeNavMesh() => Invoke(184, 8, false, 0, [], [], null);
    [ApiContract(Call = "read")]
    public bool IsPaused() => Invoke(184, 9, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool IsMoving() => Invoke(184, 10, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool HasPath() => Invoke(184, 11, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool HasArrived() => Invoke(184, 12, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public bool IsStuck() => Invoke(184, 13, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public float GetRemainingDistance() => Invoke(184, 14, true, 0, [], [], null).Scalar;
    [ApiContract(Units = "m/s", Call = "read")]
    public Vector3 GetVelocity() => Invoke(184, 15, true, 0, [], [], null).Vector3;
    [ApiContract(Units = "not_applicable", ParameterUnits = new[] { "speed=m/s" })]
    public void SetSpeed(float speed) => Invoke(184, 16, false, 0, [speed], [], null);
    [ApiContract(Units = "m/s", Call = "read")]
    public float GetSpeed() => Invoke(184, 17, true, 0, [], [], null).Scalar;
    [ApiContract(ParameterUnits = new[] { "degreesPerSecond=deg/s" })]
    public void SetAngularSpeed(float degreesPerSecond) => Invoke(184, 18, false, 0, [degreesPerSecond], [], null);
    [ApiContract(Units = "deg/s", Call = "read")]
    public float GetAngularSpeed() => Invoke(184, 19, true, 0, [], [], null).Scalar;
    [ApiContract(Call = "read")]
    public Vector3 GetDestination() => Invoke(184, 20, true, 0, [], [], null).Vector3;
    [ApiContract(Call = "read")]
    public float GetStoppingDistance() => Invoke(184, 21, true, 0, [], [], null).Scalar;
    public void SetStoppingDistance(float distance) => Invoke(184, 22, false, 0, [distance], [], null);
    [ApiContract(Call = "read")]
    public int GetAreaMask() => Invoke(184, 23, true, 0, [], [], null).Int32;
    public void SetAreaMask(int mask) => Invoke(184, 24, false, (long)mask, [], [], null);
    [ApiContract(Call = "read")]
    public bool CanSeeTarget() => Invoke(184, 25, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public GameObject? GetDetectedTarget() => Invoke(184, 26, true, 0, [], [], null).GameObject;
}

/// @note 自作剛体を操作する。速度は world m/s、Jump impulse は N s。変更は FixedUpdate、接地登録は物理 callback も許可する。
public sealed class CharacterController : NativeComponent
{
    internal CharacterController(GameObject owner, ulong lifetimeToken = 0) : base(owner, 10, lifetimeToken) { }
    NativeServiceResult Call(uint method, bool readOnly, long integer, float[] values, string[] text, GameObject? related)
    {
        ScriptCallback callback = Scope().Callback;
        bool contact = callback >= ScriptCallback.ON_COLLISION_ENTER && callback <= ScriptCallback.ON_TRIGGER_EXIT;
        if (!readOnly && callback != ScriptCallback.FIXED_UPDATE && !(method == 16 && contact))
            throw new InvalidOperationException("Character changes require FixedUpdate; contact registration also accepts physics callbacks.");
        return Invoke(185, method, readOnly, integer, values, text, related);
    }
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "deltaSeconds=s" }, Summary = "Updates grounding and jump state from the current rigid body velocity; call once at the beginning of FixedUpdate.", Failure = "requires_enabled_RigidBody_and_finite_state; finite_nonnegative_delta_required; failure_leaves_body_and_controller_unchanged")]
    public void Tick(float deltaSeconds) => Call(0, false, 0, [deltaSeconds], [], null);
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "impulse=N*s" }, ParameterCoordinates = new[] { "impulse=world" }, Failure = "requires_enabled_RigidBody_and_finite_state; invalid_input_or_scope_throws_without_partial_changes")]
    public void Jump(Vector3 impulse) => Call(1, false, 0, [impulse.X, impulse.Y, impulse.Z], [], null);
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "verticalSpeed=m/s" }, ParameterCoordinates = new[] { "verticalSpeed=world" }, Failure = "requires_enabled_RigidBody_and_finite_state; invalid_input_or_scope_throws_without_partial_changes")]
    public void JumpAtVelocity(float verticalSpeed) => Call(2, false, 0, [verticalSpeed], [], null);
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "velocity=m/s" }, ParameterCoordinates = new[] { "velocity=world" }, Failure = "requires_enabled_RigidBody_and_finite_state; invalid_input_or_scope_throws_without_partial_changes")]
    public void SetHorizontalVelocity(Vector3 velocity) => Call(3, false, 0, [velocity.X, velocity.Y, velocity.Z], [], null);
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "velocity=m/s" }, ParameterCoordinates = new[] { "velocity=world" }, Failure = "requires_enabled_RigidBody_and_finite_state; invalid_input_or_scope_throws_without_partial_changes")]
    public void AddHorizontalVelocity(Vector3 velocity) => Call(4, false, 0, [velocity.X, velocity.Y, velocity.Z], [], null);
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "desiredVelocity=m/s", "deltaSeconds=s", "acceleration=m/s^2", "deceleration=m/s^2" }, ParameterCoordinates = new[] { "desiredVelocity=world" }, ParameterHelp = new[] { "desiredVelocity=Desired horizontal velocity; the Y component is not a jump command.", "acceleration=Negative values use the component configured acceleration.", "deceleration=Negative values use the component configured deceleration." }, Summary = "Moves horizontal velocity towards the requested velocity using acceleration and deceleration; preserves vertical motion.", Failure = "requires_enabled_RigidBody_and_finite_state; finite_nonnegative_delta_required; failure_leaves_body_and_controller_unchanged")]
    public void Move(Vector3 desiredVelocity, float deltaSeconds, float acceleration = -1f, float deceleration = -1f) => Call(5, false, 0, [desiredVelocity.X, desiredVelocity.Y, desiredVelocity.Z, deltaSeconds, acceleration, deceleration], [], null);
    [ApiContract(Call = "read")]
    public Vector3 GetVelocity() => Call(6, true, 0, [], [], null).Vector3;
    [ApiContract(Call = "read")]
    public Vector3 GetHorizontalVelocity() => Call(7, true, 0, [], [], null).Vector3;
    [ApiContract(Call = "read")]
    public bool IsGrounded() => Call(8, true, 0, [], [], null).Flag;
    [ApiContract(Call = "read")]
    public float GetVerticalSpeed() => Call(9, true, 0, [], [], null).Scalar;
    [ApiContract(Call = "read")]
    public Vector3 GetGroundNormal() => Call(10, true, 0, [], [], null).Vector3;
    [ApiContract(Call = "fixed")]
    public void ForceGrounded(Vector3 normal) => Call(11, false, 0, [normal.X, normal.Y, normal.Z], [], null);
    [ApiContract(Call = "fixed")]
    public void ForceGrounded() => ForceGrounded(Vector3.UP);
    [ApiContract(Call = "fixed")]
    public void ForceAirborne() => Call(12, false, 0, [], [], null);
    [ApiContract(Call = "fixed")]
    public void UseAutomaticGrounding() => Call(13, false, 0, [], [], null);
    [ApiContract(Call = "fixed")]
    public void SetEnabled(bool enabled) => Call(14, false, enabled ? 1 : 0, [], [], null);
    [ApiContract(Call = "read")]
    public bool IsEnabled() => Call(15, true, 0, [], [], null).Flag;
    [ApiContract(Failure = "only_FixedUpdate_or_physics_notification; finite_contact_normal_required", Call = "physics")]
    public void RegisterGroundContact(Collision collision) => Call(16, false, 0, [collision.Normal.X, collision.Normal.Y, collision.Normal.Z], [], null);
}

public enum SynthWave : uint { SINE = 0, SQUARE = 1, SAW = 2, TRIANGLE = 3, NOISE = 4 }
public enum SynthPreset : uint { PICKUP = 0, LASER = 1, EXPLOSION = 2, POWER_UP = 3, HIT = 4, JUMP = 5, BLIP = 6 }
/// @note 純粋な効果音パラメーター。秒・Hz・波形値域は既存自作 Synth と同じ。保存 codec は対象外。
public struct SynthSpec
{
    public SynthSpec() { Wave = SynthWave.SQUARE; Attack = 0f; Sustain = 0.1f; Decay = 0.2f; Punch = 0f; StartFrequency = 440f; MinFrequency = 0f; Slide = 0f; VibratoDepth = 0f; VibratoRate = 0f; DutyCycle = 0.5f; DutySweep = 0f; LowPassCutoff = 1f; LowPassSweep = 0f; HighPassCutoff = 0f; BitCrush = 0f; RepeatRate = 0f; ArpeggioMod = 0f; Amplitude = 0.5f; Seed = 0; SampleRate = 0; }
    public SynthWave Wave;
    public float Attack;
    public float Sustain;
    public float Decay;
    public float Punch;
    public float StartFrequency;
    public float MinFrequency;
    public float Slide;
    public float VibratoDepth;
    public float VibratoRate;
    public float DutyCycle;
    public float DutySweep;
    public float LowPassCutoff;
    public float LowPassSweep;
    public float HighPassCutoff;
    public float BitCrush;
    public float RepeatRate;
    public float ArpeggioMod;
    public float Amplitude;
    public uint Seed;
    public uint SampleRate;
    internal long Packed => unchecked((long)(((ulong)SampleRate << 32) | Seed));
    internal float[] Values => [(float)Wave, Attack, Sustain, Decay, Punch, StartFrequency, MinFrequency, Slide, VibratoDepth, VibratoRate, DutyCycle, DutySweep, LowPassCutoff, LowPassSweep, HighPassCutoff, BitCrush, RepeatRate, ArpeggioMod, Amplitude];
    internal static SynthSpec From(NativeServiceResult result)
    {
        if (result.ValueCount != 19 || result.Value(0) < 0f || result.Value(0) > 4f || MathF.Truncate(result.Value(0)) != result.Value(0))
            throw new InvalidOperationException("Invalid SynthSpec response.");
        ulong packed = unchecked((ulong)result.Integer);
        return new() { Wave = (SynthWave)result.Value(0), Attack = result.Value(1), Sustain = result.Value(2), Decay = result.Value(3), Punch = result.Value(4), StartFrequency = result.Value(5), MinFrequency = result.Value(6), Slide = result.Value(7), VibratoDepth = result.Value(8), VibratoRate = result.Value(9), DutyCycle = result.Value(10), DutySweep = result.Value(11), LowPassCutoff = result.Value(12), LowPassSweep = result.Value(13), HighPassCutoff = result.Value(14), BitCrush = result.Value(15), RepeatRate = result.Value(16), ArpeggioMod = result.Value(17), Amplitude = result.Value(18), Seed = (uint)packed, SampleRate = (uint)(packed >> 32) };
    }
}
/// @note ハンドル非零は生成成功を示す。操作時は Scene・Script 世代・所有者を再照合し、Release/Play 終了後は再利用しない。
public readonly struct SynthClip
{
    internal SynthClip(long handle) { Handle = handle; }
    internal long Handle { get; }
    public bool IsValid => Handle != 0;
}
/// @note 所属 Script 専用の独立ループ。無効化・破棄・リロードで失効する。保存はしない。
public sealed class AudioLoop
{
    internal long Handle;
    internal ScriptAudio? Owner;
    public bool IsValid => Owner is not null && Owner.LoopIsValid(this);
}
public readonly struct AudioLoopSettings
{
    public AudioLoopSettings(string label = "Loop", string bus = "SE", float spatialBlend = 1f, float minDistance = 2f, float maxDistance = 22f)
    { Label = label; Bus = bus; SpatialBlend = spatialBlend; MinDistance = minDistance; MaxDistance = maxDistance; m_initialized = true; }
    readonly bool m_initialized;
    public string Label { get; }
    public string Bus { get; }
    public float SpatialBlend { get; }
    public float MinDistance { get; }
    public float MaxDistance { get; }
    internal AudioLoopSettings Effective => m_initialized ? this : new("Loop", "SE", 1f, 2f, 22f);
}

public sealed class UIText : NativeComponent
{
    internal UIText(GameObject owner, ulong lifetimeToken = 0) : base(owner, 11, lifetimeToken) { }
    [ApiContract(Call = "read")]
    public bool IsTextEnabled() => WidgetUI().IsTextEnabled();
    [ApiContract(Call = "read")]
    public string GetText() => WidgetUI().GetText();
    [ApiContract(Call = "read")]
    public Vector4 GetTextColor() => WidgetUI().GetTextColor();
    public void SetText(string text) => WidgetUI().SetText(text);
    public void SetTextColor(Vector4 color) => WidgetUI().SetTextColor(color);
    public void SetTextEnabled(bool enabled) => WidgetUI().SetTextEnabled(enabled);
    public void SetVisibleCharacters(int count) => WidgetUI().SetVisibleCharacters(count);
    [ApiContract(Call = "read")]
    public int GetVisibleCharacters() => WidgetUI().GetVisibleCharacters();
    [ApiContract(Call = "read")]
    public int GetCharacterCount() => WidgetUI().GetCharacterCount();
}
public sealed class UIImage : NativeComponent
{
    internal UIImage(GameObject owner, ulong lifetimeToken = 0) : base(owner, 12, lifetimeToken) { }
    [ApiContract(Call = "read")]
    public bool IsImageEnabled() => WidgetUI().IsImageEnabled();
    [ApiContract(Call = "read")]
    public float GetImageFillAmount() => WidgetUI().GetImageFillAmount();
    [ApiContract(Call = "read")]
    public Vector4 GetImageColor() => WidgetUI().GetImageColor();
    public void SetImageColor(Vector4 color) => WidgetUI().SetImageColor(color);
    public void SetImageSpriteRect(float x, float y, float width, float height, float textureWidth, float textureHeight) => WidgetUI().SetImageSpriteRect(x, y, width, height, textureWidth, textureHeight);
    public void SetImageTexture(string path) => WidgetUI().SetImageTexture(path);
    public void SetImageFillAmount(float amount) => WidgetUI().SetImageFillAmount(amount);
    public void SetImageEnabled(bool enabled) => WidgetUI().SetImageEnabled(enabled);
}
public sealed class UIButton : NativeComponent
{
    internal UIButton(GameObject owner, ulong lifetimeToken = 0) : base(owner, 13, lifetimeToken) { }
    [ApiContract(Call = "read")]
    public bool WasClicked() => WidgetUI().WasClicked();
    [ApiContract(Call = "read")]
    public bool WasHoverEnter() => WidgetUI().WasHoverEnter();
    [ApiContract(Call = "read")]
    public bool WasHoverExit() => WidgetUI().WasHoverExit();
    [ApiContract(Call = "read")]
    public bool IsHovered() => WidgetUI().IsHovered();
    [ApiContract(Call = "read")]
    public bool IsPressed() => WidgetUI().IsPressed();
    [ApiContract(Call = "read")]
    public bool IsInteractable() => WidgetUI().IsInteractable();
    [ApiContract(Call = "read")]
    public int GetButtonPhase() => WidgetUI().GetButtonPhase();
    public void SetButtonInteractable(bool interactable) => WidgetUI().SetButtonInteractable(interactable);
    public void SetFocus() => WidgetUI().SetFocus();
    [ApiContract(Call = "read")]
    public bool IsFocused() => WidgetUI().IsFocused();
    public void ClearFocus() => WidgetUI().ClearFocus();
}
public enum CanvasDepthMode { SceneDepth = 0, AlwaysOnTop = 1 }

public sealed class Canvas : NativeComponent
{
    internal Canvas(GameObject owner, ulong lifetimeToken = 0) : base(owner, 14, lifetimeToken) { }
    [ApiContract(Call = "read")]
    public Vector2 GetCanvasMousePosition() => WidgetUI().GetCanvasMousePosition();
    public void SetCanvasSortOrder(int order) => WidgetUI().SetCanvasSortOrder(order);
    public bool TryGetCanvasSize(out Vector2 size) => WidgetUI().TryGetCanvasSize(out size);
    [ApiContract(Call = "read")]
    public CanvasDepthMode GetDepthMode() => WidgetUI().GetCanvasDepthMode();
    public void SetDepthMode(CanvasDepthMode mode) => WidgetUI().SetCanvasDepthMode(mode);
    [ApiContract(Call = "read", Units = "m")]
    public float GetSurfaceOffset() => WidgetUI().GetCanvasSurfaceOffset();
    [ApiContract(ParameterUnits = new[] { "metres=m" }, Summary = "Metres toward the canvas front (-local Z), -1..1.")]
    public void SetSurfaceOffset(float metres) => WidgetUI().SetCanvasSurfaceOffset(metres);
}
public sealed class Slider : NativeComponent
{
    internal Slider(GameObject owner, ulong lifetimeToken = 0) : base(owner, 20, lifetimeToken) { }
    [ApiContract(Call = "read")]
    public float GetSliderValue() => WidgetUI().GetSliderValue();
    public void SetSliderValue(float value) => WidgetUI().SetSliderValue(value);
    [ApiContract(Call = "read")]
    public bool WasSliderChanged() => WidgetUI().WasSliderChanged();
    public void SetSliderRange(float minimum, float maximum) => WidgetUI().SetSliderRange(minimum, maximum);
    public void SetSliderInteractable(bool interactable) => WidgetUI().SetSliderInteractable(interactable);
}
public sealed class Toggle : NativeComponent
{
    internal Toggle(GameObject owner, ulong lifetimeToken = 0) : base(owner, 21, lifetimeToken) { }
    [ApiContract(Call = "read")]
    public bool IsToggleOn() => WidgetUI().IsToggleOn();
    public void SetToggleOn(bool isOn) => WidgetUI().SetToggleOn(isOn);
    [ApiContract(Call = "read")]
    public bool WasToggleChanged() => WidgetUI().WasToggleChanged();
    public void SetToggleInteractable(bool interactable) => WidgetUI().SetToggleInteractable(interactable);
}
public sealed class ScrollView : NativeComponent
{
    internal ScrollView(GameObject owner, ulong lifetimeToken = 0) : base(owner, 22, lifetimeToken) { }
    [ApiContract(Call = "read")]
    public Vector2 GetScrollPosition() => WidgetUI().GetScrollPosition();
    public void SetScrollPosition(Vector2 position) => WidgetUI().SetScrollPosition(position);
    public void SetScrollContentSize(Vector2 size) => WidgetUI().SetScrollContentSize(size);
    [ApiContract(Call = "read")]
    public bool WasScrollChanged() => WidgetUI().WasScrollChanged();
}
public sealed class TextField : NativeComponent
{
    internal TextField(GameObject owner, ulong lifetimeToken = 0) : base(owner, 23, lifetimeToken) { }
    [ApiContract(Call = "read")]
    public string GetFieldText() => WidgetUI().GetFieldText();
    public void SetFieldText(string text) => WidgetUI().SetFieldText(text);
    [ApiContract(Call = "read")]
    public bool WasFieldChanged() => WidgetUI().WasFieldChanged();
    [ApiContract(Call = "read")]
    public bool WasSubmitted() => WidgetUI().WasSubmitted();
    [ApiContract(Call = "read")]
    public bool IsFieldFocused() => WidgetUI().IsFieldFocused();
    public void SetFieldFocused(bool focused) => WidgetUI().SetFieldFocused(focused);
}
public sealed class CanvasGroup : NativeComponent
{
    internal CanvasGroup(GameObject owner, ulong lifetimeToken = 0) : base(owner, 24, lifetimeToken) { }
    [ApiContract(Call = "read")]
    public float GetGroupAlpha() => WidgetUI().GetGroupAlpha();
    public void SetGroupAlpha(float alpha) => WidgetUI().SetGroupAlpha(alpha);
    public void SetGroupInteractable(bool interactable) => WidgetUI().SetGroupInteractable(interactable);
    public void SetGroupBlocksRaycasts(bool blocks) => WidgetUI().SetGroupBlocksRaycasts(blocks);
}
public sealed class Draggable : NativeComponent
{
    internal Draggable(GameObject owner, ulong lifetimeToken = 0) : base(owner, 25, lifetimeToken) { }
    [ApiContract(Call = "read")]
    public bool IsDragging() => WidgetUI().IsDragging();
    [ApiContract(Call = "read")]
    public bool WasDropped() => WidgetUI().WasDropped();
    [ApiContract(Call = "read")]
    public GameObject? GetDropTarget() => WidgetUI().GetDropTarget();
}
public sealed class DropTarget : NativeComponent
{
    internal DropTarget(GameObject owner, ulong lifetimeToken = 0) : base(owner, 26, lifetimeToken) { }
    [ApiContract(Call = "read")]
    public bool WasReceived() => WidgetUI().WasReceived();
    [ApiContract(Call = "read")]
    public GameObject? GetReceivedFrom() => WidgetUI().GetReceivedFrom();
    [ApiContract(Call = "read")]
    public string GetReceivedPayload() => WidgetUI().GetReceivedPayload();
    [ApiContract(Call = "read")]
    public bool IsDropHovered() => WidgetUI().IsDropHovered();
}
