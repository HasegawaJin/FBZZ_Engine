/// @file    ManagedPrimitiveCodec.cs
/// @brief   Exact primitive wire values, enum masks and schema-derived numeric constraints.
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
using System.Globalization;
using System.IO;
using System.Numerics;
using System.Text.Json;
using Fbzz;

namespace Fbzz.ManagedHost;

/// @note Wide integers and decimals cross JSON/JavaScript as canonical strings; char crosses as a UInt16 code unit.
internal static class ManagedPrimitiveCodec
{
    internal static Type NumericType(Type type) => type.IsEnum ? Enum.GetUnderlyingType(type) : type;
    internal static string? Kind(Type type)
    {
        if (type == typeof(int)) return "int"; if (type == typeof(float)) return "float"; if (type == typeof(uint)) return "uint";
        if (type == typeof(bool) || type == typeof(string)) return null;
        if (type == typeof(sbyte)) return "int8"; if (type == typeof(byte)) return "uint8";
        if (type == typeof(short)) return "int16"; if (type == typeof(ushort)) return "uint16";
        if (type == typeof(long)) return "int64"; if (type == typeof(ulong)) return "uint64";
        if (type == typeof(double)) return "double";
        if (type == typeof(decimal)) return "decimal"; if (type == typeof(char)) return "char";
        return type.IsEnum ? "enum" : null;
    }
    internal static bool IsNumeric(Type type) => Kind(type) is not null;
    internal static bool StringEncoded(Type type) => NumericType(type) == typeof(long) || NumericType(type) == typeof(ulong) || NumericType(type) == typeof(decimal);
    /// @note Canonical decimal values normalize scale and signed zero while retaining their exact mathematical value.
    static string DecimalText(decimal value)
    {
        if (value == 0m) return "0";
        return value.ToString("0.############################", CultureInfo.InvariantCulture);
    }
    internal static object WireValue(object value, Type type)
    {
        if (type.IsEnum) { if (!ScriptFieldCodec.ValidEnum(type, value)) throw new InvalidDataException("Unknown enum value or Flags bits."); type = NumericType(type); value = Convert.ChangeType(value, type, CultureInfo.InvariantCulture); }
        if (type == typeof(long)) return ((long)value).ToString(CultureInfo.InvariantCulture);
        if (type == typeof(ulong)) return ((ulong)value).ToString(CultureInfo.InvariantCulture);
        if (type == typeof(decimal)) return DecimalText((decimal)value);
        if (type == typeof(char)) return (ushort)(char)value;
        if (value is float single && !float.IsFinite(single) || value is double real && !double.IsFinite(real)) throw new InvalidDataException("Primitive numbers must be finite.");
        return value;
    }
    internal static void Write(Utf8JsonWriter writer, object value, Type type)
    {
        /// @note Preserve the existing common typed path; enum classification and wide-value conversion are unnecessary per array element.
        if (type == typeof(int)) { writer.WriteNumberValue((int)value); return; }
        if (type == typeof(uint)) { writer.WriteNumberValue((uint)value); return; }
        if (type == typeof(float)) { float number = (float)value; if (!float.IsFinite(number)) throw new InvalidDataException("Float must be finite."); writer.WriteNumberValue(number); return; }
        object wire = WireValue(value, type);
        if (wire is string text) { writer.WriteStringValue(text); return; }
        if (wire is float single) writer.WriteNumberValue(single);
        else if (wire is double real) writer.WriteNumberValue(real);
        else if (wire is uint unsigned) writer.WriteNumberValue(unsigned);
        else writer.WriteNumberValue(Convert.ToInt64(wire, CultureInfo.InvariantCulture));
    }
    internal static object Read(JsonElement value, Type type)
    {
        try { return ReadExact(value, type); }
        catch (Exception exception) when (exception is OverflowException or FormatException or InvalidOperationException) {
            throw new InvalidDataException($"Invalid {type.Name} primitive value: {exception.Message}", exception);
        }
    }
    static object ReadExact(JsonElement value, Type type)
    {
        if (type == typeof(int)) return value.GetInt32();
        if (type == typeof(uint)) return value.GetUInt32();
        if (type == typeof(float)) { float number = value.GetSingle(); if (!float.IsFinite(number)) throw new InvalidDataException("Float must be finite."); return number; }
        if (type.IsEnum) { object enumeration = Enum.ToObject(type, Read(value, NumericType(type))); if (!ScriptFieldCodec.ValidEnum(type, enumeration)) throw new InvalidDataException("Unknown enum value or Flags bits."); return enumeration; }
        if (type == typeof(long) || type == typeof(ulong) || type == typeof(decimal)) {
            if (value.ValueKind != JsonValueKind.String) throw new InvalidDataException($"{type.Name} requires a canonical invariant decimal string.");
            string text = value.GetString()!; object parsed;
            if (type == typeof(long)) parsed = long.Parse(text, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture);
            else if (type == typeof(ulong)) parsed = ulong.Parse(text, NumberStyles.None, CultureInfo.InvariantCulture);
            else parsed = decimal.Parse(text, NumberStyles.AllowLeadingSign | NumberStyles.AllowDecimalPoint, CultureInfo.InvariantCulture);
            if ((string)WireValue(parsed, type) != text) throw new InvalidDataException($"{type.Name} string is not canonical or cannot be represented exactly.");
            return parsed;
        }
        if (value.ValueKind != JsonValueKind.Number) throw new InvalidDataException($"{type.Name} requires a JSON number.");
        if (type == typeof(double)) { double number = value.GetDouble(); if (!double.IsFinite(number)) throw new InvalidDataException("Double must be finite."); return number; }
        long integer = value.GetInt64();
        checked {
            if (type == typeof(sbyte)) return (sbyte)integer; if (type == typeof(byte)) return (byte)integer;
            if (type == typeof(short)) return (short)integer; if (type == typeof(ushort)) return (ushort)integer;
            if (type == typeof(char)) return (char)(ushort)integer;
        }
        throw new InvalidDataException($"Unsupported primitive: {type.FullName}.");
    }
    internal static ulong Bits(object value, Type type)
    {
        Type scalar = NumericType(type);
        if (scalar == typeof(ulong)) return Convert.ToUInt64(value, CultureInfo.InvariantCulture);
        if (scalar == typeof(uint)) return Convert.ToUInt32(value, CultureInfo.InvariantCulture);
        if (scalar == typeof(ushort)) return Convert.ToUInt16(value, CultureInfo.InvariantCulture);
        if (scalar == typeof(byte)) return Convert.ToByte(value, CultureInfo.InvariantCulture);
        int width = scalar == typeof(sbyte) ? 8 : scalar == typeof(short) ? 16 : scalar == typeof(int) ? 32 : 64;
        return unchecked((ulong)Convert.ToInt64(value, CultureInfo.InvariantCulture)) & (width == 64 ? ulong.MaxValue : (1UL << width) - 1);
    }

    /// @note Exact rationals prevent range comparisons from rounding an integer/decimal bound through binary floating point.
    readonly record struct Exact(BigInteger Numerator, BigInteger Denominator) : IComparable<Exact>
    {
        public int CompareTo(Exact other) => (Numerator * other.Denominator).CompareTo(other.Numerator * Denominator);
        internal static Exact Of(object value)
        {
            if (value is string text) {
                int dot = text.IndexOf('.'); string digits = dot < 0 ? text : text.Remove(dot, 1);
                BigInteger numerator = BigInteger.Parse(digits, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture);
                return new(numerator, BigInteger.Pow(10, dot < 0 ? 0 : text.Length - dot - 1));
            }
            if (value is decimal dec) {
                int[] bits = decimal.GetBits(dec); BigInteger number = (uint)bits[0] | (BigInteger)(uint)bits[1] << 32 | (BigInteger)(uint)bits[2] << 64;
                return new(bits[3] < 0 ? -number : number, BigInteger.Pow(10, (bits[3] >> 16) & 255));
            }
            if (value is float || value is double) {
                double number = Convert.ToDouble(value, CultureInfo.InvariantCulture);
                if (!double.IsFinite(number)) throw new InvalidDataException("Range bounds and values must be finite.");
                ulong bits = unchecked((ulong)BitConverter.DoubleToInt64Bits(number)); int exponent = (int)((bits >> 52) & 2047);
                BigInteger significand = bits & ((1UL << 52) - 1); if (exponent != 0) significand += BigInteger.One << 52;
                int shift = (exponent == 0 ? -1022 : exponent - 1023) - 52;
                if ((bits >> 63) != 0) significand = -significand;
                return shift < 0 ? new(significand, BigInteger.One << -shift) : new(significand << shift, BigInteger.One);
            }
            if (value is char code) return new(code, BigInteger.One);
            Type type = value.GetType(); if (type.IsEnum) value = Convert.ChangeType(value, NumericType(type), CultureInfo.InvariantCulture);
            return new(BigInteger.Parse(Convert.ToString(value, CultureInfo.InvariantCulture)!, CultureInfo.InvariantCulture), BigInteger.One);
        }
        internal BigInteger Ceiling() { BigInteger result = BigInteger.DivRem(Numerator, Denominator, out BigInteger remainder); return remainder > 0 ? result + 1 : result; }
        internal BigInteger Floor() { BigInteger result = BigInteger.DivRem(Numerator, Denominator, out BigInteger remainder); return remainder < 0 ? result - 1 : result; }
    }
    static (object Minimum, object Maximum) Limits(Type type)
    {
        type = NumericType(type);
        if (type == typeof(sbyte)) return (sbyte.MinValue, sbyte.MaxValue); if (type == typeof(byte)) return (byte.MinValue, byte.MaxValue);
        if (type == typeof(short)) return (short.MinValue, short.MaxValue); if (type == typeof(ushort) || type == typeof(char)) return (ushort.MinValue, ushort.MaxValue);
        if (type == typeof(int)) return (int.MinValue, int.MaxValue); if (type == typeof(uint)) return (uint.MinValue, uint.MaxValue);
        if (type == typeof(long)) return (long.MinValue, long.MaxValue); if (type == typeof(ulong)) return (ulong.MinValue, ulong.MaxValue);
        if (type == typeof(float)) return (float.MinValue, float.MaxValue); if (type == typeof(double)) return (double.MinValue, double.MaxValue);
        if (type == typeof(decimal)) return (decimal.MinValue, decimal.MaxValue);
        throw new InvalidDataException("ValueRange requires a numeric primitive.");
    }
    internal static bool WithinRange(object value, ValueRangeAttribute range)
    { Exact number = Exact.Of(value); return number.CompareTo(Exact.Of(range.Minimum)) >= 0 && number.CompareTo(Exact.Of(range.Maximum)) <= 0; }
    internal static (object Minimum, object Maximum) Range(Type type, ValueRangeAttribute range)
    {
        type = NumericType(type); if (!IsNumeric(type)) throw new InvalidDataException("ValueRange requires a numeric primitive.");
        Exact minimum = Exact.Of(range.Minimum), maximum = Exact.Of(range.Maximum);
        if (minimum.CompareTo(maximum) > 0) throw new InvalidDataException("ValueRange bounds must be ordered.");
        var limits = Limits(type);
        if (maximum.CompareTo(Exact.Of(limits.Minimum)) < 0 || minimum.CompareTo(Exact.Of(limits.Maximum)) > 0) throw new InvalidDataException("ValueRange contains no representable value.");
        if (minimum.CompareTo(Exact.Of(limits.Minimum)) < 0) minimum = Exact.Of(limits.Minimum);
        if (maximum.CompareTo(Exact.Of(limits.Maximum)) > 0) maximum = Exact.Of(limits.Maximum);
        object lower, upper;
        if (type == typeof(float) || type == typeof(double)) {
            double a = Approximate(minimum), b = Approximate(maximum);
            if (type == typeof(float)) {
                float first = (float)a, last = (float)b;
                if (!float.IsFinite(first)) first = first < 0 ? float.MinValue : float.MaxValue;
                if (!float.IsFinite(last)) last = last < 0 ? float.MinValue : float.MaxValue;
                if (Exact.Of(first).CompareTo(minimum) < 0) first = float.BitIncrement(first);
                if (Exact.Of(last).CompareTo(maximum) > 0) last = float.BitDecrement(last);
                lower = first; upper = last;
            } else {
                if (Exact.Of(a).CompareTo(minimum) < 0) a = double.BitIncrement(a);
                if (Exact.Of(b).CompareTo(maximum) > 0) b = double.BitDecrement(b);
                lower = a; upper = b;
            }
        } else if (type == typeof(decimal)) {
            if (range.Minimum is not string || range.Maximum is not string) throw new InvalidDataException("Decimal ValueRange requires invariant-string bounds.");
            lower = decimal.Parse((string)range.Minimum, NumberStyles.AllowLeadingSign | NumberStyles.AllowDecimalPoint, CultureInfo.InvariantCulture);
            upper = decimal.Parse((string)range.Maximum, NumberStyles.AllowLeadingSign | NumberStyles.AllowDecimalPoint, CultureInfo.InvariantCulture);
            if (Exact.Of(lower).CompareTo(minimum) != 0 || Exact.Of(upper).CompareTo(maximum) != 0) throw new InvalidDataException("Decimal range bounds must be exactly representable.");
        } else {
            BigInteger first = minimum.Ceiling(), last = maximum.Floor();
            if (first > last) throw new InvalidDataException("ValueRange contains no representable integer.");
            lower = Integral(first, type); upper = Integral(last, type);
        }
        if (Exact.Of(lower).CompareTo(Exact.Of(upper)) > 0) throw new InvalidDataException("ValueRange contains no representable value.");
        return (lower, upper);
    }
    static double Approximate(Exact value)
    {
        if (value.Numerator.IsZero) return 0;
        int numeratorShift = Math.Max(0, (int)BigInteger.Abs(value.Numerator).GetBitLength() - 54);
        int denominatorShift = Math.Max(0, (int)value.Denominator.GetBitLength() - 54);
        return Math.ScaleB((double)(value.Numerator >> numeratorShift) / (double)(value.Denominator >> denominatorShift), numeratorShift - denominatorShift);
    }
    static object Integral(BigInteger value, Type type)
    {
        if (type == typeof(char)) return (char)(ushort)value;
        return Convert.ChangeType(value.ToString(CultureInfo.InvariantCulture), type, CultureInfo.InvariantCulture);
    }
    internal static object Default(Type type, ValueRangeAttribute? range = null)
    {
        if (type.IsEnum) {
            object zero = Enum.ToObject(type, range is null ? 0 : Default(NumericType(type), range));
            if (ScriptFieldCodec.ValidEnum(type, zero)) return zero;
            foreach (object candidate in Enum.GetValues(type)) if (range is null || WithinRange(candidate, range)) return candidate;
            throw new InvalidDataException("An enum requires a declared value or valid Flags combination within its schema range.");
        }
        object zeroValue = Integral(BigInteger.Zero, type);
        if (range is null) return zeroValue;
        var bounds = Range(type, range);
        return Exact.Of(zeroValue).CompareTo(Exact.Of(bounds.Minimum)) < 0 ? bounds.Minimum : Exact.Of(zeroValue).CompareTo(Exact.Of(bounds.Maximum)) > 0 ? bounds.Maximum : zeroValue;
    }
}
