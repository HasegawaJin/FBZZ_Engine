/// @file    QuaternionBatchTests.cs
/// @brief   Cross-agent SIMD rotation parity, alias safety and generated API contracts.
/// @author  Hasegawa Jin
/// @date    2026-10-20
using System;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class QuaternionBatchTests
{
    internal static void Run()
    {
        LookRotationMatchesScalarAcrossBranchesAndTails();
        SlerpMatchesScalarForMixedArcsAndNormalization();
        BuffersValidateBeforeWritesAndSupportExactInPlace();
        MetadataPublishesManagedSpanContracts();
        Program.Check(ScriptApiRuntime.Current is null, "Batch math must not create an Engine callback scope.");
    }

    /// @note Four simultaneous basis rotations force trace/X/Y/Z branches; counts cover every scalar tail and nonzero array offset.
    static void LookRotationMatchesScalarAcrossBranchesAndTails()
    {
        float minusZero = BitConverter.Int32BitsToSingle(int.MinValue);
        float nan = BitConverter.Int32BitsToSingle(0x7fc12345);
        Vector3[] directions = [Vector3.FORWARD, -Vector3.FORWARD, -Vector3.FORWARD, Vector3.FORWARD,
            Vector3.ZERO, Vector3.UP, Vector3.RIGHT, new(1, 2, 3), new(minusZero, 0, -1), new(nan, 1, 2),
            new(float.PositiveInfinity, 0, 1), new(float.MaxValue, 1, -2), new(0, 0, 0.0001f),
            new(0, 0, MathF.Sqrt(Mathf.EPSILON)), new(1e-20f, -1e-20f, 0), new(-2, 7, 0.3f)];
        Vector3[] ups = [Vector3.UP, -Vector3.UP, Vector3.UP, -Vector3.UP,
            Vector3.UP, Vector3.UP, Vector3.RIGHT, new(3, 2, -1), new(minusZero, -1, 0), new(1, nan, 0),
            Vector3.UP, Vector3.UP, Vector3.ZERO, Vector3.UP, new(0, 0, 1), new(2, -1, 3)];
        Quaternion guard = new(19, 23, 29, 31);
        for (int count = 0; count <= 35; ++count)
        {
            Vector3[] forward = new Vector3[count + 2], up = new Vector3[count + 2];
            Quaternion[] output = Enumerable.Repeat(guard, count + 2).ToArray();
            for (int i = 0; i < count; ++i) { forward[i + 1] = directions[i % directions.Length]; up[i + 1] = ups[i % ups.Length]; }
            SimdAcrossAgentsLook(forward.AsSpan(1, count), up.AsSpan(1, count), output.AsSpan(1, count));
            for (int i = 0; i < count; ++i) Same(output[i + 1], ScalarLook(forward[i + 1], up[i + 1]), "mixed LookRotation");
            Same(output[0], guard, "leading guard"); Same(output[^1], guard, "trailing guard");
            SimdAcrossAgentsLook(forward.AsSpan(1, count), ReadOnlySpan<Vector3>.Empty, output.AsSpan(1, count));
            for (int i = 0; i < count; ++i) Same(output[i + 1], ScalarLook(forward[i + 1], Vector3.UP), "implicit up");
        }
        uint state = 0x7123f19a;
        Vector3[] randomForward = new Vector3[2051], randomUp = new Vector3[2051];
        Quaternion[] randomOutput = new Quaternion[2051];
        for (int i = 0; i < randomForward.Length; ++i) { randomForward[i] = new(Next(ref state), Next(ref state), Next(ref state)); randomUp[i] = new(Next(ref state), Next(ref state), Next(ref state)); }
        SimdAcrossAgentsLook(randomForward, randomUp, randomOutput);
        for (int i = 0; i < randomForward.Length; ++i) Same(randomOutput[i], ScalarLook(randomForward[i], randomUp[i]), "deterministic LookRotation");
        Program.Check(Marshal.SizeOf<Vector3>() == 12 && Marshal.SizeOf<Quaternion>() == 16, "Batch operations must preserve value ABI.");
    }

    /// @note Scalar golden expressions retain reduction order, near-arc Lerp, unclamped t and the caller's optional second normalization.
    static void SlerpMatchesScalarForMixedArcsAndNormalization()
    {
        float minusZero = BitConverter.Int32BitsToSingle(int.MinValue), nan = BitConverter.Int32BitsToSingle(0x7fc23456);
        Quaternion[] values = [Quaternion.IDENTITY, new(0, 1, 0, 0), new(0, -1, 0, 0), new(1, 0, 0, 0),
            new(0, 0, 0, -1), new(minusZero, 0, minusZero, 1), default,
            new(nan, 0, 0, 1), new(float.PositiveInfinity, 1, 0, 0), new(1e-8f, 2e-8f, -1e-8f, 1),
            new(0.5f, 0.5f, 0.5f, 0.5f), new(-0.5f, 0.5f, -0.5f, 0.5f), new(1e20f, -1e20f, 3, -4)];
        float[] times = [0f, 1f, 0.3f, -0.5f, 2f, minusZero, float.NaN, float.PositiveInfinity];
        Quaternion guard = new(17, 19, 23, 29);
        for (int count = 0; count <= 35; ++count)
        {
            Quaternion[] from = new Quaternion[count], to = new Quaternion[count], output = Enumerable.Repeat(guard, count + 2).ToArray();
            float[] t = new float[count];
            for (int i = 0; i < count; ++i) { from[i] = values[i % values.Length]; to[i] = values[(i * 5 + 1) % values.Length]; t[i] = times[i % times.Length]; }
            foreach (bool normalize in new[] { false, true })
            {
                SimdAcrossAgentsSlerp(from, to, t, output.AsSpan(1, count), normalize);
                for (int i = 0; i < count; ++i) { Quaternion expected = ScalarSlerp(from[i], to[i], t[i]); Same(output[i + 1], normalize ? Norm(expected) : expected, "mixed Slerp"); }
                Same(output[0], guard, "Slerp leading guard"); Same(output[^1], guard, "Slerp trailing guard");
            }
        }
        /// @note All four lanes take the dedicated near-equal fast path, including shortest-arc flips, signed zeros and extrapolation.
        Quaternion[] nearFrom = [Quaternion.IDENTITY, new(minusZero, 0, minusZero, 1), new(0, 1, 0, 0), new(1, 0, 0, 0)];
        Quaternion[] nearTo = [Quaternion.IDENTITY, new(0, minusZero, 0, -1), new(0, 1, 0, 0), new(-1, minusZero, minusZero, minusZero)];
        float[] nearFactors = [-0.5f, 2f, minusZero, 0.3f];
        Quaternion[] nearOutput = new Quaternion[4];
        foreach (bool normalize in new[] { false, true }) {
            SimdAcrossAgentsSlerp(nearFrom, nearTo, nearFactors, nearOutput, normalize);
            for (int i = 0; i < 4; ++i) { Quaternion expected = ScalarSlerp(nearFrom[i], nearTo[i], nearFactors[i]); Same(nearOutput[i], normalize ? Norm(expected) : expected, "uniform near Slerp"); }
        }
        uint state = 0x193dc317;
        Quaternion[] first = new Quaternion[2051], second = new Quaternion[2051], results = new Quaternion[2051];
        float[] factors = new float[2051];
        for (int i = 0; i < first.Length; ++i) { first[i] = Norm(new Quaternion(Next(ref state), Next(ref state), Next(ref state), Next(ref state))); second[i] = Norm(new Quaternion(Next(ref state), Next(ref state), Next(ref state), Next(ref state))); factors[i] = Next(ref state) * 0.2f; }
        foreach (bool normalize in new[] { false, true })
        {
            SimdAcrossAgentsSlerp(first, second, factors, results, normalize);
            for (int i = 0; i < results.Length; ++i) { Quaternion expected = ScalarSlerp(first[i], second[i], factors[i]); Same(results[i], normalize ? Norm(expected) : expected, "deterministic Slerp"); }
        }
    }

    static void BuffersValidateBeforeWritesAndSupportExactInPlace()
    {
        Quaternion guard = new(11, 13, 17, 19);
        Quaternion[] output = Enumerable.Repeat(guard, 9).ToArray();
        Program.Throws<ArgumentException>(() => QuaternionBatch.LookRotation(new Vector3[9], new Vector3[8], output));
        Program.Throws<ArgumentException>(() => QuaternionBatch.LookRotation(new Vector3[10], [], output));
        Program.Throws<ArgumentException>(() => QuaternionBatch.Slerp(new Quaternion[9], new Quaternion[8], new float[9], output));
        Program.Throws<ArgumentException>(() => QuaternionBatch.Slerp(new Quaternion[9], new Quaternion[9], new float[8], output));
        Program.Throws<ArgumentException>(() => QuaternionBatch.Slerp(new Quaternion[10], new Quaternion[10], new float[10], output));
        Program.Throws<ArgumentException>(() => QuaternionBatch.Slerp(output.AsSpan(0, 8), new Quaternion[8], new float[8], output.AsSpan(1, 8)));
        Program.Throws<ArgumentException>(() => QuaternionBatch.Slerp(new Quaternion[8], output.AsSpan(1, 8), new float[8], output.AsSpan(0, 8)));
        Program.Throws<ArgumentException>(() => QuaternionBatch.Slerp(new Quaternion[8], new Quaternion[8], MemoryMarshal.Cast<Quaternion, float>(output).Slice(1, 8), output));
        Program.Throws<ArgumentException>(() => QuaternionBatch.LookRotation(MemoryMarshal.Cast<Quaternion, Vector3>(output).Slice(0, 8), [], output));
        Program.Throws<ArgumentException>(() => QuaternionBatch.LookRotation(new Vector3[8], MemoryMarshal.Cast<Quaternion, Vector3>(output).Slice(1, 8), output));
        foreach (Quaternion value in output) Same(value, guard, "invalid buffers must be transactional");
        Quaternion[] a = Enumerable.Range(0, 9).Select(i => ScalarLook(new Vector3(i + 1, 2, 3), Vector3.UP)).ToArray();
        Quaternion[] b = Enumerable.Range(0, 9).Select(i => ScalarLook(new Vector3(3, i - 4, -1), Vector3.UP)).ToArray();
        float[] t = Enumerable.Range(0, 9).Select(i => i * 0.13f - 0.2f).ToArray();
        Quaternion[] golden = a.Select((value, i) => ScalarSlerp(value, b[i], t[i])).ToArray();
        Quaternion[] exactA = (Quaternion[])a.Clone(), exactB = (Quaternion[])b.Clone();
        QuaternionBatch.Slerp(exactA, b, t, exactA);
        QuaternionBatch.Slerp(a, exactB, t, exactB);
        for (int i = 0; i < a.Length; ++i) { Same(exactA[i], golden[i], "in-place from"); Same(exactB[i], golden[i], "in-place to"); }
    }

    static void MetadataPublishesManagedSpanContracts()
    {
        using JsonDocument document = JsonDocument.Parse(ApiCatalog.Json);
        JsonElement type = document.RootElement.GetProperty("types").EnumerateArray().Single(value => value.GetProperty("name").GetString() == "Fbzz.QuaternionBatch");
        JsonElement[] members = type.GetProperty("members").EnumerateArray().Where(value => value.GetProperty("name").GetString() is "LookRotation" or "Slerp").ToArray();
        Program.Check(members.Length == 2, "Only the two used batch entry points must be published.");
        foreach (JsonElement member in members)
        {
            JsonElement contract = member.GetProperty("contract");
            Program.Check(member.GetProperty("signature").GetString()!.Contains("System.Span<", StringComparison.Ordinal)
                && contract.GetProperty("pure").GetBoolean()
                && contract.GetProperty("allowedCallbacks").GetProperty("call")[0].GetString() == "any"
                && contract.GetProperty("failureBehavior").GetProperty("contract").GetString()!.Contains("before_output_changes", StringComparison.Ordinal),
                "Generated batch contracts must retain spans, pure managed availability and transactional validation.");
        }
    }

    /// @note Stable JIT witness uses actual cross-agent batch implementation; no hardware availability assertion is required.
    [MethodImpl(MethodImplOptions.NoInlining)]
    internal static void SimdAcrossAgentsLook(ReadOnlySpan<Vector3> forward, ReadOnlySpan<Vector3> up, Span<Quaternion> output)
        => QuaternionBatch.LookRotation(forward, up, output);
    [MethodImpl(MethodImplOptions.NoInlining)]
    internal static void SimdAcrossAgentsSlerp(ReadOnlySpan<Quaternion> from, ReadOnlySpan<Quaternion> to, ReadOnlySpan<float> t, Span<Quaternion> output, bool normalize)
        => QuaternionBatch.Slerp(from, to, t, output, normalize);

    static float Next(ref uint state) { state = unchecked(state * 1664525 + 1013904223); return ((state >> 8) * (1f / 16777216f) - 0.5f) * 16f; }
    static float Dot(Vector3 a, Vector3 b) => (a.X * b.X + a.Y * b.Y) + a.Z * b.Z;
    static Vector3 Cross(Vector3 a, Vector3 b) => new(a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X);
    static Vector3 Norm(Vector3 a) { float length = MathF.Sqrt(Dot(a, a)); return length < Mathf.EPSILON ? Vector3.ZERO : new(a.X / length, a.Y / length, a.Z / length); }
    static float Dot(Quaternion a, Quaternion b) => ((a.X * b.X + a.Y * b.Y) + a.Z * b.Z) + a.W * b.W;
    static Quaternion Norm(Quaternion a) { float length = MathF.Sqrt(Dot(a, a)); if (MathF.Abs(length) < Mathf.EPSILON) return Quaternion.IDENTITY; float inverse = 1f / length; return new(a.X * inverse, a.Y * inverse, a.Z * inverse, a.W * inverse); }
    static Quaternion ScalarLook(Vector3 forward, Vector3 up)
    {
        if (MathF.Abs(Dot(forward, forward)) < Mathf.EPSILON) return Quaternion.IDENTITY;
        Vector3 f = Norm(forward), safeUp = up;
        if (MathF.Abs(Dot(Cross(up, f), Cross(up, f))) < Mathf.EPSILON)
            safeUp = MathF.Abs(Dot(Cross(Vector3.RIGHT, f), Cross(Vector3.RIGHT, f))) < Mathf.EPSILON ? Vector3.FORWARD : Vector3.RIGHT;
        Vector3 r = Norm(Cross(safeUp, f)), u = Cross(f, r);
        float trace = (r.X + u.Y) + f.Z;
        if (trace > 0f) { float s = 0.5f / MathF.Sqrt(trace + 1f); return new((u.Z - f.Y) * s, (f.X - r.Z) * s, (r.Y - u.X) * s, 0.25f / s); }
        if (r.X > u.Y && r.X > f.Z) { float s = 2f * MathF.Sqrt(((1f + r.X) - u.Y) - f.Z); return new(0.25f * s, (u.X + r.Y) / s, (f.X + r.Z) / s, (u.Z - f.Y) / s); }
        if (u.Y > f.Z) { float s = 2f * MathF.Sqrt(((1f + u.Y) - r.X) - f.Z); return new((u.X + r.Y) / s, 0.25f * s, (u.Z + f.Y) / s, (f.X - r.Z) / s); }
        float last = 2f * MathF.Sqrt(((1f + f.Z) - r.X) - u.Y);
        return new((f.X + r.Z) / last, (u.Z + f.Y) / last, 0.25f * last, (r.Y - u.X) / last);
    }
    static Quaternion ScalarSlerp(Quaternion a, Quaternion b, float t)
    {
        float dot = Dot(a, b);
        if (dot < 0f) { b = new(-b.X, -b.Y, -b.Z, -b.W); dot = -dot; }
        if (dot > 1f - Mathf.EPSILON) return Norm(new Quaternion(a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t, a.W + (b.W - a.W) * t));
        float theta = MathF.Acos(dot), sine = MathF.Sin(theta);
        float first = MathF.Sin((1f - t) * theta) / sine, second = MathF.Sin(t * theta) / sine;
        return new(a.X * first + b.X * second, a.Y * first + b.Y * second, a.Z * first + b.Z * second, a.W * first + b.W * second);
    }
    static void Same(Quaternion actual, Quaternion expected, string label)
    {
        Same(actual.X, expected.X, label + ".X"); Same(actual.Y, expected.Y, label + ".Y"); Same(actual.Z, expected.Z, label + ".Z"); Same(actual.W, expected.W, label + ".W");
    }
    static void Same(float actual, float expected, string label)
    {
        Program.Check(float.IsNaN(expected) ? float.IsNaN(actual) : BitConverter.SingleToInt32Bits(actual) == BitConverter.SingleToInt32Bits(expected),
            label + " must match scalar float32 bits (NaN arithmetic retains its classification).");
    }
}
