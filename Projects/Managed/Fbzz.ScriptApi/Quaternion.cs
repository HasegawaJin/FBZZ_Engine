/// @file    Quaternion.cs
/// @brief   Native Math と同じ成分順と演算順を持つ回転の値型。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;
using System.Runtime.Intrinsics;

namespace Fbzz;

/// @note XYZ が虚部、W が実部。既定値 default は零で、回転の初期値には IDENTITY を使う。
/// @see Math/Quaternion.hpp と Math/Quaternion.cpp の float32 演算。
[StructLayout(LayoutKind.Sequential)]
public struct Quaternion : IEquatable<Quaternion>
{
    public float X;
    public float Y;
    public float Z;
    public float W;

    public Quaternion(float x, float y, float z, float w) { X = x; Y = y; Z = z; W = w; }
    public static readonly Quaternion IDENTITY = new(0f, 0f, 0f, 1f);
    public readonly bool IsFinite => float.IsFinite(X) && float.IsFinite(Y) && float.IsFinite(Z) && float.IsFinite(W);
    public readonly float LengthSquared => Dot(this, this);
    public readonly float Length => MathF.Sqrt(LengthSquared);

    /// @param angleRadians 回転角 [rad]。軸は内部で正規化する。
    /// @see Math/Quaternion.cpp FromAxisAngle。
    [ApiContract(Units = "unitless_quaternion_xyzw", ParameterUnits = new[] { "angleRadians=rad" })]
    public static Quaternion FromAxisAngle(Vector3 axis, float angleRadians)
    {
        float half = angleRadians * 0.5f;
        float sine = MathF.Sin(half);
        float cosine = MathF.Cos(half);
        Vector3 normal = axis.NormalizedOr(Vector3.ZERO);
        return new Quaternion(normal.X * sine, normal.Y * sine, normal.Z * sine, cosine);
    }

    /// @note eulerRadians は [rad]。YXZ 内因順 Qy * Qx * Qz で、Unity の Euler 度数契約とは異なる。
    /// @see Math/Quaternion.cpp FromEuler。
    [ApiContract(Units = "unitless_quaternion_xyzw", ParameterUnits = new[] { "eulerRadians=rad" }, Coordinate = "intrinsic_YXZ_Qy_Qx_Qz")]
    public static Quaternion FromEuler(Vector3 eulerRadians)
    {
        float cx = MathF.Cos(eulerRadians.X * 0.5f), sx = MathF.Sin(eulerRadians.X * 0.5f);
        float cy = MathF.Cos(eulerRadians.Y * 0.5f), sy = MathF.Sin(eulerRadians.Y * 0.5f);
        float cz = MathF.Cos(eulerRadians.Z * 0.5f), sz = MathF.Sin(eulerRadians.Z * 0.5f);
        return new Quaternion(cy * sx * cz + sy * cx * sz, -cy * sx * sz + sy * cx * cz,
            cy * cx * sz - sy * sx * cz, cy * cx * cz + sy * sx * sz);
    }

    public static Quaternion LookRotation(Vector3 forward) => LookRotation(forward, Vector3.UP);

    /// @note +Z を forward へ向ける。長さ二乗が EPSILON 未満の forward は IDENTITY、平行な up は native と同じ代替軸を使う。
    /// @see Math/Quaternion.cpp LookRotation。
    public static Quaternion LookRotation(Vector3 forward, Vector3 up)
    {
        if (MathF.Abs(forward.LengthSquared) < Mathf.EPSILON) return IDENTITY;
        Vector3 f = forward.NormalizedOr(Vector3.ZERO);
        Vector3 safeUp = up;
        if (MathF.Abs(Vector3.Cross(up, f).LengthSquared) < Mathf.EPSILON)
            safeUp = MathF.Abs(Vector3.Cross(Vector3.RIGHT, f).LengthSquared) < Mathf.EPSILON ? Vector3.FORWARD : Vector3.RIGHT;
        Vector3 r = Vector3.Cross(safeUp, f).NormalizedOr(Vector3.ZERO);
        Vector3 u = Vector3.Cross(f, r);
        float trace = r.X + u.Y + f.Z;
        if (trace > 0f)
        {
            float scale = 0.5f / MathF.Sqrt(trace + 1f);
            return new Quaternion((u.Z - f.Y) * scale, (f.X - r.Z) * scale, (r.Y - u.X) * scale, 0.25f / scale);
        }
        if (r.X > u.Y && r.X > f.Z)
        {
            float scale = 2f * MathF.Sqrt(1f + r.X - u.Y - f.Z);
            return new Quaternion(0.25f * scale, (u.X + r.Y) / scale, (f.X + r.Z) / scale, (u.Z - f.Y) / scale);
        }
        if (u.Y > f.Z)
        {
            float scale = 2f * MathF.Sqrt(1f + u.Y - r.X - f.Z);
            return new Quaternion((u.X + r.Y) / scale, 0.25f * scale, (u.Z + f.Y) / scale, (f.X - r.Z) / scale);
        }
        float lastScale = 2f * MathF.Sqrt(1f + f.Z - r.X - u.Y);
        return new Quaternion((f.X + r.Z) / lastScale, (u.Z + f.Y) / lastScale, 0.25f * lastScale, (r.Y - u.X) / lastScale);
    }

    /// @pre 有効な回転の長さは EPSILON 以上。
    /// @note 退化入力は native の回復値 IDENTITY を返す。NaN / Infinity の通常演算は置き換えない。
    /// @see Math/Quaternion.cpp Normalized。
    public readonly Quaternion Normalized()
    {
        float length = Length;
        if (MathF.Abs(length) < Mathf.EPSILON) return IDENTITY;
        float inverse = 1f / length;
        return FromVector128(Vector128.Multiply(AsVector128(), Vector128.Create(inverse)));
    }

    /// @note Flip only imaginary sign bits; signed zero and NaN payloads remain ordinary negation results.
    public readonly Quaternion Conjugate() => FromVector128(Vector128.Xor(AsVector128(),
        Vector128.Create(int.MinValue, int.MinValue, int.MinValue, 0).AsSingle()));

    /// @pre 有効な回転の長さ二乗は EPSILON 以上。
    /// @note 正規化とは退化閾値が異なる。退化入力は native の回復値 IDENTITY を返す。
    /// @see Math/Quaternion.cpp Inverse。
    public readonly Quaternion Inverse()
    {
        float squared = LengthSquared;
        if (MathF.Abs(squared) < Mathf.EPSILON) return IDENTITY;
        float inverse = 1f / squared;
        return FromVector128(Vector128.Multiply(Conjugate().AsVector128(), Vector128.Create(inverse)));
    }

    /// @note t を clamp せず、成分補間を正規化する。符号を反転する短弧補間は Slerp を使う。
    /// @see Math/Quaternion.cpp Lerp。
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Quaternion Lerp(Quaternion a, Quaternion b, float t)
    {
        Vector128<float> first = a.AsVector128();
        Vector128<float> difference = Vector128.Subtract(b.AsVector128(), first);
        Vector128<float> scaled = Vector128.Multiply(difference, Vector128.Create(t));
        return FromVector128(Vector128.Add(first, scaled)).Normalized();
    }

    /// @pre a と b は正規化済み。
    /// @note t を clamp せず、dot が負なら b の符号を反転して短弧を使う。
    /// @see Math/Quaternion.cpp Slerp。
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Quaternion Slerp(Quaternion a, Quaternion b, float t)
    {
        float dot = Dot(a, b);
        Quaternion end = b;
        if (dot < 0f) { end = new Quaternion(-b.X, -b.Y, -b.Z, -b.W); dot = -dot; }
        if (dot > 1f - Mathf.EPSILON) return Lerp(a, end, t);
        float theta = MathF.Acos(dot);
        float sine = MathF.Sin(theta);
        float first = MathF.Sin((1f - t) * theta) / sine;
        float second = MathF.Sin(t * theta) / sine;
        Vector128<float> startWeight = Vector128.Multiply(a.AsVector128(), Vector128.Create(first));
        Vector128<float> endWeight = Vector128.Multiply(end.AsVector128(), Vector128.Create(second));
        return FromVector128(Vector128.Add(startWeight, endWeight));
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float Dot(Quaternion a, Quaternion b)
    {
        Vector128<float> products = Vector128.Multiply(a.AsVector128(), b.AsVector128());
        /// @note Keep the scalar X/Y/Z/W reduction order instead of a reassociated horizontal dot intrinsic.
        return ((products.GetElement(0) + products.GetElement(1)) + products.GetElement(2)) + products.GetElement(3);
    }
    /// @see Math/Quaternion.hpp Hamilton 積の成分順。
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Quaternion operator *(Quaternion a, Quaternion b)
    {
        Vector128<float> values = b.AsVector128();
        Vector128<float> first = Vector128.Multiply(Vector128.Create(a.W), values);
        Vector128<float> second = Vector128.Multiply(Vector128.Create(a.X), Vector128.Shuffle(values, Vector128.Create(3, 2, 1, 0)));
        Vector128<float> third = Vector128.Multiply(Vector128.Create(a.Y), Vector128.Shuffle(values, Vector128.Create(2, 3, 0, 1)));
        Vector128<float> fourth = Vector128.Multiply(Vector128.Create(a.Z), Vector128.Shuffle(values, Vector128.Create(1, 0, 3, 2)));
        /// @note Each lane keeps its original left-associated Add/Subtract sequence, including finite signed-zero results; no FMA or dot reduction is used.
        Vector128<float> result = Vector128.ConditionalSelect(Vector128.Create(-1, 0, -1, 0).AsSingle(),
            Vector128.Add(first, second), Vector128.Subtract(first, second));
        result = Vector128.ConditionalSelect(Vector128.Create(-1, -1, 0, 0).AsSingle(),
            Vector128.Add(result, third), Vector128.Subtract(result, third));
        result = Vector128.ConditionalSelect(Vector128.Create(0, -1, -1, 0).AsSingle(),
            Vector128.Add(result, fourth), Vector128.Subtract(result, fourth));
        return FromVector128(result);
    }
    /// @pre rotation は正規化済み。
    /// @see Math/Quaternion.hpp q*v*q^-1 の展開形。
    public static Vector3 operator *(Quaternion rotation, Vector3 value)
    {
        Vector3 vector = new(rotation.X, rotation.Y, rotation.Z);
        Vector3 twiceCross = Vector3.Cross(vector, value) * 2f;
        return value + twiceCross * rotation.W + Vector3.Cross(vector, twiceCross);
    }

    /// @note Public XYZW fields remain sixteen-byte sequential values; only local registers carry SIMD state.
    /// @see https://learn.microsoft.com/en-us/dotnet/api/system.runtime.intrinsics.vector128?view=net-10.0 JIT-accelerated lane arithmetic with a software fallback.
    /// @see https://learn.microsoft.com/en-us/dotnet/api/system.runtime.intrinsics.vector128.conditionalselect?view=net-10.0 Bitwise lane selection preserves the Hamilton Add/Subtract order.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    readonly Vector128<float> AsVector128() => Vector128.Create(X, Y, Z, W);

    /// @note No SIMD register or additional field crosses the public value/ABI boundary.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static Quaternion FromVector128(Vector128<float> value) => new(value.GetElement(0), value.GetElement(1), value.GetElement(2), value.GetElement(3));

    /// @note Equals は Vector3 と同じ成分の厳密な値比較で、native の NearlyEqual 演算子とは異なる。
    public readonly bool Equals(Quaternion other) => X.Equals(other.X) && Y.Equals(other.Y) && Z.Equals(other.Z) && W.Equals(other.W);
    public override readonly bool Equals(object? other) => other is Quaternion value && Equals(value);
    public override readonly int GetHashCode() => HashCode.Combine(X, Y, Z, W);
}
