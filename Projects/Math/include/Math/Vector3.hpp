/// @file    Vector3.hpp
/// @brief   3次元ベクトル (位置・方向・法線・RGB色)。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once

namespace fbzz::math {

struct Vector3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    constexpr Vector3() = default;
    constexpr Vector3(float x, float y, float z) : x(x), y(y), z(z) {}

    Vector3  operator+(const Vector3& rhs) const;
    Vector3  operator-(const Vector3& rhs) const;
    Vector3  operator*(float s)            const;
    Vector3  operator/(float s)            const;
    Vector3  operator-()                   const;
    Vector3& operator+=(const Vector3& rhs);
    Vector3& operator-=(const Vector3& rhs);
    bool     operator==(const Vector3& rhs) const;
    bool     operator!=(const Vector3& rhs) const;

    float   Length()     const;
    float   LengthSq()   const;
    /// @brief 正規化したベクトルを返す。
    /// @pre 長さが 0 でないこと。
    /// @note 契約違反時は ZERO を返し、MathContract.hpp が発生位置つきで 1 度だけ報告する。
    Vector3 Normalized() const;
    /// @brief 正規化を試み、長さ 0 なら fallback を返す。
    /// @note 接触法線・進行方向・視線ベクトルなど退化入力が正常系に含まれる箇所で使う。契約違反として報告しない。
    Vector3 NormalizedOr(const Vector3& fallback) const;

    static float   Dot(const Vector3& a, const Vector3& b);
    static Vector3 Cross(const Vector3& a, const Vector3& b);
    static Vector3 Lerp(const Vector3& a, const Vector3& b, float t);

    static const Vector3 ZERO;
    static const Vector3 ONE;
    static const Vector3 UP;
    static const Vector3 RIGHT;
    static const Vector3 FORWARD; ///< DirectX 左手系: +Z が画面奥方向 (OpenGL は -Z)。
};

inline const Vector3 Vector3::ZERO    = {0.0f, 0.0f, 0.0f};
inline const Vector3 Vector3::ONE     = {1.0f, 1.0f, 1.0f};
inline const Vector3 Vector3::UP      = {0.0f, 1.0f, 0.0f};
inline const Vector3 Vector3::RIGHT   = {1.0f, 0.0f, 0.0f};
inline const Vector3 Vector3::FORWARD = {0.0f, 0.0f, 1.0f};

} // namespace fbzz::math
