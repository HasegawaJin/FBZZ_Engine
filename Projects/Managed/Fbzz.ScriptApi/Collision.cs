/// @file    Collision.cs
/// @brief   native 物理通知のワールド接触値と保持可能な相手参照。
/// @author  Hasegawa Jin
/// @date    2026-10-04
namespace Fbzz;

/// @note 固定物理ステップで採った値の snapshot。Other は非所有参照で、破棄後の操作は現在の callback で寿命を照合する。
/// @see Docs/design/csharp-authoring.md
public readonly struct Collision
{
    internal Collision(GameObject? other, Vector3 point, Vector3 normal, float depth,
        Vector3 relativeVelocity, float approachSpeed, float impulse)
    {
        Other = other;
        Point = point;
        Normal = normal;
        Depth = depth;
        RelativeVelocity = relativeVelocity;
        ApproachSpeed = approachSpeed;
        Impulse = impulse;
    }

    /// @note native 通知に相手が無い場合は null。通知後に失効した参照は null へ置き換えない。
    [ApiContract(Read = "any", Purity = ApiPurity.MANAGED, Lifetime = "snapshot_with_non_owning_scene_entity_reference", Failure = "may_be_null; destroyed_reference_remains_non_null_and_IsAlive_is_false")]
    public GameObject? Other { get; }
    /// @note ワールド位置 [m]。
    [ApiContract(Units = "m", Coordinate = "world", Read = "any", Purity = ApiPurity.MANAGED, Lifetime = "immutable_collision_snapshot", Failure = "stored_snapshot_read; ApproachSpeed_and_Impulse_are_meaningful_on_Enter_only")]
    public Vector3 Point { get; }
    /// @note 自分から相手へ向くワールド法線。反対側への通知では符号が反転する。
    [ApiContract(Units = "unit_direction", Coordinate = "world_from_self_to_other", Read = "any", Purity = ApiPurity.MANAGED, Lifetime = "immutable_collision_snapshot", Failure = "stored_snapshot_read; ApproachSpeed_and_Impulse_are_meaningful_on_Enter_only")]
    public Vector3 Normal { get; }
    /// @note 侵入深さ [m]。
    [ApiContract(Units = "m", Coordinate = "not_applicable", Read = "any", Purity = ApiPurity.MANAGED, Lifetime = "immutable_collision_snapshot", Failure = "stored_snapshot_read; ApproachSpeed_and_Impulse_are_meaningful_on_Enter_only")]
    public float Depth { get; }
    /// @note 接触点での相対速度 [m/s]。角速度の寄与を含み、反対側への通知では符号が反転する。
    [ApiContract(Units = "m/s", Coordinate = "world", Read = "any", Purity = ApiPurity.MANAGED, Lifetime = "immutable_collision_snapshot", Failure = "stored_snapshot_read; ApproachSpeed_and_Impulse_are_meaningful_on_Enter_only")]
    public Vector3 RelativeVelocity { get; }
    /// @note 接近速度 [m/s]。正は接近。Enter のみ意味を持ち、Stay / Exit は native の零値を渡す。
    [ApiContract(Units = "m/s", Coordinate = "not_applicable", Read = "any", Purity = ApiPurity.MANAGED, Lifetime = "immutable_collision_snapshot", Failure = "stored_snapshot_read; ApproachSpeed_and_Impulse_are_meaningful_on_Enter_only")]
    public float ApproachSpeed { get; }
    /// @note 解決時の法線インパルス。Enter のみ意味を持ち、Stay / Exit は native の零値を渡す。
    [ApiContract(Units = "N*s", Coordinate = "not_applicable", Read = "any", Purity = ApiPurity.MANAGED, Lifetime = "immutable_collision_snapshot", Failure = "stored_snapshot_read; ApproachSpeed_and_Impulse_are_meaningful_on_Enter_only")]
    public float Impulse { get; }
}
