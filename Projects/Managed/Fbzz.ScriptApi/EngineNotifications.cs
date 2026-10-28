/// @file    EngineNotifications.cs
/// @brief   Animation・root motion・Sequence の同期通知用の所有値。
/// @author  Hasegawa Jin
/// @date    2026-10-06
namespace Fbzz;

/// @note native AnimationClip の Event Track を跨いだ合図。文字列と値は callback 後も保持できる。
public readonly struct AnimationEvent
{
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any")]
    public AnimationEvent(string name, int intParam, float floatParam, float clipTime)
    { Name = name; IntParam = intParam; FloatParam = floatParam; ClipTime = clipTime; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public string Name { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public int IntParam { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public float FloatParam { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "s")]
    public float ClipTime { get; }
}

/// @note Animator の抽出直後の差分。ExtractOnly の適用は作者が行い、物理変更は次の FixedUpdate に渡す。
public readonly struct RootMotionInfo
{
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any")]
    public RootMotionInfo(Vector3 deltaPosition, Quaternion deltaRotation, Vector3 worldDeltaPosition,
        Vector3 worldVelocity, float deltaSeconds, bool appliedByEngine)
    { DeltaPosition = deltaPosition; DeltaRotation = deltaRotation; WorldDeltaPosition = worldDeltaPosition;
        WorldVelocity = worldVelocity; DeltaSeconds = deltaSeconds; AppliedByEngine = appliedByEngine; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "m", Coordinate = "local")]
    public Vector3 DeltaPosition { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Coordinate = "local", Units = "quaternion_xyzw")]
    public Quaternion DeltaRotation { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "m", Coordinate = "world")]
    public Vector3 WorldDeltaPosition { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "m/s", Coordinate = "world")]
    public Vector3 WorldVelocity { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Units = "s")]
    public float DeltaSeconds { get; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public bool AppliedByEngine { get; }
}

/// @note binding 付き EventTrack は OnSequenceEvent、binding 無しは Events.Subscribe&lt;SequenceEvent&gt; へ同期配送する。
/// @note public field は通常の型付き Events の保存 codec と共有する所有値。書き換えても Engine の key は変わらない。
[System.Serializable]
public struct SequenceEvent
{
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any")]
    public SequenceEvent(string name, int intParam, float floatParam, float sequenceTime, string sequenceName)
    { Name = name; IntParam = intParam; FloatParam = floatParam; SequenceTime = sequenceTime; SequenceName = sequenceName; }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Write = "any")]
    public string Name;
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Write = "any")]
    public int IntParam;
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Write = "any")]
    public float FloatParam;
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Write = "any", Units = "s")]
    public float SequenceTime;
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Write = "any")]
    public string SequenceName;
}
