/// @file    QuaternionTests.cs
/// @brief   純 C# 回転の単位・成分順・退化境界・補間契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;
using Fbzz;

namespace Fbzz.ManagedTests;

internal static class QuaternionTests
{
    internal static void Run()
    {
        Program.Check(Marshal.SizeOf<Quaternion>() == 16 && Marshal.OffsetOf<Quaternion>(nameof(Quaternion.W)).ToInt32() == 12,
            "Quaternion must remain the four-float XYZW value layout.");
        Quaternion yaw = Quaternion.FromAxisAngle(Vector3.UP * 3f, Mathf.HALF_PI);
        Near(yaw * Vector3.FORWARD, Vector3.RIGHT);
        Near(Quaternion.FromEuler(new Vector3(0f, Mathf.HALF_PI, 0f)) * Vector3.FORWARD, Vector3.RIGHT);
        Quaternion pitch = Quaternion.FromAxisAngle(Vector3.RIGHT, 0.4f);
        Quaternion roll = Quaternion.FromAxisAngle(Vector3.FORWARD, -0.3f);
        Quaternion mixed = Quaternion.FromEuler(new Vector3(0.4f, Mathf.HALF_PI, -0.3f));
        Near(mixed * Vector3.FORWARD, (yaw * pitch * roll) * Vector3.FORWARD);
        Quaternion nonunit = new(1f, 2f, 3f, 4f);
        Near(nonunit.LengthSquared, 30f);
        Near(nonunit.Normalized().Length, 1f);
        Near((nonunit * nonunit.Inverse()).W, 1f);
        Program.Check(nonunit.Conjugate().Equals(new Quaternion(-1f, -2f, -3f, 4f)), "Conjugate must preserve the real component.");

        Program.Check(default(Quaternion).Normalized().Equals(Quaternion.IDENTITY)
            && default(Quaternion).Inverse().Equals(Quaternion.IDENTITY), "Degenerate pure values must use the native recovery identity.");
        Program.Check(new Quaternion(Mathf.EPSILON * 0.5f, 0f, 0f, 0f).Normalized().Equals(Quaternion.IDENTITY),
            "Normalization compares length, not length squared.");
        Near(new Quaternion(Mathf.EPSILON, 0f, 0f, 0f).Normalized().X, 1f);
        Program.Check(new Quaternion(0.0001f, 0f, 0f, 0f).Inverse().Equals(Quaternion.IDENTITY),
            "Inverse must use the native squared-length threshold.");
        Program.Check(Quaternion.LookRotation(new Vector3(0f, 0f, 0.0001f)).Equals(Quaternion.IDENTITY),
            "LookRotation must use the native forward squared-length threshold.");
        Near(Quaternion.LookRotation(new Vector3(1f, 2f, 3f)) * Vector3.FORWARD, new Vector3(1f, 2f, 3f).NormalizedOr(Vector3.ZERO));
        Near(Quaternion.LookRotation(Vector3.UP, Vector3.UP) * Vector3.FORWARD, Vector3.UP);
        Near(Quaternion.LookRotation(Vector3.RIGHT, Vector3.RIGHT) * Vector3.FORWARD, Vector3.RIGHT);

        Near(Quaternion.Lerp(Quaternion.IDENTITY, yaw, 0.5f) * Vector3.FORWARD,
            Quaternion.Slerp(Quaternion.IDENTITY, yaw, 0.5f) * Vector3.FORWARD);
        Near(Quaternion.Slerp(Quaternion.IDENTITY, yaw, 2f) * Vector3.FORWARD, -Vector3.FORWARD);
        Quaternion negative = new(-yaw.X, -yaw.Y, -yaw.Z, -yaw.W);
        Near(Quaternion.Slerp(yaw, negative, 0.3f) * Vector3.FORWARD, Vector3.RIGHT);
        Near(Quaternion.Dot(yaw, negative), -1f);
        Program.Check(!new Quaternion(float.NaN, 0f, 0f, 1f).IsFinite
            && float.IsNaN(new Quaternion(float.NaN, 0f, 0f, 1f).Normalized().X), "Pure calculations must retain native non-finite propagation.");
        Program.Check(ScriptApiRuntime.Current is null, "Pure rotation math must not create an Engine callback scope.");
        SimdQuaternionContracts();
    }

    /// @note Scalar golden expressions verify XYZW arithmetic and signed-zero recovery across mixed rotations, extrapolation and non-finite inputs.
    static void SimdQuaternionContracts()
    {
        float negativeZero = BitConverter.Int32BitsToSingle(int.MinValue);
        float payload = BitConverter.Int32BitsToSingle(0x7fc12345);
        Quaternion[] values = [Quaternion.IDENTITY, new(1f, 2f, -3f, 4f), new(-5f, 6f, 7f, -8f),
            new(16777216f, 1f, -16777216f, 2f), new(0f, negativeZero, 0f, negativeZero),
            new(negativeZero, 0f, negativeZero, 0f), new(Mathf.EPSILON * 0.5f, 0f, 0f, 0f),
            new(float.MaxValue, float.Epsilon, -float.MaxValue, 1f),
            new(float.PositiveInfinity, 1f, float.NegativeInfinity, 0f), new(payload, -2f, negativeZero, 1f)];
        Vector3[] vectors = [new(2f, -3f, 4f), new(0f, negativeZero, 0f), new(float.PositiveInfinity, 1f, payload)];
        foreach (Quaternion first in values) {
            Equivalent(first.LengthSquared, ScalarDot(first, first), "length squared");
            Equivalent(first.Normalized(), ScalarNormalized(first), "normalization");
            Equivalent(first.Inverse(), ScalarInverse(first), "inverse");
            Equivalent(first.Conjugate(), new Quaternion(-first.X, -first.Y, -first.Z, first.W), "conjugate");
            foreach (Vector3 vector in vectors) Equivalent(first * vector, ScalarRotate(first, vector), "vector rotation");
            foreach (Quaternion second in values) {
                Equivalent(Quaternion.Dot(first, second), ScalarDot(first, second), "ordered dot");
                Equivalent(SimdQuaternionProduct(first, second), ScalarProduct(first, second), "Hamilton product");
                foreach (float factor in new[] { -0.5f, 0f, 0.33333334f, 1f, 1.5f })
                    Equivalent(Quaternion.Lerp(first, second, factor), ScalarLerp(first, second, factor), "unclamped lerp");
            }
        }
        Quaternion[] rotations = [Quaternion.IDENTITY, Quaternion.FromEuler(new Vector3(0.4f, -1.2f, 0.7f)),
            Quaternion.FromEuler(new Vector3(-0.3f, 0.8f, -1.1f)), new(0f, 0f, 0f, -1f), new(payload, 0f, 0f, 1f)];
        foreach (Quaternion first in rotations) foreach (Quaternion second in rotations)
            foreach (float factor in new[] { -0.5f, 0f, 0.33333334f, 1f, 1.5f })
                Equivalent(SimdQuaternionBlend(first, second, factor), ScalarSlerp(first, second, factor), "shortest-arc slerp");
        Quaternion conjugated = new Quaternion(payload, negativeZero, 0f, payload).Conjugate();
        Program.Check(BitConverter.SingleToInt32Bits(conjugated.X) == (BitConverter.SingleToInt32Bits(payload) ^ int.MinValue)
            && BitConverter.SingleToInt32Bits(conjugated.Y) == 0 && BitConverter.SingleToInt32Bits(conjugated.Z) == int.MinValue
            && BitConverter.SingleToInt32Bits(conjugated.W) == BitConverter.SingleToInt32Bits(payload),
            "SIMD conjugation must flip only XYZ sign bits, preserving the real NaN payload.");
        Equivalent(Quaternion.Dot(new Quaternion(16777216f, 1f, -16777216f, 2f), new Quaternion(1f, 1f, 1f, 1f)), 2f,
            "left-associated cancellation");
        Program.Check(Marshal.SizeOf<Quaternion>() == 16 && Marshal.OffsetOf<Quaternion>(nameof(Quaternion.W)).ToInt32() == 12,
            "SIMD must retain the sixteen-byte XYZW public layout.");
    }

    /// @note Root's JIT diagnostic can inspect packed multiply and ordered lane sums with all four returned components consumed by tests.
    [MethodImpl(MethodImplOptions.NoInlining)]
    internal static Quaternion SimdQuaternionProduct(Quaternion first, Quaternion second) => first * second;

    /// @note Keep a representative Ocean interpolation call visible without changing production API or requiring hardware acceleration in correctness tests.
    [MethodImpl(MethodImplOptions.NoInlining)]
    internal static Quaternion SimdQuaternionBlend(Quaternion first, Quaternion second, float factor) => Quaternion.Slerp(first, second, factor);

    /// @note These references retain the pre-SIMD scalar formulas, including each original Add/Subtract and recovery threshold.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static float ScalarDot(Quaternion a, Quaternion b)
    {
        float x = a.X * b.X, y = a.Y * b.Y, z = a.Z * b.Z, w = a.W * b.W;
        return ((x + y) + z) + w;
    }
    static Quaternion ScalarProduct(Quaternion a, Quaternion b) => new(
        a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y,
        a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
        a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W,
        a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z);
    static Quaternion ScalarNormalized(Quaternion value)
    {
        float length = MathF.Sqrt(ScalarDot(value, value));
        if (MathF.Abs(length) < Mathf.EPSILON) return Quaternion.IDENTITY;
        float inverse = 1f / length;
        return new(value.X * inverse, value.Y * inverse, value.Z * inverse, value.W * inverse);
    }
    static Quaternion ScalarInverse(Quaternion value)
    {
        float squared = ScalarDot(value, value);
        if (MathF.Abs(squared) < Mathf.EPSILON) return Quaternion.IDENTITY;
        float inverse = 1f / squared;
        return new(-value.X * inverse, -value.Y * inverse, -value.Z * inverse, value.W * inverse);
    }
    static Quaternion ScalarLerp(Quaternion a, Quaternion b, float t) => ScalarNormalized(new(
        a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t, a.W + (b.W - a.W) * t));
    static Quaternion ScalarSlerp(Quaternion a, Quaternion b, float t)
    {
        float dot = ScalarDot(a, b);
        Quaternion end = b;
        if (dot < 0f) { end = new(-b.X, -b.Y, -b.Z, -b.W); dot = -dot; }
        if (dot > 1f - Mathf.EPSILON) return ScalarLerp(a, end, t);
        float theta = MathF.Acos(dot), sine = MathF.Sin(theta);
        float first = MathF.Sin((1f - t) * theta) / sine;
        float second = MathF.Sin(t * theta) / sine;
        return new(a.X * first + end.X * second, a.Y * first + end.Y * second,
            a.Z * first + end.Z * second, a.W * first + end.W * second);
    }
    static Vector3 ScalarCross(Vector3 a, Vector3 b) => new(a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X);
    static Vector3 ScalarRotate(Quaternion rotation, Vector3 value)
    {
        Vector3 cross = ScalarCross(new(rotation.X, rotation.Y, rotation.Z), value);
        Vector3 twice = new(cross.X * 2f, cross.Y * 2f, cross.Z * 2f);
        Vector3 last = ScalarCross(new(rotation.X, rotation.Y, rotation.Z), twice);
        return new((value.X + twice.X * rotation.W) + last.X, (value.Y + twice.Y * rotation.W) + last.Y, (value.Z + twice.Z * rotation.W) + last.Z);
    }
    /// @note Check every float bit except the unspecified arithmetic NaN payload; Infinity signs and signed zeros remain exact.
    static void Equivalent(float actual, float expected, string operation)
        => Program.Check(float.IsNaN(expected) ? float.IsNaN(actual) : BitConverter.SingleToInt32Bits(actual) == BitConverter.SingleToInt32Bits(expected),
            $"SIMD Quaternion {operation} must preserve scalar float32 results: actual={actual}, expected={expected}.");
    static void Equivalent(Quaternion actual, Quaternion expected, string operation)
    { Equivalent(actual.X, expected.X, operation + ".X"); Equivalent(actual.Y, expected.Y, operation + ".Y"); Equivalent(actual.Z, expected.Z, operation + ".Z"); Equivalent(actual.W, expected.W, operation + ".W"); }
    static void Equivalent(Vector3 actual, Vector3 expected, string operation)
    { Equivalent(actual.X, expected.X, operation + ".X"); Equivalent(actual.Y, expected.Y, operation + ".Y"); Equivalent(actual.Z, expected.Z, operation + ".Z"); }

    static void Near(float actual, float expected)
        => Program.Check(MathF.Abs(actual - expected) < 0.00002f, $"Quaternion value differs: {actual} != {expected}.");
    static void Near(Vector3 actual, Vector3 expected) { Near(actual.X, expected.X); Near(actual.Y, expected.Y); Near(actual.Z, expected.Z); }
}
