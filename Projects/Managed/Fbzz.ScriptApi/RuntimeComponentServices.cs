/// @file    RuntimeComponentServices.cs
/// @brief   アニメーション・UI・経路・カメラ・音声・流速場の型付き Component facade。
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
using System.Globalization;
using System.Text;
namespace Fbzz;

public enum RagdollProfile { MECH = 0, HUMANOID = 1 }
public enum UIEasing { LINEAR = 0, EASE_IN = 1, EASE_OUT = 2, EASE_IN_OUT = 3 }
public enum UITweenChannel { COLOR = 0, POSITION = 1, SCALE = 2, ROTATION = 3, FILL = 4, MATERIAL = 5 }
public enum PatrolMode { LOOP = 0, PING_PONG = 1 }
public enum SplineWrapMode { ONCE = 0, LOOP = 1, PING_PONG = 2 }
public enum FlowFieldType { UNIFORM = 0, SINK = 1, SOURCE = 2, VORTEX = 3, CURL = 4, LEGACY_DRAG = 5, BAKED = 6 }
public enum MotionWarpPointMode { CAPTURED = 0, LIVE = 1 }

internal static class RuntimeComponentValue
{
    internal static T ReadEnum<T>(NativeServiceResult result) where T : struct, Enum
    {
        T value = (T)Enum.ToObject(typeof(T), result.Int32);
        return Enum.IsDefined(value) ? value : throw new InvalidOperationException("Invalid native Component enum response.");
    }
    internal static int EnumValue<T>(T value) where T : struct, Enum
    {
        if (!Enum.IsDefined(value)) throw new ArgumentOutOfRangeException(nameof(value));
        return Convert.ToInt32(value, CultureInfo.InvariantCulture);
    }
}

public sealed class MotionWarp : NativeComponent
{
    internal MotionWarp(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.MOTION_WARP, lifetimeToken) { }
    const uint Service = (uint)ScriptServiceKind.SERVICE_MOTION_WARP;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public bool Enabled { get => Invoke(Service, 0, true).Flag; set => Invoke(Service, 1, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public bool IsWarping => Invoke(Service, 2, true).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "s")]
    public float RemainingTime => Invoke(Service, 3, true).Scalar;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "m")]
    public float RemainingDistance => Invoke(Service, 4, true).Scalar;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "position=m", "duration=s" }, ParameterCoordinates = new[] { "position=world" }, Failure = "nonpositive_duration_or_nonfinite_input_throws; expired_row_or_scope_throws")]
    public void WarpTo(Vector3 position, float duration) => Invoke(Service, 5, false, values: [position.X, position.Y, position.Z, duration]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "position=m", "duration=s" }, ParameterCoordinates = new[] { "position=world", "rotation=world" }, Summary = "Starts position and rotation correction using a finite nondegenerate normalized pose.")]
    public void WarpToPose(Vector3 position, Quaternion rotation, float duration) => Invoke(Service, 6, false, values: [position.X, position.Y, position.Z, rotation.X, rotation.Y, rotation.Z, rotation.W, duration]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "weight=0_to_1" }, ParameterCoordinates = new[] { "weight=world_axes" })]
    public void SetAxisWeight(Vector3 weight) => Invoke(Service, 7, false, values: [weight.X, weight.Y, weight.Z]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "metersPerSecond=m/s" }, Summary = "Sets a nonnegative correction speed limit; zero is unlimited.")]
    public void SetMaxSpeed(float metersPerSecond) => Invoke(Service, 8, false, values: [metersPerSecond]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void Cancel() => Invoke(Service, 9, false);
    /// @note 通常の数値WarpToはこの接続を保持し、進行中warpの入力だけを数値へ切り替える。
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Lifetime = "exact_current_scene_entity", Summary = "Configures a TargetPoint without starting or resetting a warp. Null explicitly clears the reference. Live targets must be outside the owner's hierarchy.", Failure = "expired_foreign_scene_nonmarker_invalid_mode_or_live_descendant_throws_without_changes")]
    public void SetTargetPoint(GameObject? target, MotionWarpPointMode mode = MotionWarpPointMode.CAPTURED)
        => Invoke(Service, 10, false, RuntimeComponentValue.EnumValue(mode), related: target);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "duration=s" }, Summary = "Starts a warp from the configured point's world pose captured during the synchronous callback. Live refresh updates pose without restarting remaining time.", Failure = "missing_inactive_or_invalid_target_returns_false_without_changing_active_warp; nonpositive_duration_nonfinite_input_or_scope_throws")]
    public bool WarpToTarget(float duration) => Invoke(Service, 11, false, values: [duration]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Lifetime = "exact_current_scene_entity", Failure = "null_for_unconfigured_missing_or_nonmarker_reference")]
    public GameObject? TargetPoint => Invoke(Service, 12, true).GameObject;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public MotionWarpPointMode TargetPointMode => RuntimeComponentValue.ReadEnum<MotionWarpPointMode>(Invoke(Service, 13, true));
}

public sealed partial class IKSolver : NativeComponent
{
    internal IKSolver(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.IK_SOLVER, lifetimeToken) { }
    const uint Service = (uint)ScriptServiceKind.SERVICE_IK;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public bool Enabled { get => Invoke(Service, 0, true).Flag; set => Invoke(Service, 1, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query")]
    public bool HasChain(string targetName) => Invoke(Service, 2, true, text: [targetName]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Failure = "missing_chain_returns_false")]
    public bool IsChainEnabled(string targetName) => Invoke(Service, 3, true, text: [targetName]).Flag;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Failure = "missing_chain_is_noop; empty_name_or_scope_throws")]
    public void SetChainEnabled(string targetName, bool enabled) => Invoke(Service, 4, false, enabled ? 1 : 0, text: [targetName]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Units = "0_to_1", Failure = "missing_chain_returns_zero")]
    public float GetChainWeight(string targetName) => Invoke(Service, 5, true, text: [targetName]).Scalar;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Summary = "Sets the named chain weight using native zero-to-one clamping.")]
    public void SetChainWeight(string targetName, float weight) => Invoke(Service, 6, false, values: [weight], text: [targetName]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void SetAllEnabled(bool enabled) => Invoke(Service, 7, false, enabled ? 1 : 0);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Lifetime = "exact_current_scene_entity", Summary = "Replaces a named chain's persistent target reference with a live current Scene object, including normal objects or bones. Null explicitly clears it; the chain label and world target offset are retained.", Failure = "missing_chain_is_noop; expired_foreign_scene_or_scope_throws_without_changes")]
    public void SetChainTarget(string targetName, GameObject? target) => Invoke(Service, 8, false, text: [targetName], related: target);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Lifetime = "exact_current_scene_entity", Summary = "Replaces the pole reference of the chain selected by its unchanged targetName label. Null explicitly clears it; normal objects and bones are allowed.", Failure = "missing_chain_is_noop; expired_foreign_scene_or_scope_throws_without_changes")]
    public void SetChainPole(string targetName, GameObject? target) => Invoke(Service, 9, false, text: [targetName], related: target);
}

public enum RagdollDriveSpace { Parent = 0, World = 1 }

/// @brief Hz・減衰比・N・N·m。周波数または出力0は該当する駆動を停止する。
public struct RagdollDriveSettings
{
    public bool Enabled;
    public RagdollDriveSpace Space;
    public float LinearFrequency;
    public float AngularFrequency;
    public float LinearDampingRatio;
    public float AngularDampingRatio;
    public float MaxForce;
    public float MaxTorque;
    public float TargetVelocityMultiplier;
    public RagdollDriveSettings()
    {
        Enabled = true; Space = RagdollDriveSpace.Parent;
        LinearFrequency = 0; AngularFrequency = 4;
        LinearDampingRatio = 1; AngularDampingRatio = 1;
        MaxForce = 0; MaxTorque = 100; TargetVelocityMultiplier = 1;
    }
}

public sealed partial class Ragdoll : NativeComponent
{
    internal Ragdoll(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.RAGDOLL, lifetimeToken) { }
    const uint Service = (uint)ScriptServiceKind.SERVICE_RAGDOLL;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "fixed", WriteRunModes = new[] { "Play" })]
    public bool Enabled { get => Invoke(Service, 0, true).Flag; set => Invoke(Service, 1, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public bool IsActive => Invoke(Service, 2, true).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public bool IsStanding => Invoke(Service, 3, true).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "0_to_1")]
    public float Weight => Invoke(Service, 4, true).Scalar;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "m")]
    public float Deviation => Invoke(Service, 5, true).Scalar;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public string Status => Invoke(Service, 6, true).String;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "count")]
    public int ParticleCount => Invoke(Service, 7, true).Int32;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "count")]
    public int SaturatedJointCount => Invoke(Service, 8, true).Int32;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "count")]
    public int LimitedJointCount => Invoke(Service, 9, true).Int32;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "count")]
    public int ContactCount => Invoke(Service, 10, true).Int32;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "count")]
    public int UnmatchedBoneCount => Invoke(Service, 11, true).Int32;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "count")]
    public int RootCount => Invoke(Service, 12, true).Int32;
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", ParameterUnits = new[] { "holdSeconds=s", "maxWeight=0_to_1", "gravityScale=dimensionless" }, Summary = "Captures the current pose and starts passive ragdoll; zero hold continues until End.")]
    public void Begin(float holdSeconds = 0, float maxWeight = 1, float gravityScale = 1) => Invoke(Service, 13, false, values: [holdSeconds, maxWeight, gravityScale]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", ParameterUnits = new[] { "holdSeconds=s" })]
    public void BeginActive(float holdSeconds = 0) => Invoke(Service, 14, false, values: [holdSeconds]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed")]
    public void End() => Invoke(Service, 15, false);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", ParameterUnits = new[] { "velocity=m/s" }, ParameterCoordinates = new[] { "velocity=world" })]
    public void Push(Vector3 velocity) => Invoke(Service, 16, false, values: [velocity.X, velocity.Y, velocity.Z]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", ParameterUnits = new[] { "origin=m", "velocity=m/s", "radius=m" }, ParameterCoordinates = new[] { "origin=world", "velocity=world" })]
    public void PushAt(Vector3 origin, Vector3 velocity, float radius) => Invoke(Service, 17, false, values: [origin.X, origin.Y, origin.Z, velocity.X, velocity.Y, velocity.Z, radius]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", Summary = "Selects a single root and nonnegative maximum depth; an empty name selects the skeleton root.")]
    public void SetRoot(string rootBoneName, int maxDepth = 0) => Invoke(Service, 18, false, maxDepth, text: [rootBoneName]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", Summary = "Validates and stages every root, then replaces the recipe once. Empty input restores the skeleton root; transfer packets do not limit total roots or name length.", Failure = "invalid_names_depth_or_transfer_failure_preserves_previous_recipe")]
    public void SetRoots(ReadOnlySpan<string> rootBoneNames, int maxDepth = 0) => WriteNames(rootBoneNames, maxDepth, false);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", ParameterUnits = new[] { "angularVelocity=rad/s" }, ParameterCoordinates = new[] { "angularVelocity=world" })]
    public void PushAngular(Vector3 angularVelocity) => Invoke(Service, 20, false, values: [angularVelocity.X, angularVelocity.Y, angularVelocity.Z]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", ParameterUnits = new[] { "origin=m", "angularVelocity=rad/s", "radius=m" }, ParameterCoordinates = new[] { "origin=world", "angularVelocity=world" })]
    public void PushAngularAt(Vector3 origin, Vector3 angularVelocity, float radius) => Invoke(Service, 21, false, values: [origin.X, origin.Y, origin.Z, angularVelocity.X, angularVelocity.Y, angularVelocity.Z, radius]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", ParameterUnits = new[] { "gravity=m/s^2" })]
    public void SetGravity(float gravity) => Invoke(Service, 22, false, values: [gravity]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", ParameterUnits = new[] { "blendIn=s", "blendOut=s" })]
    public void SetBlend(float blendIn, float blendOut) => Invoke(Service, 23, false, values: [blendIn, blendOut]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", Summary = "Sets native profile-relative drive and damping multipliers; falloff clamps to zero-to-one.")]
    public void SetMuscle(float stiffness, float falloff, float damping) => Invoke(Service, 24, false, values: [stiffness, falloff, damping]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", ParameterUnits = new[] { "impactSlack=0_to_1", "recoverySeconds=s" })]
    public void SetRecovery(float impactSlack, float recoverySeconds) => Invoke(Service, 25, false, values: [impactSlack, recoverySeconds]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", ParameterUnits = new[] { "distance=m" })]
    public void SetCollapse(float distance) => Invoke(Service, 26, false, values: [distance]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", ParameterUnits = new[] { "maxDistance=m", "maxDegrees=degree" })]
    public void SetStandingGuard(bool enabled, float maxDistance, float maxDegrees) => Invoke(Service, 27, false, enabled ? 1 : 0, [maxDistance, maxDegrees]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed")]
    public void SetProfile(RagdollProfile profile) => Invoke(Service, 28, false, RuntimeComponentValue.EnumValue(profile));
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", Summary = "Atomically replaces excluded bone branches after staging all valid names; empty input clears the exclusions.", Failure = "invalid_names_or_transfer_failure_preserves_previous_recipe")]
    public void SetExcludedBranches(ReadOnlySpan<string> rootBoneNames) => WriteNames(rootBoneNames, 0, true);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", ParameterUnits = new[] { "scale=dimensionless", "sag=m", "tiltDegrees=degree" })]
    public void SetRootAnchor(float scale, float sag, float tiltDegrees) => Invoke(Service, 30, false, values: [scale, sag, tiltDegrees]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed")]
    public void SetContacts(bool world, bool dynamic, bool self, bool whileActive) => Invoke(Service, 31, false, (world ? 1 : 0) | (dynamic ? 2 : 0) | (self ? 4 : 0) | (whileActive ? 8 : 0));
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", ParameterUnits = new[] { "offset=m" })]
    public void SetGroundPlane(bool enabled, float offset = 0) => Invoke(Service, 32, false, enabled ? 1 : 0, [offset]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", Summary = "Selects a GUID or .ragdoll asset; empty restores the preset. Invalid reloads retain the previous valid definition.")]
    public void SetPhysicsAsset(string reference) => Invoke(Service, 33, false, text: [reference]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", Summary = "Overrides a built body's animation drive. Frequencies are Hz, force N, torque N*m; zero disables that axis.", Failure = "missing_body_returns_false_and_preserves_settings")]
    public bool SetBodyDrive(string bone, RagdollDriveSettings settings) => Invoke(Service, 34, false,
        (settings.Enabled ? 1 : 0) | (RuntimeComponentValue.EnumValue(settings.Space) << 1),
        [settings.LinearFrequency, settings.AngularFrequency, settings.LinearDampingRatio,
         settings.AngularDampingRatio, settings.MaxForce, settings.MaxTorque, settings.TargetVelocityMultiplier], [bone]).Flag;
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", Summary = "Restores the body's authored drive; returns false if the body is unavailable.")]
    public bool ClearBodyDrive(string bone) => Invoke(Service, 35, false, text: [bone]).Flag;
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed", ParameterUnits = new[] { "staticFriction=dimensionless", "dynamicFriction=dimensionless", "restitution=0_to_1" })]
    public void SetSurface(float staticFriction, float dynamicFriction, float restitution = 0) => Invoke(Service, 36, false, values: [staticFriction, dynamicFriction, restitution]);
    void WriteNames(ReadOnlySpan<string> names, int depth, bool exclusions)
    {
        if (depth < 0) throw new ArgumentOutOfRangeException(nameof(depth));
        UTF8Encoding utf8 = new(false, true);
        foreach (string name in names) {
            ArgumentException.ThrowIfNullOrEmpty(name);
            if (name.IndexOf('\0') >= 0) throw new ArgumentException("Bone names cannot contain NUL.", nameof(names));
            utf8.GetByteCount(name);
        }
        ScriptExecutionScope scope = Scope();
        long token = scope.Bridge.InvokeService(scope, GameObject.ObjectId, Service, 40, depth, [exclusions ? 1 : 0], [names.Length.ToString(CultureInfo.InvariantCulture)], null, false).Integer;
        bool committed = false;
        try {
            for (int index = 0; index < names.Length; index++) {
                string name = names[index]; int start = 0; long offset = 0;
                while (start < name.Length) {
                    int length = Math.Min(8192, name.Length - start);
                    if (start + length < name.Length && char.IsHighSurrogate(name[start + length - 1])) length--;
                    string chunk = name.Substring(start, length); bool last = start + length == name.Length;
                    scope.Bridge.InvokeService(scope, GameObject.ObjectId, Service, 41, token, [last ? 1 : 0],
                        [index.ToString(CultureInfo.InvariantCulture), offset.ToString(CultureInfo.InvariantCulture), chunk], null, false);
                    offset += utf8.GetByteCount(chunk); start += length;
                }
            }
            scope.Bridge.InvokeService(scope, GameObject.ObjectId, Service, 42, token, [], [], null, false); committed = true;
        } finally {
            if (!committed) {
                try { scope.Bridge.InvokeService(scope, GameObject.ObjectId, Service, 43, token, [], [], null, false); }
                catch (InvalidOperationException) { }
            }
        }
    }
}

public sealed class SpringBone : NativeComponent
{
    internal SpringBone(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.SPRING_BONE, lifetimeToken) { }
    const uint Service = (uint)ScriptServiceKind.SERVICE_SPRING_BONE;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public bool Enabled { get => Invoke(Service, 0, true).Flag; set => Invoke(Service, 1, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Summary = "Creates a chain on the existing SpringBone row or updates its depth; new chains start with weight and force zero.")]
    public void EnsureChain(string rootBoneName, int maxDepth = 0) => Invoke(Service, 2, false, maxDepth, text: [rootBoneName]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query")]
    public bool HasChain(string rootBoneName) => Invoke(Service, 3, true, text: [rootBoneName]).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query")]
    public bool IsChainEnabled(string rootBoneName) => Invoke(Service, 4, true, text: [rootBoneName]).Flag;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void SetChainEnabled(string rootBoneName, bool enabled) => Invoke(Service, 5, false, enabled ? 1 : 0, text: [rootBoneName]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Units = "0_to_1")]
    public float GetWeight(string rootBoneName) => Invoke(Service, 6, true, text: [rootBoneName]).Scalar;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Summary = "Missing named chains are unchanged; weight clamps to zero-to-one.")]
    public void SetWeight(string rootBoneName, float weight) => Invoke(Service, 7, false, values: [weight], text: [rootBoneName]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "stiffness=0_to_1", "damping=0_to_1" })]
    public void SetSpring(string rootBoneName, float stiffness, float damping) => Invoke(Service, 8, false, values: [stiffness, damping], text: [rootBoneName]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "degrees=degree" })]
    public void SetLimitAngle(string rootBoneName, float degrees) => Invoke(Service, 9, false, values: [degrees], text: [rootBoneName]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "radius=m" })]
    public void SetRadius(string rootBoneName, float radius) => Invoke(Service, 10, false, values: [radius], text: [rootBoneName]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "power=m/s^2" }, ParameterCoordinates = new[] { "direction=world" })]
    public void SetForce(string rootBoneName, Vector3 direction, float power) => Invoke(Service, 11, false, values: [direction.X, direction.Y, direction.Z, power], text: [rootBoneName]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void ClearForce(string rootBoneName) => Invoke(Service, 12, false, text: [rootBoneName]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Summary = "Drops inertia; the next native update rebuilds resting chain state.")]
    public void ResetAll() => Invoke(Service, 13, false);
}

public sealed class UIAnimator : NativeComponent
{
    internal UIAnimator(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.UI_ANIMATOR, lifetimeToken) { }
    const uint Service = (uint)ScriptServiceKind.SERVICE_UI_ANIMATOR;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool Enabled { get => Invoke(Service, 0, true).Flag; set => Invoke(Service, 1, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public bool IsPlaying => Invoke(Service, 2, true).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", ParameterUnits = new[] { "from=linear_rgba", "to=linear_rgba", "duration=s" })]
    public void PlayColor(Vector4 from, Vector4 to, float duration, UIEasing easing = UIEasing.LINEAR, bool loop = false, bool pingPong = false)
        => Invoke(Service, 3, false, Options(easing, loop, pingPong), [from.X, from.Y, from.Z, from.W, to.X, to.Y, to.Z, to.W, duration]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", ParameterUnits = new[] { "duration=s" }, ParameterCoordinates = new[] { "from=ui_local", "to=ui_local" })]
    public void PlayPosition(Vector2 from, Vector2 to, float duration, UIEasing easing = UIEasing.LINEAR, bool loop = false, bool pingPong = false)
        => Invoke(Service, 4, false, Options(easing, loop, pingPong), [from.X, from.Y, to.X, to.Y, duration]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", ParameterUnits = new[] { "from=dimensionless", "to=dimensionless", "duration=s" })]
    public void PlayScale(Vector2 from, Vector2 to, float duration, UIEasing easing = UIEasing.LINEAR, bool loop = false, bool pingPong = false)
        => Invoke(Service, 5, false, Options(easing, loop, pingPong), [from.X, from.Y, to.X, to.Y, duration]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", ParameterUnits = new[] { "fromDegrees=degree", "toDegrees=degree", "duration=s" })]
    public void PlayRotation(float fromDegrees, float toDegrees, float duration, UIEasing easing = UIEasing.LINEAR, bool loop = false, bool pingPong = false)
        => Invoke(Service, 6, false, Options(easing, loop, pingPong), [fromDegrees, toDegrees, duration]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", ParameterUnits = new[] { "from=0_to_1", "to=0_to_1", "duration=s" })]
    public void PlayFill(float from, float to, float duration, UIEasing easing = UIEasing.LINEAR, bool loop = false, bool pingPong = false)
        => Invoke(Service, 7, false, Options(easing, loop, pingPong), [from, to, duration]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", ParameterUnits = new[] { "duration=s" }, Summary = "Tweens an individual UIImage material override; the property declaration defines value units.")]
    public void PlayMaterialFloat(string property, float from, float to, float duration, UIEasing easing = UIEasing.LINEAR, bool loop = false, bool pingPong = false)
        => Invoke(Service, 8, false, Options(easing, loop, pingPong), [from, to, duration], [property]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", ParameterUnits = new[] { "seconds=s" }, Summary = "Resets the common delay before calling a Play method.")]
    public void SetDelay(float seconds) => Invoke(Service, 9, false, values: [seconds]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live")]
    public void StopAll() => Invoke(Service, 10, false);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live")]
    public void Stop(UITweenChannel channel) => Invoke(Service, 11, false, RuntimeComponentValue.EnumValue(channel));
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live")]
    public void StopColor() => Stop(UITweenChannel.COLOR);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live")]
    public void StopPosition() => Stop(UITweenChannel.POSITION);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live")]
    public void StopScale() => Stop(UITweenChannel.SCALE);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live")]
    public void StopRotation() => Stop(UITweenChannel.ROTATION);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live")]
    public void StopFill() => Stop(UITweenChannel.FILL);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live")]
    public void StopMaterial() => Stop(UITweenChannel.MATERIAL);
    static int Options(UIEasing easing, bool loop, bool pingPong) => RuntimeComponentValue.EnumValue(easing) | (loop ? 4 : 0) | (pingPong ? 8 : 0);
}

public sealed class NavMeshPatrol : NativeComponent
{
    internal NavMeshPatrol(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.NAV_MESH_PATROL, lifetimeToken) { }
    const uint Service = (uint)ScriptServiceKind.SERVICE_PATROL;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public bool Enabled { get => Invoke(Service, 0, true).Flag; set => Invoke(Service, 1, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public int CurrentIndex => Invoke(Service, 2, true).Int32;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "count")]
    public int WaypointCount => Invoke(Service, 3, true).Int32;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public PatrolMode Mode { get => RuntimeComponentValue.ReadEnum<PatrolMode>(Invoke(Service, 4, true)); set => Invoke(Service, 5, false, RuntimeComponentValue.EnumValue(value)); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" }, Units = "s")]
    public float WaitTime { get => Invoke(Service, 6, true).Scalar; set => Invoke(Service, 7, false, values: [value]); }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void ClearWaypoints() => Invoke(Service, 8, false);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "position=m", "waitTime=s", "speed=m/s" }, ParameterCoordinates = new[] { "position=world" }, Summary = "Appends a waypoint; negative wait uses common wait, nonpositive speed uses Agent speed.")]
    public void AddWaypoint(Vector3 position, float waitTime = -1, float speed = -1) => Invoke(Service, 9, false, values: [position.X, position.Y, position.Z, waitTime, speed]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "position=m" }, ParameterCoordinates = new[] { "position=world" }, Failure = "out_of_range_returns_false_without_changes")]
    public bool SetWaypoint(int index, Vector3 position) => Invoke(Service, 10, false, index, [position.X, position.Y, position.Z]).Flag;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void Restart(int startIndex = 0) => Invoke(Service, 11, false, startIndex);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Units = "m", Coordinate = "world", Failure = "out_of_range_returns_false_and_default_position")]
    public bool TryGetWaypoint(int index, out Vector3 position)
    {
        NativeServiceResult result = Invoke(Service, 12, true, index); position = default;
        if (!result.Flag) return false;
        position = result.Vector3; return true;
    }
}

public sealed class SplineFollower : NativeComponent
{
    internal SplineFollower(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.SPLINE_FOLLOWER, lifetimeToken) { }
    const uint Service = (uint)ScriptServiceKind.SERVICE_SPLINE_FOLLOWER;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public bool Enabled { get => Invoke(Service, 0, true).Flag; set => Invoke(Service, 1, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public bool IsPlaying => Invoke(Service, 2, true).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" }, Units = "m/s")]
    public float Speed { get => Invoke(Service, 3, true).Scalar; set => Invoke(Service, 4, false, values: [value]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" }, Units = "0_to_1")]
    public float NormalizedPosition { get => Invoke(Service, 5, true).Scalar; set => Invoke(Service, 6, false, values: [value]); }
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Summary = "Restarts at zero or one according to Reverse, otherwise continues current progress.")]
    public void Play(bool restart = false) => Invoke(Service, 7, false, restart ? 1 : 0);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live")]
    public void Pause() => Invoke(Service, 8, false);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public SplineWrapMode WrapMode { get => RuntimeComponentValue.ReadEnum<SplineWrapMode>(Invoke(Service, 9, true)); set => Invoke(Service, 10, false, RuntimeComponentValue.EnumValue(value)); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public bool OrientToPath { get => Invoke(Service, 11, true).Flag; set => Invoke(Service, 12, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public bool Reverse { get => Invoke(Service, 13, true).Flag; set => Invoke(Service, 14, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Lifetime = "exact_current_scene_entity")]
    public GameObject? SplineObject => Invoke(Service, 15, true).GameObject;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Summary = "Binds an existing native Spline row in the current Scene; null clears the reference.")]
    public void BindSpline(GameObject? splineObject) => Invoke(Service, 16, false, related: splineObject);
}

public readonly struct CameraShakeSettings
{
    internal CameraShakeSettings(NativeServiceResult result)
    {
        if (result.ValueCount != 5) throw new InvalidOperationException("Invalid native CameraShake settings response.");
        Amplitude = result.Value(0); RotationAmplitudeDegrees = result.Value(1); Frequency = result.Value(2); Duration = result.Value(3); FalloffPower = result.Value(4);
    }
    public float Amplitude { get; }
    public float RotationAmplitudeDegrees { get; }
    public float Frequency { get; }
    public float Duration { get; }
    public float FalloffPower { get; }
}
public sealed class CameraShake : NativeComponent
{
    internal CameraShake(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.CAMERA_SHAKE, lifetimeToken) { }
    const uint Service = (uint)ScriptServiceKind.SERVICE_CAMERA_SHAKE;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public bool Enabled { get => Invoke(Service, 0, true).Flag; set => Invoke(Service, 1, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public bool IsPlaying => Invoke(Service, 2, true).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "s")]
    public float Elapsed => Invoke(Service, 3, true).Scalar;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "amplitude=m", "duration=s" }, Summary = "Starts native deterministic shake, resetting elapsed; negative amplitude and duration clamp to zero.")]
    public void Play(float amplitude, float duration, int seed = 1) => Invoke(Service, 4, false, seed, [amplitude, duration]);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", Summary = "Stops playback; CameraRigSystem restores the previously applied offset on its next update.")]
    public void Stop() => Invoke(Service, 5, false);
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "rotationAmplitudeDegrees=degree", "frequency=1/s", "falloffPower=dimensionless" })]
    public void Configure(float rotationAmplitudeDegrees, float frequency, float falloffPower) => Invoke(Service, 6, false, values: [rotationAmplitudeDegrees, frequency, falloffPower]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public CameraShakeSettings Settings => new(Invoke(Service, 7, true));
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public int Seed => Invoke(Service, 8, true).Int32;
}

public sealed class VirtualCamera : NativeComponent
{
    internal VirtualCamera(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.VIRTUAL_CAMERA, lifetimeToken) { }
    const uint Service = (uint)ScriptServiceKind.SERVICE_VIRTUAL_CAMERA;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool Enabled { get => Invoke(Service, 0, true).Flag; set => Invoke(Service, 1, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public bool IsActive => Invoke(Service, 2, true).Flag;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public int Priority { get => Invoke(Service, 3, true).Int32; set => Invoke(Service, 4, false, value); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", Units = "degree", Summary = "Vertical field of view, clamped to 1 through 179 degrees.")]
    public float FieldOfView { get => Invoke(Service, 5, true).Scalar; set => Invoke(Service, 6, false, values: [value]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", Units = "s")]
    public float BlendInTime { get => Invoke(Service, 7, true).Scalar; set => Invoke(Service, 8, false, values: [value]); }
}

public sealed class AudioMixerSend : NativeComponent
{
    internal AudioMixerSend(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.AUDIO_MIXER_SEND, lifetimeToken) { }
    const uint Service = (uint)ScriptServiceKind.SERVICE_AUDIO_MIXER_SEND;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", WriteRunModes = new[] { "Play" })]
    public bool Enabled { get => Invoke(Service, 0, true).Flag; set => Invoke(Service, 1, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query")]
    public string Bus => Invoke(Service, 2, true).String;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "0_to_1")]
    public float Level => Invoke(Service, 3, true).Scalar;
    [ApiContract(RunModes = new[] { "Play" }, Call = "live", ParameterUnits = new[] { "level=0_to_1" }, Summary = "Changes the auxiliary bus and native clamped send level together; main source volume is unchanged.")]
    public void Set(string bus, float level) => Invoke(Service, 4, false, values: [level], text: [bus]);
}

public sealed class FlowField : NativeComponent
{
    internal FlowField(GameObject owner, ulong lifetimeToken = 0) : base(owner, (uint)NativeComponentKind.FLOW_FIELD, lifetimeToken) { }
    const uint Service = (uint)ScriptServiceKind.SERVICE_FLOW_FIELD;
    /// @note 先頭の流れを操作する。空のリストは診断し暗黙に追加しない。
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live")]
    public bool Enabled { get => Invoke(Service, 0, true).Flag; set => Invoke(Service, 1, false, value ? 1 : 0); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", Summary = "Reads all native field types; assignment accepts Uniform through Curl. Baked and legacy Drag are authored-only.")]
    public FlowFieldType Type
    {
        get => RuntimeComponentValue.ReadEnum<FlowFieldType>(Invoke(Service, 2, true));
        set { int type = RuntimeComponentValue.EnumValue(value); if (type > 4) throw new ArgumentOutOfRangeException(nameof(value)); Invoke(Service, 3, false, type); }
    }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", Units = "m/s_or_baked_dimensionless_scale")]
    public float Speed { get => Invoke(Service, 4, true).Scalar; set => Invoke(Service, 5, false, values: [value]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "m")]
    public float Radius => Invoke(Service, 6, true).Scalar;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", ParameterUnits = new[] { "radius=m", "falloffPower=dimensionless" }, Summary = "Nonpositive radius removes distance attenuation; falloff clamps to at least 0.01.")]
    public void SetRadius(float radius, float falloffPower = 2) => Invoke(Service, 7, false, values: [radius, falloffPower]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", Coordinate = "component_local_direction")]
    public Vector3 Direction { get => Invoke(Service, 8, true).Vector3; set => Invoke(Service, 9, false, values: [value.X, value.Y, value.Z]); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "query", Summary = "Returns native noise frequency and scroll speed.")]
    public Vector2 GetTurbulence() => Invoke(Service, 10, true).Vector2;
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Call = "live", ParameterUnits = new[] { "frequency=1/m", "speed=noise_scroll/s" })]
    public void SetTurbulence(float frequency, float speed) => Invoke(Service, 11, false, values: [frequency, speed]);
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Write = "live", Summary = "Reads first-force channels; writes the full uint32 mask to every force as native FlowField does.")]
    public uint Channels { get => checked((uint)Invoke(Service, 12, true).Integer); set => Invoke(Service, 13, false, value); }
    [ApiContract(RunModes = new[] { "Edit", "Play" }, Read = "query", Units = "count")]
    public int ForceCount => Invoke(Service, 16, true).Int32;
}
