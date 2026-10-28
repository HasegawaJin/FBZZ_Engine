/// @file    TransformPoseComponent.cs
/// @brief   実 CoreCLR から対象の回転・拡縮・方向を操作して記録する fixture。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using Fbzz;

namespace ManagedFixtures;

public sealed class TransformPoseComponent : Script
{
    public GameObject target;
    public int operation;
    public Vector3 rotationXYZ = Vector3.ZERO;
    public float rotationW = 1f;
    public Vector3 scale = Vector3.ONE;
    public Vector3 axis = Vector3.UP;
    public float angleDegrees;
    public Vector3 lookTarget = Vector3.ZERO;
    public bool useCached;
    public Vector3 localRotationXYZ;
    public float localRotationW;
    public Vector3 worldRotationXYZ;
    public float worldRotationW;
    public Vector3 localScale;
    public Vector3 forward;
    public Vector3 right;
    public Vector3 up;
    private Transform m_cached;

    protected override void Awake() { m_cached = target != null ? target.Transform : Transform; }

    protected override void Update()
    {
        Transform pose = useCached ? m_cached : target != null ? target.Transform : Transform;
        switch (operation)
        {
            case 1: pose.LocalRotation = new Quaternion(rotationXYZ.X, rotationXYZ.Y, rotationXYZ.Z, rotationW); break;
            case 2: pose.LocalScale = scale; break;
            case 3: pose.Rotate(axis, angleDegrees); break;
            case 4: pose.LookAt(lookTarget); break;
        }
        Quaternion local = pose.LocalRotation;
        Quaternion world = pose.WorldRotation;
        localRotationXYZ = new Vector3(local.X, local.Y, local.Z);
        localRotationW = local.W;
        worldRotationXYZ = new Vector3(world.X, world.Y, world.Z);
        worldRotationW = world.W;
        localScale = pose.LocalScale;
        forward = pose.Forward;
        right = pose.Right;
        up = pose.Up;
    }
}
