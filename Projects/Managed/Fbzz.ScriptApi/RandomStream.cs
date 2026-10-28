/// @file    RandomStream.cs
/// @brief   Native と同じ状態遷移・偏りのない整数範囲を持つ独立乱数列。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;

namespace Fbzz;

/// @note 値コピーは同じ位置から再開する独立 stream。暗号用途には使わず、他 Script の draw 回数から独立する。
public struct RandomStream
{
    const ulong DEFAULT_STATE = 0x9E3779B97F4A7C15UL;
    ulong m_state;
    public RandomStream() { m_state = DEFAULT_STATE; }
    public RandomStream(ulong seed) { m_state = seed == 0 ? DEFAULT_STATE : seed; }
    /// @note default(struct) と zero seed/state は同じ非ゼロ既定値へ正規化する。
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Write = "any", Units = "uint64_state", Lifetime = "independent_value_state")]
    public ulong State { readonly get => m_state == 0 ? DEFAULT_STATE : m_state; set => m_state = value == 0 ? DEFAULT_STATE : value; }
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Summary = "Resets this independent stream; zero maps to the fixed nonzero default seed.")]
    public void SetSeed(ulong seed) => State = seed;
    /// @see https://prng.di.unimi.it/ Xorshift64* の非ゼロ状態と乗算出力。
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Returns = "nonzero_uint64_word", Summary = "Advances native-compatible xorshift64* state and returns a nonzero 64-bit word.")]
    public ulong NextUInt64()
    {
        m_state = State;
        m_state ^= m_state >> 12; m_state ^= m_state << 25; m_state ^= m_state >> 27;
        return unchecked(m_state * 0x2545F4914F6CDD1DUL);
    }
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Returns = "float32_in_[0,1)")]
    public float Value() => (NextUInt64() >> 40) * (1f / 16777216f);
    /// @return 両端を含む int。範囲の差・加算は int64 で行い、全 int 範囲を支持する。
    /// @see https://www.pcg-random.org/posts/bounded-rands.html Rejection による modulo bias の除去。
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "reversed_endpoints_throw_without_advancing_state", Returns = "inclusive_[min,max]; equal_endpoints_do_not_advance", Summary = "Unbiased integer sampling over the nonzero xorshift64* word domain; supports both int extremes.")]
    public int Range(int min, int max)
    {
        if (min > max) throw new ArgumentException("Random range endpoints are reversed.", nameof(max));
        if (min == max) return min;
        ulong span = (ulong)((long)max - min) + 1;
        ulong limit = ulong.MaxValue - ulong.MaxValue % span;
        ulong word = NextUInt64();
        while (word > limit) word = NextUInt64();
        return (int)((long)min + (long)((word - 1) % span));
    }
    /// @note double 中間値と上端直前の float 値を native と同じ手順で使う。
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "nonfinite_or_reversed_endpoints_throw_without_advancing", Returns = "[min,max); equal_endpoints_do_not_advance")]
    public float Range(float min, float max)
    {
        if (!float.IsFinite(min) || !float.IsFinite(max) || min > max) throw new ArgumentException("Random range requires finite ordered endpoints.", nameof(max));
        if (min == max) return min;
        float value = (float)((double)min + ((double)max - min) * Value());
        return value < max ? value : MathF.BitDecrement(max);
    }
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", ParameterUnits = new[] { "probability=ratio_0_to_1" }, Failure = "nonfinite_or_out_of_range_probability_throws_without_advancing")]
    public bool Chance(float probability)
    {
        if (!float.IsFinite(probability) || probability < 0 || probability > 1) throw new ArgumentOutOfRangeException(nameof(probability));
        return Value() < probability;
    }
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Returns = "either_minus_one_or_plus_one")]
    public float Sign() => Value() < 0.5f ? -1f : 1f;
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Coordinate = "unit_circle")]
    public Vector2 InsideUnitCircle()
    {
        for (;;) { float x = Range(-1f, 1f), y = Range(-1f, 1f); if (x * x + y * y <= 1f) return new(x, y); }
    }
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Coordinate = "unit_circle")]
    public Vector2 OnUnitCircle()
    { float angle = Value() * (MathF.PI * 2f); return new(MathF.Cos(angle), MathF.Sin(angle)); }
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Coordinate = "unit_sphere")]
    public Vector3 InsideUnitSphere()
    {
        for (;;) { float x = Range(-1f, 1f), y = Range(-1f, 1f), z = Range(-1f, 1f); if (x * x + y * y + z * z <= 1f) return new(x, y, z); }
    }
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Coordinate = "unit_sphere")]
    public Vector3 OnUnitSphere()
    {
        float theta = Value() * (MathF.PI * 2f), phi = MathF.Acos(1f - 2f * Value());
        float sinPhi = MathF.Sin(phi);
        return new(sinPhi * MathF.Cos(theta), sinPhi * MathF.Sin(theta), MathF.Cos(phi));
    }
}
