/// @file    PrimitiveSettingsComponent.cs
/// @brief   Exact primitive settings across native Inspector, common transactions and scene persistence.
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
using System.Collections.Generic;
using Fbzz;

namespace ManagedFixtures;

[Flags] public enum PrimitiveWideOptions : ulong { NONE = 0, LOW = 1, HIGH = 1UL << 63 }
[Serializable] public sealed class PrimitiveValues
{
    public long signed = long.MinValue;
    public ulong unsigned = ulong.MaxValue;
    public decimal amount = 1.25m;
    public char code = '\ud800';
    public double real = double.MaxValue;
}
public class PrimitiveSettingsComponent : Script
{
    public sbyte signed8 = sbyte.MinValue;
    public byte unsigned8 = byte.MaxValue;
    public short signed16 = short.MinValue;
    public ushort unsigned16 = ushort.MaxValue;
    public long signed = long.MinValue;
    public ulong unsigned = ulong.MaxValue;
    public double real = double.MaxValue;
    public decimal amount = decimal.MaxValue;
    public char code = '\udfff';
    public PrimitiveWideOptions options = PrimitiveWideOptions.HIGH | PrimitiveWideOptions.LOW;
    public long[] signedPath = [long.MinValue, long.MaxValue];
    public List<ulong> unsignedPath = [0, ulong.MaxValue];
    public List<PrimitiveValues> groups = [new()];
    [ValueRange(0, 100)] public float single = 1f;
}
