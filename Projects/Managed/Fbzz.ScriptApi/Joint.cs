/// @file    Joint.cs
/// @brief   FixedUpdate 操作と実 Joint row birth を持つ型付き SDK。
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
namespace Fbzz;
public enum JointType { FIXED = 0, DISTANCE = 1, ROPE = 2, SPRING = 3, HINGE = 4, SLIDER = 5, CHAIN = 6 }
/// @note 接続先は GameObject recipe。保持済み RigidBody facade の lifetime を自動で復活させない。
public sealed class Joint : NativeComponent
{
    internal Joint(GameObject owner, ulong token = 0) : base(owner, 34, token) { }
    NativeServiceResult Call(uint index, bool read, ReadOnlySpan<float> values = default, GameObject? target = null)
    {
        if (!read && ScriptApiRuntime.RequireScope().Callback != ScriptCallback.FIXED_UPDATE)
            throw new InvalidOperationException("Joint mutations require FixedUpdate in Play.");
        return Invoke(200, index, read, values: values, related: target);
    }
    [ApiContract(Read = "query", RunModes = new[] { "Play" })]
    public bool IsConnected => Call(0, true).Flag;
    [ApiContract(Read = "query", Write = "fixed", RunModes = new[] { "Play" })]
    public bool Enabled { get => Call(1, true).Flag; set => Call(2, false, [value ? 1 : 0]); }
    [ApiContract(Read = "query", RunModes = new[] { "Play" })]
    public JointType Type => (JointType)Call(3, true).Int32;
    [ApiContract(Read = "query", RunModes = new[] { "Play" })]
    public GameObject? ConnectedObject => Call(4, true).GameObject;
    [ApiContract(Read = "query", Units = "m", RunModes = new[] { "Play" })]
    public float Distance => Call(5, true).Scalar;
    [ApiContract(Read = "query", Units = "m", RunModes = new[] { "Play" })]
    public float CurrentDistance => Call(6, true).Scalar;
    [ApiContract(Call = "fixed", RunModes = new[] { "Play" }, Failure = "false_and_row_unchanged_for_missing_pending_self_or_invalid_target; invalid_scope_throws")]
    public bool TryConnectFixed(GameObject target) { ArgumentNullException.ThrowIfNull(target); return Call(7, false, target: target).Flag; }
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "distance=m" }, RunModes = new[] { "Play" })]
    public bool TryConnectDistance(GameObject target, float distance = 0) { ArgumentNullException.ThrowIfNull(target); return Call(8, false, [distance], target).Flag; }
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "maxLength=m" }, RunModes = new[] { "Play" })]
    public bool TryConnectRope(GameObject target, float maxLength = 0) { ArgumentNullException.ThrowIfNull(target); return Call(9, false, [maxLength], target).Flag; }
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "restLength=m", "stiffness=N/m", "damping=native_damping_coefficient" }, RunModes = new[] { "Play" })]
    public bool TryConnectSpring(GameObject target, float restLength, float stiffness, float damping) {
        ArgumentNullException.ThrowIfNull(target);
        if (stiffness < 0 || damping < 0) throw new ArgumentOutOfRangeException(nameof(stiffness));
        return Call(10, false, [restLength, stiffness, damping], target).Flag;
    }
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "worldAxis=direction", "localAnchor=m", "connectedLocalAnchor=m" }, ParameterCoordinates = new[] { "worldAxis=world", "localAnchor=owner_local", "connectedLocalAnchor=connected_local" }, RunModes = new[] { "Play" })]
    public bool TryConnectHinge(GameObject target, Vector3 worldAxis, Vector3 localAnchor = default, Vector3 connectedLocalAnchor = default) {
        ArgumentNullException.ThrowIfNull(target); Axis(worldAxis);
        return Call(11, false, [worldAxis.X, worldAxis.Y, worldAxis.Z, localAnchor.X, localAnchor.Y, localAnchor.Z,
            connectedLocalAnchor.X, connectedLocalAnchor.Y, connectedLocalAnchor.Z], target).Flag;
    }
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "worldAxis=direction" }, ParameterCoordinates = new[] { "worldAxis=world" }, RunModes = new[] { "Play" })]
    public bool TryConnectSlider(GameObject target, Vector3 worldAxis) {
        ArgumentNullException.ThrowIfNull(target); Axis(worldAxis); return Call(12, false, [worldAxis.X, worldAxis.Y, worldAxis.Z], target).Flag;
    }
    /// @note 全 targets と候補 row を検証してから一度だけ変更する。内部 packet 境界は chain 上限ではない。
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "segmentLength=m" }, RunModes = new[] { "Play" }, Failure = "false_and_row_unchanged_on_any_invalid_or_duplicate_target; whole_chain_preflight_before_publication; invalid_scope_throws")]
    public bool TryConnectChain(ReadOnlySpan<GameObject> targets, float segmentLength = 0, int iterations = 4) {
        ScriptExecutionScope scope = Scope();
        if (scope.Callback != ScriptCallback.FIXED_UPDATE) throw new InvalidOperationException("Joint mutations require FixedUpdate.");
        if (!float.IsFinite(segmentLength) || iterations < 1) throw new ArgumentOutOfRangeException(nameof(iterations));
        return scope.Bridge.ConnectJointChain(scope, GameObject.ObjectId, targets, segmentLength, iterations);
    }
    [ApiContract(Call = "fixed", RunModes = new[] { "Play" }, Summary = "Marks the Joint absent immediately and disables physical registration before deferred row cleanup. An explicit later lookup obtains a new birth after re-addition.")]
    public void Break() => Call(13, false);
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "lower=degree_for_hinge_m_for_slider", "upper=degree_for_hinge_m_for_slider" }, RunModes = new[] { "Play" })]
    public void SetLimits(float lower, float upper) => Call(14, false, [lower, upper]);
    [ApiContract(Call = "fixed", RunModes = new[] { "Play" })]
    public void ClearLimits() => Call(15, false);
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "targetDegreesPerSecond=degree/s", "maxTorque=native_motor_limit" }, RunModes = new[] { "Play" })]
    public void SetMotor(float targetDegreesPerSecond, float maxTorque) {
        if (maxTorque < 0) throw new ArgumentOutOfRangeException(nameof(maxTorque)); Call(16, false, [targetDegreesPerSecond, maxTorque]);
    }
    [ApiContract(Call = "fixed", RunModes = new[] { "Play" })]
    public void ClearMotor() => Call(17, false);
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "value=m" }, RunModes = new[] { "Play" })]
    public void SetDistance(float value) => Call(18, false, [value]);
    static void Axis(Vector3 value) {
        if (!value.IsFinite || !float.IsFinite(value.LengthSquared) || value.LengthSquared <= 0) throw new ArgumentOutOfRangeException(nameof(value));
    }
}
