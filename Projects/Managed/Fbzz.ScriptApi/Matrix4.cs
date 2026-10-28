/// @file    Matrix4.cs
/// @brief   Native Math::Matrix4 と同じ行優先16要素の独立した値。
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
namespace Fbzz;
/// @note Shader property へ渡す行優先の値。空間・単位はその shader の宣言に従う。
public readonly struct Matrix4
{
    readonly float m_00, m_01, m_02, m_03, m_10, m_11, m_12, m_13, m_20, m_21, m_22, m_23, m_30, m_31, m_32, m_33;
    public Matrix4(ReadOnlySpan<float> rowMajor)
    {
        if (rowMajor.Length != 16) throw new ArgumentException("Matrix4 requires exactly sixteen row-major elements.", nameof(rowMajor));
        m_00 = rowMajor[0]; m_01 = rowMajor[1]; m_02 = rowMajor[2]; m_03 = rowMajor[3];
        m_10 = rowMajor[4]; m_11 = rowMajor[5]; m_12 = rowMajor[6]; m_13 = rowMajor[7];
        m_20 = rowMajor[8]; m_21 = rowMajor[9]; m_22 = rowMajor[10]; m_23 = rowMajor[11];
        m_30 = rowMajor[12]; m_31 = rowMajor[13]; m_32 = rowMajor[14]; m_33 = rowMajor[15];
    }
    public static Matrix4 Identity => new([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
    public float this[int row, int column]
    {
        get {
            if ((uint)row > 3 || (uint)column > 3) throw new ArgumentOutOfRangeException(nameof(row));
            return (row * 4 + column) switch { 0 => m_00, 1 => m_01, 2 => m_02, 3 => m_03,
                4 => m_10, 5 => m_11, 6 => m_12, 7 => m_13, 8 => m_20, 9 => m_21, 10 => m_22, 11 => m_23,
                12 => m_30, 13 => m_31, 14 => m_32, _ => m_33 };
        }
    }
    /// @return 必要16要素。短い buffer は Written=0、出力未変更。余りも保持する。
    public CollectionReadResult CopyTo(Span<float> buffer)
    {
        if (buffer.Length < 16) return new(16, 0);
        for (int row = 0; row < 4; row++) for (int column = 0; column < 4; column++) buffer[row * 4 + column] = this[row, column];
        return new(16, 16);
    }
}
