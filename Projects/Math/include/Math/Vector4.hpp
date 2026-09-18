/// @file    Vector4.hpp
/// @brief   4次元ベクトル (同次座標・RGBA色・シェーダー定数)。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once

#include "Vector3.hpp"

namespace fbzz::math {

struct Vector4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;

    constexpr Vector4() = default;
    constexpr Vector4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
    constexpr Vector4(const Vector3& v, float w) : x(v.x), y(v.y), z(v.z), w(w) {}

    Vector4 operator+(const Vector4& rhs) const;
    Vector4 operator-(const Vector4& rhs) const;
    Vector4 operator*(float s)            const;
    bool    operator==(const Vector4& rhs) const;

    float   Length()     const;
    Vector4 Normalized() const;

    Vector3 XYZ() const;

    static const Vector4 ZERO;
    static const Vector4 ONE;
    static const Vector4 WHITE;
    static const Vector4 BLACK;
    static const Vector4 RED;
    static const Vector4 GREEN;
    static const Vector4 BLUE;
};

inline const Vector4 Vector4::ZERO  = {0.0f, 0.0f, 0.0f, 0.0f};
inline const Vector4 Vector4::ONE   = {1.0f, 1.0f, 1.0f, 1.0f};
inline const Vector4 Vector4::WHITE = {1.0f, 1.0f, 1.0f, 1.0f};
inline const Vector4 Vector4::BLACK = {0.0f, 0.0f, 0.0f, 1.0f};
inline const Vector4 Vector4::RED   = {1.0f, 0.0f, 0.0f, 1.0f};
inline const Vector4 Vector4::GREEN = {0.0f, 1.0f, 0.0f, 1.0f};
inline const Vector4 Vector4::BLUE  = {0.0f, 0.0f, 1.0f, 1.0f};

/// @note FBZZMath は DLL のため、.cpp に置くと呼び出しごとに DLL 境界を越えてインライン化されない。契約を報告しない小関数はヘッダーで定義する。
inline Vector4 Vector4::operator+(const Vector4& rhs) const { return {x + rhs.x, y + rhs.y, z + rhs.z, w + rhs.w}; }
inline Vector4 Vector4::operator-(const Vector4& rhs) const { return {x - rhs.x, y - rhs.y, z - rhs.z, w - rhs.w}; }
inline Vector4 Vector4::operator*(float s)            const { return {x * s, y * s, z * s, w * s}; }

inline bool Vector4::operator==(const Vector4& rhs) const {
    return NearlyEqual(x, rhs.x) && NearlyEqual(y, rhs.y)
        && NearlyEqual(z, rhs.z) && NearlyEqual(w, rhs.w);
}

inline float Vector4::Length() const {
    return std::sqrt(x * x + y * y + z * z + w * w);
}

inline Vector3 Vector4::XYZ() const { return {x, y, z}; }

} // namespace fbzz::math
