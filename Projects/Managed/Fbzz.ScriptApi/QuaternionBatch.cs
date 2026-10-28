/// @file    QuaternionBatch.cs
/// @brief   Four independent rotations per SIMD group with caller-owned continuous buffers.
/// @author  Hasegawa Jin
/// @date    2026-10-20
using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Intrinsics;
using V = System.Runtime.Intrinsics.Vector128<float>;

namespace Fbzz;

/// @note Each SIMD lane belongs to a different agent. Public Vector3/Quaternion layouts remain unchanged.
/// @see https://learn.microsoft.com/en-us/dotnet/api/system.runtime.intrinsics.vector128?view=net-10.0 Explicit SIMD arithmetic also has a software fallback.
public static class QuaternionBatch
{
    /// @param forward Continuous directions; their count determines the number of outputs.
    /// @param up Empty uses Vector3.UP for every direction; otherwise supply at least forward.Length values.
    /// @param output Only the first forward.Length elements are written. Input/output byte ranges must not overlap.
    /// @note Matches Quaternion.LookRotation, including its squared-length threshold and parallel-up recovery. No Engine calls or heap allocation occur.
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Units = "unitless_quaternion_xyzw", Coordinate = "same_as_input_forward_positive_Z",
        Failure = "ArgumentException_before_output_changes_on_short_buffer_or_overlap; float32_propagation",
        ParameterUnits = new[] { "forward=direction", "up=direction", "output=unitless_quaternion_xyzw" })]
    public static void LookRotation(ReadOnlySpan<Vector3> forward, ReadOnlySpan<Vector3> up, Span<Quaternion> output)
    {
        int count = forward.Length;
        if ((!up.IsEmpty && up.Length < count) || output.Length < count)
            throw new ArgumentException("Rotation buffers must cover every direction.");
        Span<Quaternion> active = output[..count];
        ReadOnlySpan<byte> destination = MemoryMarshal.AsBytes(active);
        if (MemoryMarshal.AsBytes(forward).Overlaps(destination)
            || (!up.IsEmpty && MemoryMarshal.AsBytes(up[..count]).Overlaps(destination)))
            throw new ArgumentException("Direction buffers must not overlap the rotation output.");
        BatchMathCapture? diagnostics = BatchMathDiagnostics.Capture;
        long started = diagnostics is null ? 0 : Stopwatch.GetTimestamp();
        int i = 0;
        for (; i <= count - 4; i += 4)
        {
            Three f = Pack(forward, i);
            Three u = up.IsEmpty ? new(V.Zero, Vector128.Create(1f), V.Zero) : Pack(up, i);
            Store(LookFour(f, u), active, i);
        }
        for (; i < count; ++i)
            active[i] = Quaternion.LookRotation(forward[i], up.IsEmpty ? Vector3.UP : up[i]);
        diagnostics?.Complete(BatchMathOperation.LOOK_ROTATION, count, started);
    }

    /// @param from Continuous starting rotations; their count determines the number of outputs.
    /// @param to Supply at least from.Length ending rotations, with the same normalization precondition as Quaternion.Slerp.
    /// @param t Supply at least from.Length unclamped interpolation factors.
    /// @param output May start at exactly the same element as from or to; shifted or cross-type overlaps are rejected before writes.
    /// @param normalizeOutput Apply the additional Normalized call used by callers that require it, including after the near-equal Lerp path.
    /// @note Preserves shortest-arc signs, scalar trigonometry and the original left-associated float32 arithmetic.
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Units = "unitless_quaternion_xyzw", Coordinate = "same_as_inputs",
        Failure = "ArgumentException_before_output_changes_on_short_buffer_or_shifted_overlap; float32_propagation; t_is_not_clamped",
        ParameterUnits = new[] { "from=unitless_quaternion_xyzw", "to=unitless_quaternion_xyzw", "t=ratio", "output=unitless_quaternion_xyzw" })]
    public static void Slerp(ReadOnlySpan<Quaternion> from, ReadOnlySpan<Quaternion> to, ReadOnlySpan<float> t,
        Span<Quaternion> output, bool normalizeOutput = false)
    {
        int count = from.Length;
        if (to.Length < count || t.Length < count || output.Length < count)
            throw new ArgumentException("Rotation buffers must cover every starting rotation.");
        Span<Quaternion> active = output[..count];
        if ((from.Overlaps(active, out int fromOffset) && fromOffset != 0)
            || (to[..count].Overlaps(active, out int toOffset) && toOffset != 0)
            || MemoryMarshal.AsBytes(t[..count]).Overlaps(MemoryMarshal.AsBytes(active)))
            throw new ArgumentException("Rotation output permits only exact in-place quaternion inputs.");
        BatchMathCapture? diagnostics = BatchMathDiagnostics.Capture;
        long started = diagnostics is null ? 0 : Stopwatch.GetTimestamp();
        int i = 0;
        for (; i <= count - 4; i += 4)
        {
            Four a = Pack(from, i), b = Pack(to, i);
            V factors = Vector128.Create(t[i], t[i + 1], t[i + 2], t[i + 3]);
            Store(SlerpFour(a, b, factors, normalizeOutput), active, i);
        }
        for (; i < count; ++i)
        {
            Quaternion value = Quaternion.Slerp(from[i], to[i], t[i]);
            active[i] = normalizeOutput ? value.Normalized() : value;
        }
        diagnostics?.Complete(BatchMathOperation.SLERP, count, started);
    }

    /// @note Explicit field gathers never read a fourth float beyond a twelve-byte Vector3, including the final group.
    readonly struct Three(V x, V y, V z) { internal readonly V X = x, Y = y, Z = z; }
    readonly struct Four(V x, V y, V z, V w) { internal readonly V X = x, Y = y, Z = z, W = w; }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static Three Pack(ReadOnlySpan<Vector3> values, int i) => new(
        Vector128.Create(values[i].X, values[i + 1].X, values[i + 2].X, values[i + 3].X),
        Vector128.Create(values[i].Y, values[i + 1].Y, values[i + 2].Y, values[i + 3].Y),
        Vector128.Create(values[i].Z, values[i + 1].Z, values[i + 2].Z, values[i + 3].Z));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static Four Pack(ReadOnlySpan<Quaternion> values, int i) => new(
        Vector128.Create(values[i].X, values[i + 1].X, values[i + 2].X, values[i + 3].X),
        Vector128.Create(values[i].Y, values[i + 1].Y, values[i + 2].Y, values[i + 3].Y),
        Vector128.Create(values[i].Z, values[i + 1].Z, values[i + 2].Z, values[i + 3].Z),
        Vector128.Create(values[i].W, values[i + 1].W, values[i + 2].W, values[i + 3].W));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static void Store(Four value, Span<Quaternion> output, int i)
    {
        output[i] = new(value.X.GetElement(0), value.Y.GetElement(0), value.Z.GetElement(0), value.W.GetElement(0));
        output[i + 1] = new(value.X.GetElement(1), value.Y.GetElement(1), value.Z.GetElement(1), value.W.GetElement(1));
        output[i + 2] = new(value.X.GetElement(2), value.Y.GetElement(2), value.Z.GetElement(2), value.W.GetElement(2));
        output[i + 3] = new(value.X.GetElement(3), value.Y.GetElement(3), value.Z.GetElement(3), value.W.GetElement(3));
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static V Sum3(V x, V y, V z) => Vector128.Add(Vector128.Add(x, y), z);
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static V Squared(Three a) => Sum3(Vector128.Multiply(a.X, a.X), Vector128.Multiply(a.Y, a.Y), Vector128.Multiply(a.Z, a.Z));
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static Three Cross(Three a, Three b) => new(
        Vector128.Subtract(Vector128.Multiply(a.Y, b.Z), Vector128.Multiply(a.Z, b.Y)),
        Vector128.Subtract(Vector128.Multiply(a.Z, b.X), Vector128.Multiply(a.X, b.Z)),
        Vector128.Subtract(Vector128.Multiply(a.X, b.Y), Vector128.Multiply(a.Y, b.X)));

    /// @note Keep MathF.Sqrt per lane, matching the single-value MathF boundary without reciprocal estimates.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static V Root(V a) => Vector128.Create(MathF.Sqrt(a.GetElement(0)), MathF.Sqrt(a.GetElement(1)),
        MathF.Sqrt(a.GetElement(2)), MathF.Sqrt(a.GetElement(3)));
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static Three Normalize(Three a)
    {
        V length = Root(Squared(a));
        V small = Vector128.LessThan(length, Vector128.Create(Mathf.EPSILON));
        return new(Vector128.ConditionalSelect(small, V.Zero, Vector128.Divide(a.X, length)),
            Vector128.ConditionalSelect(small, V.Zero, Vector128.Divide(a.Y, length)),
            Vector128.ConditionalSelect(small, V.Zero, Vector128.Divide(a.Z, length)));
    }
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static Three Select(V mask, Three a, Three b) => new(Vector128.ConditionalSelect(mask, a.X, b.X),
        Vector128.ConditionalSelect(mask, a.Y, b.Y), Vector128.ConditionalSelect(mask, a.Z, b.Z));

    /// @note Masks select each agent's original branch before its square root; mixed trace/X/Y/Z cases share one SIMD group.
    static Four LookFour(Three forward, Three up)
    {
        V epsilon = Vector128.Create(Mathf.EPSILON), one = Vector128.Create(1f), quarter = Vector128.Create(0.25f);
        V degenerate = Vector128.LessThan(Vector128.Abs(Squared(forward)), epsilon);
        if (Vector128.ExtractMostSignificantBits(degenerate) == 15)
            return new(V.Zero, V.Zero, V.Zero, one);
        Three f = Normalize(forward);
        V parallel = Vector128.LessThan(Vector128.Abs(Squared(Cross(up, f))), epsilon);
        Three right = new(one, V.Zero, V.Zero), front = new(V.Zero, V.Zero, one);
        V rightParallel = Vector128.LessThan(Vector128.Abs(Squared(Cross(right, f))), epsilon);
        Three safeUp = Select(parallel, Select(rightParallel, front, right), up);
        Three r = Normalize(Cross(safeUp, f)), u = Cross(f, r);
        V trace = Sum3(r.X, u.Y, f.Z);
        V traceMask = Vector128.GreaterThan(trace, V.Zero);
        if (Vector128.ExtractMostSignificantBits(traceMask) == 15)
        {
            V traceScale = Vector128.Divide(Vector128.Create(0.5f), Root(Vector128.Add(trace, one)));
            return new(Vector128.ConditionalSelect(degenerate, V.Zero, Vector128.Multiply(Vector128.Subtract(u.Z, f.Y), traceScale)),
                Vector128.ConditionalSelect(degenerate, V.Zero, Vector128.Multiply(Vector128.Subtract(f.X, r.Z), traceScale)),
                Vector128.ConditionalSelect(degenerate, V.Zero, Vector128.Multiply(Vector128.Subtract(r.Y, u.X), traceScale)),
                Vector128.ConditionalSelect(degenerate, one, Vector128.Divide(quarter, traceScale)));
        }
        V xMask = Vector128.AndNot(Vector128.BitwiseAnd(Vector128.GreaterThan(r.X, u.Y), Vector128.GreaterThan(r.X, f.Z)), traceMask);
        V yMask = Vector128.AndNot(Vector128.GreaterThan(u.Y, f.Z), Vector128.BitwiseOr(traceMask, xMask));
        V xRoot = Vector128.Subtract(Vector128.Subtract(Vector128.Add(one, r.X), u.Y), f.Z);
        V yRoot = Vector128.Subtract(Vector128.Subtract(Vector128.Add(one, u.Y), r.X), f.Z);
        V zRoot = Vector128.Subtract(Vector128.Subtract(Vector128.Add(one, f.Z), r.X), u.Y);
        V root = Root(Vector128.ConditionalSelect(traceMask, Vector128.Add(trace, one),
            Vector128.ConditionalSelect(xMask, xRoot, Vector128.ConditionalSelect(yMask, yRoot, zRoot))));
        V scale = Vector128.ConditionalSelect(traceMask, Vector128.Divide(Vector128.Create(0.5f), root),
            Vector128.Multiply(Vector128.Create(2f), root));
        V x = Vector128.ConditionalSelect(traceMask, Vector128.Multiply(Vector128.Subtract(u.Z, f.Y), scale),
            Vector128.ConditionalSelect(xMask, Vector128.Multiply(quarter, scale),
                Vector128.Divide(Vector128.ConditionalSelect(yMask, Vector128.Add(u.X, r.Y), Vector128.Add(f.X, r.Z)), scale)));
        V y = Vector128.ConditionalSelect(traceMask, Vector128.Multiply(Vector128.Subtract(f.X, r.Z), scale),
            Vector128.ConditionalSelect(yMask, Vector128.Multiply(quarter, scale),
                Vector128.Divide(Vector128.ConditionalSelect(xMask, Vector128.Add(u.X, r.Y), Vector128.Add(u.Z, f.Y)), scale)));
        V z = Vector128.ConditionalSelect(traceMask, Vector128.Multiply(Vector128.Subtract(r.Y, u.X), scale),
            Vector128.ConditionalSelect(Vector128.BitwiseOr(xMask, yMask),
                Vector128.Divide(Vector128.ConditionalSelect(xMask, Vector128.Add(f.X, r.Z), Vector128.Add(u.Z, f.Y)), scale),
                Vector128.Multiply(quarter, scale)));
        V w = Vector128.Divide(Vector128.ConditionalSelect(traceMask, quarter,
            Vector128.ConditionalSelect(xMask, Vector128.Subtract(u.Z, f.Y),
                Vector128.ConditionalSelect(yMask, Vector128.Subtract(f.X, r.Z), Vector128.Subtract(r.Y, u.X)))), scale);
        return new(Vector128.ConditionalSelect(degenerate, V.Zero, x), Vector128.ConditionalSelect(degenerate, V.Zero, y),
            Vector128.ConditionalSelect(degenerate, V.Zero, z), Vector128.ConditionalSelect(degenerate, one, w));
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static Four Normalize(Four a)
    {
        V length = Root(Vector128.Add(Sum3(Vector128.Multiply(a.X, a.X), Vector128.Multiply(a.Y, a.Y), Vector128.Multiply(a.Z, a.Z)),
            Vector128.Multiply(a.W, a.W)));
        V small = Vector128.LessThan(Vector128.Abs(length), Vector128.Create(Mathf.EPSILON));
        V inverse = Vector128.Divide(Vector128.Create(1f), length);
        return new(Vector128.ConditionalSelect(small, V.Zero, Vector128.Multiply(a.X, inverse)),
            Vector128.ConditionalSelect(small, V.Zero, Vector128.Multiply(a.Y, inverse)),
            Vector128.ConditionalSelect(small, V.Zero, Vector128.Multiply(a.Z, inverse)),
            Vector128.ConditionalSelect(small, Vector128.Create(1f), Vector128.Multiply(a.W, inverse)));
    }

    /// @note Trigonometry and its branch remain scalar per agent; only independent component arithmetic is vectorized across agents.
    static Four SlerpFour(Four a, Four b, V t, bool normalizeOutput)
    {
        V dot = Vector128.Add(Sum3(Vector128.Multiply(a.X, b.X), Vector128.Multiply(a.Y, b.Y), Vector128.Multiply(a.Z, b.Z)),
            Vector128.Multiply(a.W, b.W));
        V negative = Vector128.LessThan(dot, V.Zero);
        V signs = Vector128.BitwiseAnd(negative, Vector128.Create(int.MinValue).AsSingle());
        b = new(Vector128.Xor(b.X, signs), Vector128.Xor(b.Y, signs), Vector128.Xor(b.Z, signs), Vector128.Xor(b.W, signs));
        dot = Vector128.Xor(dot, signs);
        V near = Vector128.GreaterThan(dot, Vector128.Create(1f - Mathf.EPSILON));
        uint nearBits = Vector128.ExtractMostSignificantBits(near);
        if (nearBits == 15)
        {
            Four nearResult = Normalize(new Four(Lerp(a.X, b.X, t), Lerp(a.Y, b.Y, t), Lerp(a.Z, b.Z, t), Lerp(a.W, b.W, t)));
            return normalizeOutput ? Normalize(nearResult) : nearResult;
        }
        Weights(dot.GetElement(0), t.GetElement(0), near.GetElement(0), out float a0, out float b0);
        Weights(dot.GetElement(1), t.GetElement(1), near.GetElement(1), out float a1, out float b1);
        Weights(dot.GetElement(2), t.GetElement(2), near.GetElement(2), out float a2, out float b2);
        Weights(dot.GetElement(3), t.GetElement(3), near.GetElement(3), out float a3, out float b3);
        V first = Vector128.Create(a0, a1, a2, a3), second = Vector128.Create(b0, b1, b2, b3);
        Four result = new(Blend(a.X, b.X, first, second), Blend(a.Y, b.Y, first, second),
            Blend(a.Z, b.Z, first, second), Blend(a.W, b.W, first, second));
        if (nearBits != 0)
        {
            Four lerp = Normalize(new Four(Lerp(a.X, b.X, t), Lerp(a.Y, b.Y, t), Lerp(a.Z, b.Z, t), Lerp(a.W, b.W, t)));
            result = new(Vector128.ConditionalSelect(near, lerp.X, result.X), Vector128.ConditionalSelect(near, lerp.Y, result.Y),
                Vector128.ConditionalSelect(near, lerp.Z, result.Z), Vector128.ConditionalSelect(near, lerp.W, result.W));
        }
        return normalizeOutput ? Normalize(result) : result;
    }
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static V Lerp(V a, V b, V t) => Vector128.Add(a, Vector128.Multiply(Vector128.Subtract(b, a), t));
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static V Blend(V a, V b, V first, V second) => Vector128.Add(Vector128.Multiply(a, first), Vector128.Multiply(b, second));
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    static void Weights(float dot, float t, float near, out float first, out float second)
    {
        if (BitConverter.SingleToInt32Bits(near) != 0) { first = 0f; second = 0f; return; }
        float theta = MathF.Acos(dot), sine = MathF.Sin(theta);
        first = MathF.Sin((1f - t) * theta) / sine;
        second = MathF.Sin(t * theta) / sine;
    }
}
