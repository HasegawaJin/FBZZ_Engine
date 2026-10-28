/// @file    Vector3.cs
/// @brief   C ABI へ渡せる DirectX 左手系の値型ベクトル。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;
using System.Runtime.Intrinsics;

namespace Fbzz;

/// @note 成分は座標単位の値。位置はメートル、速度はメートル毎秒、前方は native Math と同じ +Z。
[StructLayout(LayoutKind.Sequential)]
public struct Vector3 : IEquatable<Vector3>
{
    public float X;
    public float Y;
    public float Z;

    public Vector3(float x, float y, float z) { X = x; Y = y; Z = z; }
    public static readonly Vector3 ZERO = new(0f, 0f, 0f);
    public static readonly Vector3 ONE = new(1f, 1f, 1f);
    public static readonly Vector3 RIGHT = new(1f, 0f, 0f);
    public static readonly Vector3 UP = new(0f, 1f, 0f);
    public static readonly Vector3 FORWARD = new(0f, 0f, 1f);
    public readonly bool IsFinite => float.IsFinite(X) && float.IsFinite(Y) && float.IsFinite(Z);
    public readonly float LengthSquared => Dot(this, this);
    public readonly float Length => MathF.Sqrt(LengthSquared);

    /// @note 長さが EPSILON 未満なら fallback を無変更で返す。非有限値も native と同じ float32 演算で扱う。
    /// @see Math/Vector3.hpp NormalizedOr の退化入力と評価順。
    /// @see https://github.com/Unity-Technologies/UnityCsReference/blob/master/Runtime/Export/Math/Vector3.cs Normalize の managed 数値計算。
    public readonly Vector3 NormalizedOr(Vector3 fallback)
    {
        float length = Length;
        if (length < Mathf.EPSILON)
            return fallback;
        return this / length;
    }

    /// @see Math/Vector3.hpp Dot の float32 評価順。
    /// @see https://github.com/Unity-Technologies/UnityCsReference/blob/master/Runtime/Export/Math/Vector3.cs Dot の C# 公開形。
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float Dot(Vector3 a, Vector3 b)
    {
        Vector128<float> products = Vector128.Multiply(a.AsVector128(), b.AsVector128());
        /// @note Keep the original left-associated float32 sum; the unused fourth lane never joins the reduction.
        return (products.GetElement(0) + products.GetElement(1)) + products.GetElement(2);
    }

    /// @see Math/Vector3.hpp Cross の成分順。
    /// @see https://github.com/Unity-Technologies/UnityCsReference/blob/master/Runtime/Export/Math/Vector3.cs Cross の C# 公開形。
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Vector3 Cross(Vector3 a, Vector3 b)
    {
        Vector128<float> first = a.AsVector128(), second = b.AsVector128();
        Vector128<float> left = Vector128.Multiply(Vector128.Shuffle(first, Vector128.Create(1, 2, 0, 3)),
            Vector128.Shuffle(second, Vector128.Create(2, 0, 1, 3)));
        Vector128<float> right = Vector128.Multiply(Vector128.Shuffle(first, Vector128.Create(2, 0, 1, 3)),
            Vector128.Shuffle(second, Vector128.Create(1, 2, 0, 3)));
        return FromVector128(Vector128.Subtract(left, right));
    }

    /// @note t は clamp せず、区間外の値も外挿する。
    /// @see Math/Vector3.hpp Lerp の float32 評価順。
    /// @see https://github.com/Unity-Technologies/UnityCsReference/blob/master/Runtime/Export/Math/Vector3.cs LerpUnclamped の C# 計算。
    [ApiContract(ParameterUnits = new[] { "t=ratio" }, Failure = "float32_propagation; t_is_not_clamped")]
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Vector3 Lerp(Vector3 a, Vector3 b, float t)
    {
        Vector128<float> first = a.AsVector128();
        Vector128<float> difference = Vector128.Subtract(b.AsVector128(), first);
        Vector128<float> scaled = Vector128.Multiply(difference, Vector128.Create(t, t, t, 0f));
        return FromVector128(Vector128.Add(first, scaled));
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Vector3 operator +(Vector3 a, Vector3 b) => FromVector128(Vector128.Add(a.AsVector128(), b.AsVector128()));
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Vector3 operator -(Vector3 a, Vector3 b) => FromVector128(Vector128.Subtract(a.AsVector128(), b.AsVector128()));
    /// @note Bitwise sign reversal preserves signed zero and NaN payloads without replacing negation with subtraction.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Vector3 operator -(Vector3 value) => FromVector128(Vector128.Xor(value.AsVector128(), Vector128.Create(int.MinValue).AsSingle()));
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Vector3 operator *(Vector3 value, float scale) => FromVector128(Vector128.Multiply(value.AsVector128(), Vector128.Create(scale, scale, scale, 0f)));
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Vector3 operator *(float scale, Vector3 value) => value * scale;
    /// @note 零除算を置き換えず、native の成分ごとの float32 除算と同じ NaN / Infinity を返す。
    /// @see Math/Vector3.hpp operator/。
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Vector3 operator /(Vector3 value, float scale) => FromVector128(Vector128.Divide(value.AsVector128(), Vector128.Create(scale, scale, scale, 1f)));

    /// @note Pack three explicit fields rather than loading sixteen bytes from the twelve-byte public value; the JIT supplies a software fallback if needed.
    /// @see https://learn.microsoft.com/en-us/dotnet/api/system.runtime.intrinsics.vector128.create?view=net-10.0 Explicit four-lane construction.
    /// @see https://learn.microsoft.com/en-us/dotnet/api/system.runtime.intrinsics.vector128?view=net-10.0 JIT SIMD intrinsics; use separate Multiply/Add rather than fused estimates.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    readonly Vector128<float> AsVector128() => Vector128.Create(X, Y, Z, 0f);

    /// @note The padding lane is never stored into public memory or included in a reduction.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static Vector3 FromVector128(Vector128<float> value) => new(value.GetElement(0), value.GetElement(1), value.GetElement(2));
    public readonly bool Equals(Vector3 other) => X.Equals(other.X) && Y.Equals(other.Y) && Z.Equals(other.Z);
    public override readonly bool Equals(object? other) => other is Vector3 value && Equals(value);
    public override readonly int GetHashCode() => HashCode.Combine(X, Y, Z);
}
