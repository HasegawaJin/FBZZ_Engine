/// @file    RandomStreamTests.cs
/// @brief   Native と同じ固定 vector、全 int 範囲、棄却と default state の検証。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using Fbzz;

namespace Fbzz.ManagedTests;
internal static class RandomStreamTests
{
    internal static void Run()
    {
        ulong[] words = [0x47E4CE4B896CDD1DUL, 0xABCFA6A8E079651DUL, 0xB9D10D8FEB731F57UL,
            0x4DB418A0BB1B019DUL, 0x0E6199B04D5AA600UL, 0xC8674BCB42E3AAD9UL];
        int[] full = [158129436, 1618568476, 1802706774, 991625628, -849697281, -1025266984];
        RandomStream raw = new(1), range = new(1);
        for (int index = 0; index < words.Length; index++) {
            Program.Check(raw.NextUInt64() == words[index], "Native/SDK raw random vector must match.");
            Program.Check(range.Range(int.MinValue, int.MaxValue) == full[index], "Full int range must use widened unbiased native-compatible arithmetic.");
        }
        RandomStream minimum = new(0x98D76A164D99A710UL), maximum = new(0x13184FE17DCE7927UL);
        Program.Check(minimum.Range(int.MinValue, int.MaxValue) == int.MinValue
            && maximum.Range(int.MinValue, int.MaxValue) == int.MaxValue, "Both full-range integer endpoints must be reachable.");
        RandomStream rejected = new(0xA8D395BE4B19CCE8UL);
        Program.Check(rejected.Range(int.MinValue, int.MaxValue) == 1635061397
            && rejected.State == 0x68F06B73AF7E230EUL, "The tail word UINT64_MAX must be rejected and state must advance twice.");
        RandomStream zero = new(0), implicitDefault = default, explicitDefault = new();
        ulong zeroWord = zero.NextUInt64();
        Program.Check(zeroWord == implicitDefault.NextUInt64() && zeroWord == explicitDefault.NextUInt64() && zeroWord != 0,
            "Default, zero seed and parameterless state must start at the same nonzero stream.");
        RandomStream copied = raw; _ = raw.NextUInt64();
        Program.Check(copied.State != raw.State, "Value copies must be independent streams.");
        ulong unchanged = copied.State;
        Program.Check(copied.Range(int.MaxValue, int.MaxValue) == int.MaxValue && copied.Range(2f, 2f) == 2f && copied.State == unchanged,
            "Equal endpoints must not consume state.");
        Program.Throws<ArgumentException>(() => copied.Range(1, -1));
        Program.Throws<ArgumentException>(() => copied.Range(float.NaN, 1f));
        Program.Throws<ArgumentOutOfRangeException>(() => copied.Chance(1.1f));
        Program.Check(copied.State == unchanged, "Rejected managed arguments must not advance the stream.");
        RandomStream finite = new(1), adjacent = new(1);
        Program.Check(float.IsFinite(finite.Range(-float.MaxValue, float.MaxValue))
            && adjacent.Range(1f, MathF.BitIncrement(1f)) == 1f, "Finite float endpoints must not overflow or include the upper endpoint by rounding.");
        RandomStream probability = new(1);
        Program.Check(!probability.Chance(0f) && probability.Chance(1f), "Probability endpoints must be exact.");
    }
}
