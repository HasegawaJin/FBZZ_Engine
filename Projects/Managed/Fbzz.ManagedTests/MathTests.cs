/// @file    MathTests.cs
/// @brief   Host に依存しない数学 API の通常値・境界値・非有限値の検証。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Fbzz;

namespace Fbzz.ManagedTests;

internal static class MathTests
{
    internal static void Run()
    {
        Program.Check(ScriptApiRuntime.Current is null, "Pure mathematical values must work without a Host callback scope.");
        VectorContracts();
        NormalizationContracts();
        ScalarContracts();
        SimdLaneContracts();
    }

    static void VectorContracts()
    {
        Vector3 a = new(2f, -3f, 4f);
        Vector3 b = new(-5f, 6f, -7f);
        Near(Vector3.Dot(a, b), -56f, "Dot must preserve signed component contributions.");
        Vector3 cross = Vector3.Cross(a, b);
        Near(cross, new Vector3(-3f, -6f, -3f), "Cross must preserve the native component orientation.");
        Near(Vector3.Dot(cross, a), 0f, "Cross must be orthogonal to its first finite input.");
        Near(Vector3.Dot(cross, b), 0f, "Cross must be orthogonal to its second finite input.");
        Near(Vector3.Cross(Vector3.RIGHT, Vector3.UP), Vector3.FORWARD, "The basis cross product must agree with the native axes.");

        Vector3 start = new(2f, -4f, 8f);
        Vector3 end = new(6f, 4f, -8f);
        Near(Vector3.Lerp(start, end, 0f), start, "Finite Lerp must preserve the first endpoint.");
        Near(Vector3.Lerp(start, end, 1f), end, "Finite Lerp must preserve the second endpoint.");
        Near(Vector3.Lerp(start, end, 0.25f), new Vector3(3f, -2f, 4f), "Vector Lerp must interpolate each component.");
        Near(Vector3.Lerp(start, end, -0.5f), new Vector3(0f, -8f, 16f), "Vector Lerp must extrapolate below zero.");
        Near(Vector3.Lerp(start, end, 1.5f), new Vector3(8f, 8f, -16f), "Vector Lerp must extrapolate above one.");
        Near(-start, new Vector3(-2f, 4f, -8f), "Unary negation must preserve component signs.");
        Near(start / 2f, new Vector3(1f, -2f, 4f), "Division must divide each component by the supplied scalar.");

        Vector3 dividedByZero = new Vector3(1f, 0f, -1f) / 0f;
        Program.Check(float.IsPositiveInfinity(dividedByZero.X) && float.IsNaN(dividedByZero.Y) && float.IsNegativeInfinity(dividedByZero.Z),
            "Vector zero division must retain native Infinity and NaN results without a fallback or exception.");
        Vector3 nonFiniteEndpoint = Vector3.Lerp(Vector3.ZERO, new Vector3(float.PositiveInfinity, 0f, 0f), 0f);
        Program.Check(float.IsNaN(nonFiniteEndpoint.X) && nonFiniteEndpoint.Y == 0f && nonFiniteEndpoint.Z == 0f,
            "Lerp must not shortcut endpoints and hide native non-finite arithmetic.");
        Vector3 nan = new(float.NaN, 0f, 0f);
        Program.Check(nan.Equals(new Vector3(float.NaN, 0f, 0f)), "The existing component-wise C# Equals semantics must remain unchanged.");
    }

    static void NormalizationContracts()
    {
        Vector3 fallback = new(9f, -2f, 7f);
        Near(new Vector3(3f, 4f, 0f).NormalizedOr(fallback), new Vector3(0.6f, 0.8f, 0f), "Ordinary normalization must produce a unit direction.");
        Program.Check(Vector3.ZERO.NormalizedOr(fallback).Equals(fallback), "Zero direction must return the supplied fallback without normalizing it.");
        Program.Check(new Vector3(Mathf.EPSILON * 0.5f, 0f, 0f).NormalizedOr(fallback).Equals(fallback),
            "A direction shorter than EPSILON must return the supplied fallback.");
        Near(new Vector3(Mathf.EPSILON, 0f, 0f).NormalizedOr(fallback), Vector3.RIGHT,
            "A direction at EPSILON must normalize rather than take the strict fallback branch.");

        Vector3 nonFiniteFallback = new(float.NaN, float.PositiveInfinity, float.NegativeInfinity);
        Program.Check(Vector3.ZERO.NormalizedOr(nonFiniteFallback).Equals(nonFiniteFallback), "The fallback itself must not gain a finite-value constraint.");
        Vector3 nanDirection = new Vector3(float.NaN, 1f, 0f).NormalizedOr(fallback);
        Program.Check(float.IsNaN(nanDirection.X) && float.IsNaN(nanDirection.Y) && float.IsNaN(nanDirection.Z),
            "NaN length must propagate through native component division instead of selecting a fallback.");
        Vector3 infiniteDirection = new Vector3(float.PositiveInfinity, 1f, 0f).NormalizedOr(fallback);
        Program.Check(float.IsNaN(infiniteDirection.X) && infiniteDirection.Y == 0f && infiniteDirection.Z == 0f,
            "Infinite length must retain native Infinity division behavior.");
        Program.Check(new Vector3(float.MaxValue, 0f, 0f).NormalizedOr(fallback).Equals(Vector3.ZERO),
            "Float32 length overflow must not silently switch to double precision normalization.");
    }

    static void ScalarContracts()
    {
        Program.Check(BitConverter.SingleToInt32Bits(Mathf.PI) == 0x40490FDB
            && BitConverter.SingleToInt32Bits(Mathf.TWO_PI) == 0x40C90FDB
            && BitConverter.SingleToInt32Bits(Mathf.HALF_PI) == 0x3FC90FDB && Mathf.EPSILON == 1e-6f,
            "Published constants must retain the native float32 values and normalization threshold.");
        Near(Mathf.Clamp(-3f, -2f, 2f), -2f, "Clamp must bound the lower side.");
        Near(Mathf.Clamp(3f, -2f, 2f), 2f, "Clamp must bound the upper side.");
        Near(Mathf.Clamp(0.5f, -2f, 2f), 0.5f, "Clamp must retain values inside the interval.");
        Near(Mathf.Clamp(3f, 2f, 2f), 2f, "A zero-width valid clamp interval must return its boundary.");
        Near(Mathf.Clamp01(-3f), 0f, "Clamp01 must bound below zero.");
        Near(Mathf.Clamp01(3f), 1f, "Clamp01 must bound above one.");
        Program.Check(float.IsNaN(Mathf.Clamp01(float.NaN)), "Clamp must retain a NaN input through native comparisons.");
        Near(Mathf.Clamp(float.PositiveInfinity, -2f, 2f), 2f, "Positive infinity must clamp to a finite upper boundary.");
        Near(Mathf.Clamp(float.NegativeInfinity, -2f, 2f), -2f, "Negative infinity must clamp to a finite lower boundary.");
        float negativeZero = BitConverter.Int32BitsToSingle(int.MinValue);
        Program.Check(BitConverter.SingleToInt32Bits(Mathf.Clamp01(negativeZero)) == int.MinValue, "Clamp must retain the sign of an in-range zero.");

        Near(Mathf.Lerp(-2f, 6f, 0f), -2f, "Finite scalar Lerp must preserve its first endpoint.");
        Near(Mathf.Lerp(-2f, 6f, 1f), 6f, "Finite scalar Lerp must preserve its second endpoint.");
        Near(Mathf.Lerp(-2f, 6f, 0.25f), 0f, "Scalar Lerp must interpolate.");
        Near(Mathf.Lerp(-2f, 6f, -0.5f), -6f, "Scalar Lerp must extrapolate below zero.");
        Near(Mathf.Lerp(-2f, 6f, 1.5f), 10f, "Scalar Lerp must extrapolate above one.");
        Program.Check(float.IsNaN(Mathf.Lerp(1f, float.PositiveInfinity, 0f)), "Scalar Lerp must retain non-finite arithmetic at endpoints.");

        Near(Mathf.ToRadians(180f), Mathf.PI, "Degrees to radians must use the native angle factor.");
        Near(Mathf.ToRadians(-90f), -Mathf.HALF_PI, "Negative degrees must retain their orientation.");
        Near(Mathf.ToDegrees(Mathf.PI), 180f, "Radians to degrees must use the native angle factor.");
        Near(Mathf.ToDegrees(Mathf.HALF_PI), 90f, "Quarter-turn conversion must return ninety degrees.");
        Program.Check(float.IsNaN(Mathf.ToRadians(float.NaN)) && float.IsPositiveInfinity(Mathf.ToRadians(float.PositiveInfinity))
            && float.IsNegativeInfinity(Mathf.ToDegrees(float.NegativeInfinity)), "Angle conversions must preserve native non-finite arithmetic.");
    }

    /// @note Check all public components against independent scalar expressions, including cancellation, zero signs, overflow and non-finite arithmetic.
    static void SimdLaneContracts()
    {
        Program.Check(Marshal.SizeOf<Vector3>() == 12 && Marshal.OffsetOf<Vector3>(nameof(Vector3.Z)).ToInt32() == 8,
            "SIMD must not pad or reorder the twelve-byte public Vector3.");
        float negativeZero = BitConverter.Int32BitsToSingle(int.MinValue);
        float payload = BitConverter.Int32BitsToSingle(0x7fc12345);
        Vector3[] values = [new(2f, -3f, 4f), new(-5f, 6f, -7f), new(16777216f, 1f, -16777216f),
            new(1.0000001f, -1.0000002f, 0.33333334f), new(Mathf.EPSILON * 0.5f, Mathf.EPSILON, -Mathf.EPSILON),
            new(float.MaxValue, float.Epsilon, -float.MaxValue), new(0f, negativeZero, 0f),
            new(negativeZero, 0f, negativeZero), new(float.PositiveInfinity, 1f, float.NegativeInfinity),
            new(payload, -2f, negativeZero)];
        float[] scales = [0f, negativeZero, 0.33333334f, -2f, 1.5f, float.PositiveInfinity, payload];
        Vector3 fallback = new(payload, negativeZero, 9f);
        foreach (Vector3 first in values) {
            Equivalent(-first, new Vector3(-first.X, -first.Y, -first.Z), "negation");
            Equivalent(first.NormalizedOr(fallback), ScalarNormalizedOr(first, fallback), "normalization");
            Equivalent(first.LengthSquared, ScalarDot(first, first), "length squared");
            foreach (float scale in scales) {
                Equivalent(first * scale, ScalarScale(first, scale), "scale");
                Equivalent(scale * first, ScalarScale(first, scale), "left scale");
                Equivalent(first / scale, ScalarDivide(first, scale), "division");
            }
            foreach (Vector3 second in values) {
                Equivalent(first + second, ScalarAdd(first, second), "addition");
                Equivalent(first - second, ScalarSubtract(first, second), "subtraction");
                Equivalent(Vector3.Cross(first, second), ScalarCross(first, second), "cross");
                Equivalent(SimdVectorDot(first, second), ScalarDot(first, second), "ordered dot");
                foreach (float factor in new[] { -0.5f, 0f, 0.33333334f, 1f, 1.5f }) {
                    Equivalent(Vector3.Lerp(first, second, factor), ScalarLerp(first, second, factor), "lerp");
                    Vector3 expected = ScalarDivide(ScalarLerp(ScalarCross(first, second), ScalarAdd(first, second), factor), -2f);
                    Equivalent(SimdVectorPipeline(first, second, factor, -2f), expected, "composed public SIMD math");
                }
            }
        }
        Program.Check(BitConverter.SingleToInt32Bits((-new Vector3(payload, negativeZero, 0f)).X)
            == (BitConverter.SingleToInt32Bits(payload) ^ int.MinValue), "Unary SIMD negation must retain the NaN payload and flip only its sign.");
        Vector3 recovered = Vector3.ZERO.NormalizedOr(fallback);
        Program.Check(BitConverter.SingleToInt32Bits(recovered.X) == BitConverter.SingleToInt32Bits(payload)
            && BitConverter.SingleToInt32Bits(recovered.Y) == int.MinValue, "The fallback must retain all original payload and signed-zero bits.");
        Equivalent(SimdVectorDot(new Vector3(16777216f, 1f, -16777216f), Vector3.ONE), 0f, "left-associated cancellation");
        Program.Check(ScriptApiRuntime.Current is null, "Explicit SIMD must remain pure managed math without a Host scope.");
    }

    /// @note Stable no-inline entry for root's JIT disassembly; every result component reaches the scalar contract assertions.
    [MethodImpl(MethodImplOptions.NoInlining)]
    internal static Vector3 SimdVectorPipeline(Vector3 first, Vector3 second, float factor, float divisor)
        => Vector3.Lerp(Vector3.Cross(first, second), first + second, factor) / divisor;

    /// @note Keep the public dot call visible to JIT diagnostics without folding constant inputs into a result.
    [MethodImpl(MethodImplOptions.NoInlining)]
    internal static float SimdVectorDot(Vector3 first, Vector3 second) => Vector3.Dot(first, second);

    /// @note Scalar references retain the pre-SIMD component evaluation and avoid all production arithmetic operators.
    static Vector3 ScalarAdd(Vector3 a, Vector3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    static Vector3 ScalarSubtract(Vector3 a, Vector3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    static Vector3 ScalarScale(Vector3 a, float scale) => new(a.X * scale, a.Y * scale, a.Z * scale);
    static Vector3 ScalarDivide(Vector3 a, float divisor) => new(a.X / divisor, a.Y / divisor, a.Z / divisor);
    static Vector3 ScalarCross(Vector3 a, Vector3 b) => new(a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X);
    static Vector3 ScalarLerp(Vector3 a, Vector3 b, float t) => new(a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t);
    [MethodImpl(MethodImplOptions.NoInlining)]
    static float ScalarDot(Vector3 a, Vector3 b)
    {
        float x = a.X * b.X, y = a.Y * b.Y, z = a.Z * b.Z;
        return (x + y) + z;
    }
    static Vector3 ScalarNormalizedOr(Vector3 value, Vector3 fallback)
    {
        float length = MathF.Sqrt(ScalarDot(value, value));
        return length < Mathf.EPSILON ? fallback : ScalarDivide(value, length);
    }
    /// @note Finite/infinite values and signed zeros must match bits; arithmetic NaN payload choice is not normalized or prescribed.
    static void Equivalent(float actual, float expected, string operation)
        => Program.Check(float.IsNaN(expected) ? float.IsNaN(actual) : BitConverter.SingleToInt32Bits(actual) == BitConverter.SingleToInt32Bits(expected),
            $"SIMD {operation} must preserve scalar float32 results: actual={actual}, expected={expected}.");
    static void Equivalent(Vector3 actual, Vector3 expected, string operation)
    { Equivalent(actual.X, expected.X, operation + ".X"); Equivalent(actual.Y, expected.Y, operation + ".Y"); Equivalent(actual.Z, expected.Z, operation + ".Z"); }

    static void Near(float actual, float expected, string message)
        => Program.Check(float.IsFinite(actual) && MathF.Abs(actual - expected) <= 1e-6f, message);

    static void Near(Vector3 actual, Vector3 expected, string message)
        => Program.Check(actual.IsFinite && MathF.Abs(actual.X - expected.X) <= 1e-6f
            && MathF.Abs(actual.Y - expected.Y) <= 1e-6f && MathF.Abs(actual.Z - expected.Z) <= 1e-6f, message);
}
