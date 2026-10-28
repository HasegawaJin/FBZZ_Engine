/// @file    MathParityComponent.cs
/// @brief   通常の C# helper と native Math の数値・境界・配置契約を比較する fixture。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System.Runtime.InteropServices;
using Fbzz;

namespace ManagedFixtures;

static class MathParity
{
    /// @note Engine 操作や callback scope を必要としない純計算。
    /// @see https://learn.microsoft.com/en-us/dotnet/api/system.numerics.vector3.dot Vector3.Dot の内積定義。
    /// @see https://learn.microsoft.com/en-us/dotnet/api/system.numerics.vector3.cross Vector3.Cross の外積定義。
    public static (float Dot, Vector3 Cross, Vector3 Lerp, Vector3 Normalized) Vectors(
        Vector3 first, Vector3 second, float factor, Vector3 fallback)
    {
        return (Vector3.Dot(first, second), Vector3.Cross(first, second),
            Vector3.Lerp(first, second, factor), first.NormalizedOr(fallback));
    }

    /// @note Lerp の factor は [0,1] へ制限せず、Clamp の区間は minimum <= maximum。
    public static (float Clamp, float Clamp01, float Lerp, float Radians, float Degrees) Scalars(
        float value, float minimum, float maximum, float first, float second, float factor,
        float degrees, float radians)
    {
        return (Mathf.Clamp(value, minimum, maximum), Mathf.Clamp01(value),
            Mathf.Lerp(first, second, factor), Mathf.ToRadians(degrees), Mathf.ToDegrees(radians));
    }

    public static float ConstructorDot()
    {
        return Vector3.Dot(new Vector3(2f, -3f, 4f), new Vector3(-5f, 6f, -7f));
    }
}

class MathParityComponent : Script
{
    public Vector3 first = new Vector3(1.25f, -2.5f, 3.75f);
    public Vector3 second = new Vector3(-4.5f, 5.25f, -6f);
    public Vector3 fallback = new Vector3(2f, -3f, 4f);
    public float factor = 0.25f;
    public float scalarValue = -4f;
    public float minimum = -2f;
    public float maximum = 3f;
    public float scalarFirst = -2f;
    public float scalarSecond = 6f;
    public float degrees = -450f;
    public float radians = -Mathf.PI * 2.5f;
    public float constructorDot = MathParity.ConstructorDot();
    public float pi = Mathf.PI;
    public float twoPi = Mathf.TWO_PI;
    public float halfPi = Mathf.HALF_PI;
    public float epsilon = Mathf.EPSILON;
    public float degreesToRadians = Mathf.DEG2RAD;
    public float radiansToDegrees = Mathf.RAD2DEG;
    /// @see https://learn.microsoft.com/en-us/dotnet/api/system.runtime.interopservices.structlayoutattribute Sequential の成分配置契約。
    public int vectorSize = Marshal.SizeOf<Vector3>();
    public int vectorXOffset = (int)Marshal.OffsetOf<Vector3>(nameof(Vector3.X));
    public int vectorYOffset = (int)Marshal.OffsetOf<Vector3>(nameof(Vector3.Y));
    public int vectorZOffset = (int)Marshal.OffsetOf<Vector3>(nameof(Vector3.Z));
    public float dot;
    public float length;
    public float lengthSquared;
    public Vector3 cross;
    public Vector3 sum;
    public Vector3 difference;
    public Vector3 negated;
    public Vector3 scaled;
    public Vector3 divided;
    public Vector3 vectorLerp;
    public Vector3 normalized;
    public float scalarClamp;
    public float scalarClamp01;
    public float scalarLerp;
    public float toRadians;
    public float toDegrees;

    protected override void Update()
    {
        (dot, cross, vectorLerp, normalized) = MathParity.Vectors(first, second, factor, fallback);
        (scalarClamp, scalarClamp01, scalarLerp, toRadians, toDegrees) = MathParity.Scalars(
            scalarValue, minimum, maximum, scalarFirst, scalarSecond, factor, degrees, radians);
        length = first.Length;
        lengthSquared = first.LengthSquared;
        sum = first + second;
        difference = first - second;
        negated = -first;
        scaled = first * 2f;
        divided = first / 2f;
    }
}
