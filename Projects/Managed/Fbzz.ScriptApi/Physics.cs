/// @file    Physics.cs
/// @brief   現在の callback で検証する剛体 facade と自作物理のワールド検索。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;

namespace Fbzz;

/// @note 取得時の剛体 birth token を保持し、除去・再追加で旧参照を復活させない。
/// @note 状態変更は Play の FixedUpdate のみ。失敗は例外で通知し、native の状態を部分変更しない。
public sealed class RigidBody : Component, IEquatable<RigidBody>
{
    internal RigidBody(GameObject owner, ulong lifetimeToken = 0)
    { m_owner = owner; LifetimeToken = lifetimeToken != 0 ? lifetimeToken : NativeComponentFactory.CaptureLifetime(owner, 1); }
    internal ulong LifetimeToken { get; }
    readonly GameObject m_owner;
    public bool Equals(RigidBody? other) => other is not null && LifetimeToken == other.LifetimeToken && m_owner.Equals(other.m_owner);
    public override bool Equals(object? other) => other is RigidBody body && Equals(body);
    public override int GetHashCode() => HashCode.Combine(m_owner, 1U, LifetimeToken);
    public override GameObject GameObject => m_owner;
    public override bool IsAlive
    {
        get
        {
            ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
            return m_owner.SceneLifetime == scope.SceneLifetime && LifetimeToken != 0
                && scope.Bridge.ReadComponentLifetime(scope, m_owner.ObjectId, 1) == LifetimeToken;
        }
    }

    /// @note ワールド空間 [m/s]。固定された軸の速度は native と同じく零になる。
    [ApiContract(Write = "fixed", Units = "m/s", Coordinate = "world_physics_pose")]
    public Vector3 Velocity
   {
        get => ReadVector(PhysicsBodyValue.VELOCITY);
        set => WriteVector(PhysicsBodyValue.VELOCITY, value);
    }
    /// @note ワールド空間の回転軸 × 角速度 [rad/s]。
    [ApiContract(Write = "fixed", Units = "rad/s", Coordinate = "world_physics_pose")]
    public Vector3 AngularVelocity
   {
        get => ReadVector(PhysicsBodyValue.ANGULAR_VELOCITY);
        set => WriteVector(PhysicsBodyValue.ANGULAR_VELOCITY, value);
    }
    /// @note 物理の確定位置 [m]。描画補間された Transform.WorldPosition と異なる場合がある。
    [ApiContract(Units = "m", Coordinate = "world_physics_pose")]
    public Vector3 Position => ReadVector(PhysicsBodyValue.POSITION);
    /// @note 有限かつ非負 [kg]。零は無限質量。正値は逆数も有限でなければ拒否する。
    [ApiContract(Write = "fixed", Units = "kg", Coordinate = "not_applicable", Failure = "finite_nonnegative_mass_required; zero_is_infinite_mass; positive_inverse_must_be_finite")]
    public float Mass
   {
        get => ReadScalar(PhysicsBodyValue.MASS);
        set
        {
            if (!float.IsFinite(value) || value < 0f || (value > 0f && !float.IsFinite(1f / value)))
                throw new ArgumentOutOfRangeException(nameof(value));
            WriteScalar(PhysicsBodyValue.MASS, value);
        }
    }
    /// @note ワールド重力に掛ける有限の倍率。負値は重力方向を反転する。
    [ApiContract(Write = "fixed")]
    public float GravityScale
    {
        get => ReadScalar(PhysicsBodyValue.GRAVITY_SCALE);
        set { ValidateFinite(value); WriteScalar(PhysicsBodyValue.GRAVITY_SCALE, value); }
    }
    [ApiContract(Write = "fixed")]
    public bool IsStatic
    {
        get => ReadFlag(PhysicsBodyValue.IS_STATIC);
        set
        {
            ScriptExecutionScope scope = RequireMutationScope();
            scope.Bridge.WritePhysicsFlag(scope, m_owner.ObjectId, (uint)PhysicsBodyValue.IS_STATIC, value);
        }
    }
    public bool IsSleeping => ReadFlag(PhysicsBodyValue.IS_SLEEPING);
    public bool Enabled => ReadFlag(PhysicsBodyValue.ENABLED);
    /// @note ワールド空間の単位回転。変更は FixedUpdate 内で子孫の Transform と同時に反映する。
    [ApiContract(Write = "fixed", Units = "unitless_quaternion_xyzw", Coordinate = "world_physics_pose")]
    public Quaternion Rotation
   {
        get => Extended(153, true).Quaternion;
        set
        {
            float length = value.Length;
            if (!value.IsFinite || !float.IsFinite(length) || MathF.Abs(length) < Mathf.EPSILON)
                throw new ArgumentOutOfRangeException(nameof(value));
            Extended(154, false, [value.X, value.Y, value.Z, value.W]);
        }
    }
    [ApiContract(Write = "fixed")]
    public float LinearDrag { get => ReadScalar(PhysicsBodyValue.LINEAR_DRAG); set { ValidateNonNegative(value); WriteScalar(PhysicsBodyValue.LINEAR_DRAG, value); } }
    [ApiContract(Write = "fixed")]
    public float AngularDrag { get => ReadScalar(PhysicsBodyValue.ANGULAR_DRAG); set { ValidateNonNegative(value); WriteScalar(PhysicsBodyValue.ANGULAR_DRAG, value); } }
    /// @note 自作 CCD は SphereCollider を持つ高速物体の TOI を扱う。
    [ApiContract(Write = "fixed")]
    public bool UseCCD { get => ReadFlag(PhysicsBodyValue.USE_CCD); set => WriteFlag(PhysicsBodyValue.USE_CCD, value); }
    [ApiContract(Write = "fixed")]
    public float CCDRadius { get => ReadScalar(PhysicsBodyValue.CCD_RADIUS); set { ValidateNonNegative(value); WriteScalar(PhysicsBodyValue.CCD_RADIUS, value); } }
    [ApiContract(Write = "fixed")]
    public bool UseGravity { get => ReadFlag(PhysicsBodyValue.USE_GRAVITY); set => WriteFlag(PhysicsBodyValue.USE_GRAVITY, value); }
    [ApiContract(Write = "fixed")]
    public bool AllowSleeping { get => ReadFlag(PhysicsBodyValue.ALLOW_SLEEPING); set => WriteFlag(PhysicsBodyValue.ALLOW_SLEEPING, value); }
    /// @note 個体の時計倍率 [0,8]。
    [ApiContract(Write = "fixed")]
    public float LocalTimeScale
    {
        get => ReadScalar(PhysicsBodyValue.LOCAL_TIME_SCALE);
        set { if (!float.IsFinite(value) || value < 0f || value > 8f) throw new ArgumentOutOfRangeException(nameof(value)); WriteScalar(PhysicsBodyValue.LOCAL_TIME_SCALE, value); }
    }
    /// @note 風・水流との結合係数 [1/s]。零は影響を受けない。
    [ApiContract(Write = "fixed")]
    public float FlowCoupling { get => ReadScalar(PhysicsBodyValue.FLOW_COUPLING); set { ValidateNonNegative(value); WriteScalar(PhysicsBodyValue.FLOW_COUPLING, value); } }

    /// @note ワールド空間の力 [N]。次の物理積分まで蓄積し、sleep を解除する。
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "force=N" }, ParameterCoordinates = new[] { "force=world" })]
    public void AddForce(Vector3 force) => Command(PhysicsBodyCommand.FORCE, force);
    /// @note ワールド空間のインパルス [N·s]。速度へ即時反映する。
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "impulse=N*s" }, ParameterCoordinates = new[] { "impulse=world" })]
    public void AddImpulse(Vector3 impulse) => Command(PhysicsBodyCommand.IMPULSE, impulse);
    /// @note ワールド空間のトルク [N·m]。次の物理積分まで蓄積する。
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "torque=N*m" }, ParameterCoordinates = new[] { "torque=world" })]
    public void AddTorque(Vector3 torque) => Command(PhysicsBodyCommand.TORQUE, torque);
    [ApiContract(Call = "fixed")]
    public void AddAngularImpulse(Vector3 impulse) => Command(PhysicsBodyCommand.ANGULAR_IMPULSE, impulse);
    /// @note impulse はワールド空間 [N·s]、worldPoint は [m]。並進と回転を同時に変える。
    [ApiContract(Call = "fixed")]
    public void AddImpulseAtPoint(Vector3 impulse, Vector3 worldPoint)
    {
        ValidateFinite(impulse); ValidateFinite(worldPoint);
        ScriptExecutionScope scope = RequireMutationScope();
        scope.Bridge.PhysicsBodyCommand(scope, m_owner.ObjectId, (uint)PhysicsBodyCommand.POINT_IMPULSE, impulse, worldPoint, 0);
    }
    /// @note worldPoint はワールド空間 [m]。重心からの距離に応じてトルクも加える。
    [ApiContract(Call = "fixed")]
    public void AddForceAtPoint(Vector3 force, Vector3 worldPoint)
    {
        ValidateFinite(force); ValidateFinite(worldPoint);
        ScriptExecutionScope scope = RequireMutationScope();
        scope.Bridge.PhysicsBodyCommand(scope, m_owner.ObjectId, (uint)PhysicsBodyCommand.FORCE_AT_POINT, force, worldPoint, 0);
    }
    [ApiContract(Call = "fixed")]
    public void WakeUp() => Command(PhysicsBodyCommand.WAKE, Vector3.ZERO);
    [ApiContract(Call = "fixed")]
    public void Sleep() => Command(PhysicsBodyCommand.SLEEP, Vector3.ZERO);
    [ApiContract(Call = "fixed")]
    public void SetFreezePosition(bool x, bool y, bool z) => Freeze(PhysicsBodyCommand.FREEZE_POSITION, x, y, z);
    [ApiContract(Call = "fixed")]
    public void SetFreezeRotation(bool x, bool y, bool z) => Freeze(PhysicsBodyCommand.FREEZE_ROTATION, x, y, z);
    /// @note ワールド位置 [m] を物理と子孫の Transform へ即時反映し、速度・ローカル回転・拡縮を保持する。
    /// @note 位置の固定軸は元の位置を保つ。親の零スケールで物理位置を表せない場合は全体を拒否する。
    [ApiContract(Call = "fixed", ParameterUnits = new[] { "worldPosition=m" }, ParameterCoordinates = new[] { "worldPosition=world" })]
    public void Teleport(Vector3 worldPosition) => Command(PhysicsBodyCommand.TELEPORT, worldPosition);

    ScriptExecutionScope RequireScope()
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        if (m_owner.SceneLifetime != scope.SceneLifetime)
            throw new InvalidOperationException("The rigid body belongs to another scene.");
        if (LifetimeToken == 0 || scope.Bridge.ReadComponentLifetime(scope, m_owner.ObjectId, 1) != LifetimeToken)
            throw new InvalidOperationException("The rigid body lifetime has expired; obtain a new reference explicitly.");
        return scope;
    }
    ScriptExecutionScope RequireMutationScope()
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        if (scope.Callback != ScriptCallback.FIXED_UPDATE)
            throw new InvalidOperationException("Rigid body changes require FixedUpdate in Play.");
        return RequireScope();
    }
    Vector3 ReadVector(PhysicsBodyValue selector)
    { ScriptExecutionScope scope = RequireScope(); return scope.Bridge.ReadPhysicsVector(scope, m_owner.ObjectId, (uint)selector); }
    float ReadScalar(PhysicsBodyValue selector)
    { ScriptExecutionScope scope = RequireScope(); return scope.Bridge.ReadPhysicsScalar(scope, m_owner.ObjectId, (uint)selector); }
    bool ReadFlag(PhysicsBodyValue selector)
    { ScriptExecutionScope scope = RequireScope(); return scope.Bridge.ReadPhysicsFlag(scope, m_owner.ObjectId, (uint)selector); }
    void WriteVector(PhysicsBodyValue selector, Vector3 value)
    { ValidateFinite(value); ScriptExecutionScope scope = RequireMutationScope(); scope.Bridge.WritePhysicsVector(scope, m_owner.ObjectId, (uint)selector, value); }
    void WriteScalar(PhysicsBodyValue selector, float value)
    { ScriptExecutionScope scope = RequireMutationScope(); scope.Bridge.WritePhysicsScalar(scope, m_owner.ObjectId, (uint)selector, value); }
    void WriteFlag(PhysicsBodyValue selector, bool value)
    { ScriptExecutionScope scope = RequireMutationScope(); scope.Bridge.WritePhysicsFlag(scope, m_owner.ObjectId, (uint)selector, value); }
    NativeServiceResult Extended(uint operation, bool readOnly, ReadOnlySpan<float> values = default)
    {
        ScriptExecutionScope scope = readOnly ? RequireScope() : RequireMutationScope();
        return scope.Bridge.InvokeService(scope, m_owner.ObjectId, operation, 0, 0, values, [], null, readOnly);
    }
    void Command(PhysicsBodyCommand command, Vector3 value)
    { ValidateFinite(value); ScriptExecutionScope scope = RequireMutationScope(); scope.Bridge.PhysicsBodyCommand(scope, m_owner.ObjectId, (uint)command, value, Vector3.ZERO, 0); }
    void Freeze(PhysicsBodyCommand command, bool x, bool y, bool z)
    {
        uint axes = (x ? 1u : 0u) | (y ? 2u : 0u) | (z ? 4u : 0u);
        ScriptExecutionScope scope = RequireMutationScope();
        scope.Bridge.PhysicsBodyCommand(scope, m_owner.ObjectId, (uint)command, Vector3.ZERO, Vector3.ZERO, axes);
    }
    static void ValidateFinite(Vector3 value)
    { if (!value.IsFinite) throw new ArgumentOutOfRangeException(nameof(value)); }
    static void ValidateFinite(float value)
    { if (!float.IsFinite(value)) throw new ArgumentOutOfRangeException(nameof(value)); }
    static void ValidateNonNegative(float value)
    { if (!float.IsFinite(value) || value < 0f) throw new ArgumentOutOfRangeException(nameof(value)); }
}

/// @note 現在の Collider Component を見る facade。設定の変更は Play の FixedUpdate 内に限る。
public abstract class Collider : NativeComponent
{
    internal Collider(GameObject owner, uint kind, ulong lifetimeToken = 0) : base(owner, kind, lifetimeToken) { m_kind = kind; }
    readonly uint m_kind;
    protected NativeServiceResult Read(uint property) => Invoke(150, property, true, m_kind);
    protected void Write(uint property, ReadOnlySpan<float> values = default, ReadOnlySpan<string> text = default)
    {
        if (ScriptApiRuntime.RequireScope().Callback != ScriptCallback.FIXED_UPDATE) throw new InvalidOperationException("Collider changes require FixedUpdate in Play.");
        Invoke(151, property, false, m_kind, values, text);
    }
    protected NativeServiceResult Command(uint command, ReadOnlySpan<float> values = default, ReadOnlySpan<string> text = default)
    {
        if (ScriptApiRuntime.RequireScope().Callback != ScriptCallback.FIXED_UPDATE) throw new InvalidOperationException("Collider changes require FixedUpdate in Play.");
        return Invoke(152, command, false, m_kind, values, text);
    }
    [ApiContract(Write = "fixed")]
    public bool Enabled { get => Read(0).Flag; set => Write(0, [value ? 1f : 0f]); }
    [ApiContract(Write = "fixed")]
    public bool IsTrigger { get => Read(1).Flag; set => Write(1, [value ? 1f : 0f]); }
    /// @note GameObject のローカル空間の中心 [m]。
    [ApiContract(Write = "fixed")]
    public Vector3 Center { get => Read(2).Vector3; set => Write(2, [value.X, value.Y, value.Z]); }
    [ApiContract(Write = "fixed")]
    public float StaticFriction { get => Read(4).Scalar; set { NonNegative(value); Write(4, [value]); } }
    [ApiContract(Write = "fixed")]
    public float DynamicFriction { get => Read(5).Scalar; set { NonNegative(value); Write(5, [value]); } }
    [ApiContract(Write = "fixed")]
    public float Restitution { get => Read(6).Scalar; set { if (value < 0f || value > 1f) throw new ArgumentOutOfRangeException(nameof(value)); Write(6, [value]); } }
    [ApiContract(Write = "fixed")]
    public float Density { get => Read(7).Scalar; set { NonNegative(value); Write(7, [value]); } }
    /// @note .physmat 参照。空で解除する。個別物性の変更では共有参照を解除して inline 値に切り替える。
    [ApiContract(Write = "fixed")]
    public string PhysicsMaterial { get => Read(8).String; set { ArgumentNullException.ThrowIfNull(value); Write(8, text: [value]); } }
    [ApiContract(Write = "fixed")]
    public bool AttachToParentBody { get => Read(9).Flag; set => Write(9, [value ? 1f : 0f]); }
    [ApiContract(Write = "fixed")]
    public PhysicsMaterialCombine RestitutionCombine { get => (PhysicsMaterialCombine)Read(11).Int32; set { ValidateCombine(value); Write(11, [(uint)value]); } }
    [ApiContract(Write = "fixed")]
    public PhysicsMaterialCombine FrictionCombine { get => (PhysicsMaterialCombine)Read(12).Int32; set { ValidateCombine(value); Write(12, [(uint)value]); } }
    [ApiContract(Call = "fixed")]
    public bool ApplyPhysicsMaterialPreset(string name) => Command(0, text: [name]).Flag;
    /// @note 同じ GameObject の全プリミティブを合算した world AABB。無効な形状も含む。
    [ApiContract(Call = "read", ParameterUnits = new[] { "min=m", "max=m" }, ParameterCoordinates = new[] { "min=world", "max=world" }, Summary = "Reads the combined world AABB of this GameObject primitive colliders, including disabled shapes.", Returns = "True when primitive world bounds are available; false sets both outputs to zero.", Failure = "false_sets_min_max_to_zero_when_no_primitive_bounds; invalid_scope_lifetime_or_missing_component_throws")]
    public bool TryGetWorldBounds(out Vector3 min, out Vector3 max)
    {
        NativeServiceResult result = Read(10);
        if (!result.Flag) { min = default; max = default; return false; }
        if (result.ValueCount != 6) throw new InvalidOperationException("Invalid Collider bounds.");
        min = new(result.Value(0), result.Value(1), result.Value(2)); max = new(result.Value(3), result.Value(4), result.Value(5)); return true;
    }
    protected static void NonNegative(float value)
    { if (!float.IsFinite(value) || value < 0f) throw new ArgumentOutOfRangeException(nameof(value)); }
    static void ValidateCombine(PhysicsMaterialCombine value)
    { if ((uint)value > 4) throw new ArgumentOutOfRangeException(nameof(value)); }
}

public sealed class BoxCollider : Collider
{
    internal BoxCollider(GameObject owner, ulong lifetimeToken = 0) : base(owner, 2, lifetimeToken) { }
    /// @note ローカル全寸法 [m]。各成分は有限かつ正数。
    [ApiContract(Write = "fixed")]
    public Vector3 Size
    {
        get => Read(3).Vector3;
        set { if (!value.IsFinite || value.X <= 0f || value.Y <= 0f || value.Z <= 0f) throw new ArgumentOutOfRangeException(nameof(value)); Write(3, [value.X, value.Y, value.Z]); }
    }
}

public sealed class SphereCollider : Collider
{
    internal SphereCollider(GameObject owner, ulong lifetimeToken = 0) : base(owner, 3, lifetimeToken) { }
    [ApiContract(Write = "fixed")]
    public float Radius { get => Read(3).Scalar; set { NonNegative(value); Write(3, [value]); } }
}

public sealed class CapsuleCollider : Collider
{
    internal CapsuleCollider(GameObject owner, ulong lifetimeToken = 0) : base(owner, 4, lifetimeToken) { }
    [ApiContract(Write = "fixed")]
    public float Radius { get => Read(3).Value(0); set => SetShape(value, HalfHeight); }
    [ApiContract(Write = "fixed")]
    public float HalfHeight { get => Read(3).Value(1); set => SetShape(Radius, value); }
    [ApiContract(Call = "fixed")]
    public void SetShape(float radius, float halfHeight) { NonNegative(radius); NonNegative(halfHeight); Write(3, [radius, halfHeight]); }
}

public enum PhysicsMaterialCombine : uint { AVERAGE = 0, GEOMETRIC_MEAN = 1, MINIMUM = 2, MULTIPLY = 3, MAXIMUM = 4 }
public sealed class CylinderCollider : Collider
{
    internal CylinderCollider(GameObject owner, ulong lifetimeToken = 0) : base(owner, 15, lifetimeToken) { }
    [ApiContract(Write = "fixed")]
    public float Radius { get => Read(3).Value(0); set => SetShape(value, HalfHeight); }
    [ApiContract(Write = "fixed")]
    public float HalfHeight { get => Read(3).Value(1); set => SetShape(Radius, value); }
    [ApiContract(Call = "fixed")]
    public void SetShape(float radius, float halfHeight) { NonNegative(radius); NonNegative(halfHeight); Write(3, [radius, halfHeight]); }
}
public sealed class AabbCollider : Collider
{
    internal AabbCollider(GameObject owner, ulong lifetimeToken = 0) : base(owner, 16, lifetimeToken) { }
    [ApiContract(Write = "fixed")]
    public Vector3 Size
    {
        get => Read(3).Vector3;
        set { if (!value.IsFinite || value.X <= 0f || value.Y <= 0f || value.Z <= 0f) throw new ArgumentOutOfRangeException(nameof(value)); Write(3, [value.X, value.Y, value.Z]); }
    }
}
/// @note Mesh 変更は次回の PhysicsSystem 同期で自作 mesh collider を構築する。
public class MeshCollider : Collider
{
    internal MeshCollider(GameObject owner, ulong lifetimeToken = 0) : base(owner, 17, lifetimeToken) { }
    internal MeshCollider(GameObject owner, uint kind, ulong lifetimeToken = 0) : base(owner, kind, lifetimeToken) { }
    public string MeshPath => Read(13).String;
    public int MeshIndex => Read(14).Int32;
    [ApiContract(Write = "fixed")]
    public bool UseTransformScale { get => Read(15).Flag; set => Write(15, [value ? 1f : 0f]); }
    [ApiContract(Call = "fixed")]
    public void SetMesh(string path, int meshIndex = 0)
    { if (meshIndex < 0) throw new ArgumentOutOfRangeException(nameof(meshIndex)); Command(1, text: [path, meshIndex.ToString(System.Globalization.CultureInfo.InvariantCulture)]); }
}
public sealed class ConvexHullCollider : MeshCollider { internal ConvexHullCollider(GameObject owner, ulong lifetimeToken = 0) : base(owner, 18, lifetimeToken) { } }
/// @note 同じ GameObject の Terrain データから PhysicsSystem が構築する。
public sealed class TerrainCollider : Collider { internal TerrainCollider(GameObject owner, ulong lifetimeToken = 0) : base(owner, 19, lifetimeToken) { } }

public enum QueryTriggerMode : uint { INCLUDE = 0, IGNORE = 1, ONLY = 2 }

/// @note マスクは対象 GameObject の Layer に対して評価する。既定は全 Layer・Trigger 含む・除外なし。
public readonly struct PhysicsQueryOptions
{
    public PhysicsQueryOptions(uint layerMask = uint.MaxValue, QueryTriggerMode triggers = QueryTriggerMode.INCLUDE, GameObject? ignore = null)
    { LayerMask = layerMask; Triggers = triggers; Ignore = ignore; m_initialized = true; }
    readonly bool m_initialized;
    public uint LayerMask { get; }
    public QueryTriggerMode Triggers { get; }
    public GameObject? Ignore { get; }
    internal uint EffectiveLayerMask => m_initialized ? LayerMask : uint.MaxValue;
}

/// @note 位置・法線はワールド空間、Distance は原点からの距離 [m]。native Collider のポインターを公開しない。
public readonly struct RaycastHit
{
    internal RaycastHit(GameObject gameObject, Vector3 point, Vector3 normal, float distance)
    { GameObject = gameObject; Point = point; Normal = normal; Distance = distance; }
    public GameObject GameObject { get; }
    [ApiContract(Units = "m", Coordinate = "world")]
    public Vector3 Point { get; }
    [ApiContract(Units = "unit_direction", Coordinate = "world")]
    public Vector3 Normal { get; }
    [ApiContract(Units = "m", Coordinate = "not_applicable")]
    public float Distance { get; }
}

/// @note Play の同期 callback 専用。現在の物理 World の確定 Collider を検索し、Transform の描画補間は使わない。
public sealed class ScriptPhysics
{
    internal ScriptPhysics(ScriptBinding owner) { m_owner = owner; }
    readonly ScriptBinding m_owner;
    [ApiContract(Units = "m/s^2", Coordinate = "world")]
    public Vector3 WorldGravity
   { get { ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner); return scope.Bridge.ReadWorldGravity(scope); } }
    [ApiContract(Call = "read")]
    public bool LayersCollide(int a, int b)
    {
        if ((uint)a >= 32u || (uint)b >= 32u) throw new ArgumentOutOfRangeException(nameof(a));
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.LayersCollide(scope, a, b);
    }
    /// @note direction は自動正規化する。向きが零なら false。距離は有限かつ非負 [m]。
    [ApiContract(ParameterUnits = new[] { "origin=m", "direction=direction", "maxDistance=m" }, ParameterCoordinates = new[] { "origin=world", "direction=world" }, Call = "read")]
    public bool Raycast(Vector3 origin, Vector3 direction, float maxDistance, out RaycastHit hit)
    {
        ValidateCast(origin, direction, maxDistance);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.Raycast(scope, origin, direction, maxDistance, out hit);
    }
    [ApiContract(ParameterUnits = new[] { "origin=m", "direction=direction", "maxDistance=m" }, ParameterCoordinates = new[] { "origin=world", "direction=world" }, Call = "read")]
    public bool Raycast(Vector3 origin, Vector3 direction, float maxDistance, PhysicsQueryOptions options, out RaycastHit hit)
    {
        ValidateCast(origin, direction, maxDistance); ValidateOptions(options);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.RaycastFiltered(scope, origin, direction, maxDistance, options, out hit);
    }
    /// @note radius は有限かつ非負 [m]。球以外の形状は既存自作物理の膨張 AABB 等の近似を使う。
    /// @note Point は native と同じく接触時の掃引球の中心。direction が零なら false。
    [ApiContract(ParameterUnits = new[] { "center=m", "radius=m", "direction=direction", "maxDistance=m" }, ParameterCoordinates = new[] { "center=world", "direction=world" }, Call = "read")]
    public bool SphereCast(Vector3 center, float radius, Vector3 direction, float maxDistance, out RaycastHit hit)
    {
        ValidateCast(center, direction, maxDistance); ValidateRadius(radius);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.SphereCast(scope, center, radius, direction, maxDistance, out hit);
    }
    [ApiContract(ParameterUnits = new[] { "center=m", "radius=m", "direction=direction", "maxDistance=m" }, ParameterCoordinates = new[] { "center=world", "direction=world" }, Call = "read")]
    public bool SphereCast(Vector3 center, float radius, Vector3 direction, float maxDistance, PhysicsQueryOptions options, out RaycastHit hit)
    {
        ValidateCast(center, direction, maxDistance); ValidateRadius(radius); ValidateOptions(options);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.SphereCastFiltered(scope, center, radius, direction, maxDistance, options, out hit);
    }
    /// @note 距離昇順。複数 Collider を持つ GameObject のヒットは形状ごとに残す。
    [ApiContract(ParameterUnits = new[] { "origin=m", "direction=direction", "maxDistance=m" }, ParameterCoordinates = new[] { "origin=world", "direction=world" }, Call = "read")]
    public RaycastHit[] RaycastAll(Vector3 origin, Vector3 direction, float maxDistance)
    {
        ValidateCast(origin, direction, maxDistance);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.RaycastAll(scope, origin, direction, maxDistance);
    }
    [ApiContract(ParameterUnits = new[] { "origin=m", "direction=direction", "maxDistance=m" }, ParameterCoordinates = new[] { "origin=world", "direction=world" }, Call = "read")]
    public RaycastHit[] RaycastAll(Vector3 origin, Vector3 direction, float maxDistance, PhysicsQueryOptions options)
    {
        ValidateCast(origin, direction, maxDistance); ValidateOptions(options);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.RaycastAllFiltered(scope, origin, direction, maxDistance, options);
    }
    /// @return 件数不足は Required/Written=0、出力未変更。成功時も余りを保持し、snapshot は一回の query の結果。
    [ApiContract(ParameterUnits = new[] { "origin=m", "direction=direction", "maxDistance=m" }, ParameterCoordinates = new[] { "origin=world", "direction=world" }, Call = "read", Failure = "insufficient_capacity_required_written0_output_unchanged; malformed_snapshot_throws_output_unchanged", Summary = "Reads all ordered collider hits into caller-owned storage; short output is unchanged and excess capacity is retained. Native snapshot/candidate memory and GameObject wrappers remain proportional to the result.")]
    public CollectionReadResult RaycastAll(Vector3 origin, Vector3 direction, float maxDistance, Span<RaycastHit> buffer, PhysicsQueryOptions? options = null)
    {
        ValidateCast(origin, direction, maxDistance); if (options.HasValue) ValidateOptions(options.Value);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.ReadRaycastAll(scope, origin, direction, maxDistance, buffer, options);
    }
    /// @note GameObject の重複を除き、順序は保証しない。零半径も既存 native の点接触判定を使う。
    [ApiContract(ParameterUnits = new[] { "center=m", "radius=m" }, ParameterCoordinates = new[] { "center=world" }, Call = "read")]
    public GameObject[] OverlapSphere(Vector3 center, float radius)
    {
        if (!center.IsFinite) throw new ArgumentOutOfRangeException(nameof(center));
        ValidateRadius(radius);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.OverlapSphere(scope, center, radius);
    }
    [ApiContract(ParameterUnits = new[] { "center=m", "radius=m" }, ParameterCoordinates = new[] { "center=world" }, Call = "read")]
    public GameObject[] OverlapSphere(Vector3 center, float radius, PhysicsQueryOptions options)
    {
        if (!center.IsFinite) throw new ArgumentOutOfRangeException(nameof(center));
        ValidateRadius(radius); ValidateOptions(options);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.OverlapSphereFiltered(scope, center, radius, options);
    }
    /// @return 件数不足は Required/Written=0、出力未変更。GameObject 重複は除去し、成功時も余りを保持する。
    [ApiContract(ParameterUnits = new[] { "center=m", "radius=m" }, ParameterCoordinates = new[] { "center=world" }, Call = "read", Failure = "insufficient_capacity_required_written0_output_unchanged; malformed_snapshot_throws_output_unchanged", Summary = "Reads every unique overlapping GameObject into caller-owned storage, preserving short buffers and unused tail. Native snapshots and managed identity wrappers require result-sized working memory.")]
    public CollectionReadResult OverlapSphere(Vector3 center, float radius, Span<GameObject> buffer, PhysicsQueryOptions? options = null)
    {
        if (!center.IsFinite) throw new ArgumentOutOfRangeException(nameof(center));
        ValidateRadius(radius); if (options.HasValue) ValidateOptions(options.Value);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        return scope.Bridge.ReadOverlapSphere(scope, center, radius, buffer, options);
    }
    static void ValidateOptions(PhysicsQueryOptions options)
    {
        if ((uint)options.Triggers > 2) throw new ArgumentOutOfRangeException(nameof(options));
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        if (options.Ignore is not null && options.Ignore.SceneLifetime != scope.SceneLifetime) throw new InvalidOperationException("The ignored object belongs to another scene.");
    }
    static void ValidateCast(Vector3 origin, Vector3 direction, float maxDistance)
    {
        if (!origin.IsFinite || !direction.IsFinite || !float.IsFinite(direction.LengthSquared)
            || !float.IsFinite(maxDistance) || maxDistance < 0f)
            throw new ArgumentOutOfRangeException(nameof(origin));
    }
    static void ValidateRadius(float radius)
    { if (!float.IsFinite(radius) || radius < 0f) throw new ArgumentOutOfRangeException(nameof(radius)); }
}
