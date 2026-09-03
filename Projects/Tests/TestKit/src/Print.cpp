/// @file    Print.cpp
/// @brief   数学型の PrintTo 実装。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <TestKit/Print.hpp>

#include <Math/Matrix3.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

#include <iomanip>
#include <ostream>

namespace fbzz::math {

namespace {

/// 有効数字 6 桁。float の 7 桁目は演算誤差で揺れるため、そこまで出すと
/// 「見た目が同じなのに落ちている」ではなく「見た目も違う」状態にできる。
struct Formatted {
    float value;
};

std::ostream& operator<<(std::ostream& os, Formatted f)
{
    const std::streamsize saved = os.precision();
    os << std::setprecision(6) << f.value;
    os.precision(saved);
    return os;
}

} // namespace

void PrintTo(const Vector2& v, std::ostream* os)
{
    *os << "(" << Formatted{v.x} << ", " << Formatted{v.y} << ")";
}

void PrintTo(const Vector3& v, std::ostream* os)
{
    *os << "(" << Formatted{v.x} << ", " << Formatted{v.y} << ", " << Formatted{v.z} << ")";
}

void PrintTo(const Vector4& v, std::ostream* os)
{
    *os << "(" << Formatted{v.x} << ", " << Formatted{v.y} << ", " << Formatted{v.z} << ", "
        << Formatted{v.w} << ")";
}

void PrintTo(const Quaternion& q, std::ostream* os)
{
    *os << "(x=" << Formatted{q.x} << ", y=" << Formatted{q.y} << ", z=" << Formatted{q.z}
        << ", w=" << Formatted{q.w} << ")";
}

void PrintTo(const Matrix3& m, std::ostream* os)
{
    *os << "\n";
    for (int row = 0; row < 3; ++row) {
        *os << "    [ ";
        for (int col = 0; col < 3; ++col) *os << std::setw(12) << Formatted{m.m[row][col]} << " ";
        *os << "]\n";
    }
}

void PrintTo(const Matrix4& m, std::ostream* os)
{
    *os << "\n";
    for (int row = 0; row < 4; ++row) {
        *os << "    [ ";
        for (int col = 0; col < 4; ++col) *os << std::setw(12) << Formatted{m.m[row][col]} << " ";
        *os << "]\n";
    }
}

} // namespace fbzz::math
