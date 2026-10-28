/// @file    QuaternionMathComponent.cs
/// @brief   純 C# Quaternion の固定入力を native Math と比較する fixture。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using Fbzz;

namespace ManagedFixtures;

public sealed class QuaternionMathComponent : Script
{
    public Vector3[] quaternionXYZ;
    public float[] quaternionW;
    public Vector3[] rotated;
    public float[] scalar;

    public QuaternionMathComponent()
    {
        Quaternion first = Quaternion.FromAxisAngle(Vector3.UP, Mathf.HALF_PI);
        Quaternion second = Quaternion.FromEuler(new Vector3(0.2f, -0.4f, 0.7f));
        Quaternion nonunit = new Quaternion(1f, 2f, 3f, 4f);
        Quaternion[] values =
        [
            Quaternion.IDENTITY, first, second, first * second, nonunit.Normalized(), nonunit.Inverse(), nonunit.Conjugate(),
            Quaternion.LookRotation(new Vector3(1f, 2f, 3f)), Quaternion.LookRotation(Vector3.UP, Vector3.UP),
            Quaternion.LookRotation(Vector3.RIGHT, Vector3.RIGHT), Quaternion.Lerp(first, second, 0.3f),
            Quaternion.Slerp(first, second, 0.3f), Quaternion.Lerp(first, second, 1.5f),
            Quaternion.Slerp(first, second, -0.5f), Quaternion.Slerp(first, new Quaternion(-first.X, -first.Y, -first.Z, -first.W), 0.4f)
        ];
        quaternionXYZ = new Vector3[values.Length];
        quaternionW = new float[values.Length];
        for (int index = 0; index < values.Length; index++)
        {
            Quaternion value = values[index];
            quaternionXYZ[index] = new Vector3(value.X, value.Y, value.Z);
            quaternionW[index] = value.W;
        }
        rotated = [first * Vector3.FORWARD, second * new Vector3(2f, -3f, 4f)];
        scalar = [Quaternion.Dot(first, second), nonunit.Length, nonunit.LengthSquared];
    }
}
