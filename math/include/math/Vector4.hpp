// FBZZ Engine
// Vector4.hpp | fbzz::math
// 4次元ベクトル (同次座標・RGBA色・シェーダー定数)
#pragma once

#include "Vector3.hpp"

namespace fbzz::math {

struct Vector4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;

    Vector4() = default;
    constexpr Vector4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
    constexpr Vector4(const Vector3& v, float w) : x(v.x), y(v.y), z(v.z), w(w) {}

    Vector4 operator+(const Vector4& rhs) const;
    Vector4 operator-(const Vector4& rhs) const;
    Vector4 operator*(float s)            const;
    bool    operator==(const Vector4& rhs) const;

    float   Length()     const;
    Vector4 Normalized() const;

    Vector3 XYZ() const;

    static constexpr Vector4 ZERO  = {0.0f, 0.0f, 0.0f, 0.0f};
    static constexpr Vector4 ONE   = {1.0f, 1.0f, 1.0f, 1.0f};
    static constexpr Vector4 WHITE = {1.0f, 1.0f, 1.0f, 1.0f};
    static constexpr Vector4 BLACK = {0.0f, 0.0f, 0.0f, 1.0f};
    static constexpr Vector4 RED   = {1.0f, 0.0f, 0.0f, 1.0f};
    static constexpr Vector4 GREEN = {0.0f, 1.0f, 0.0f, 1.0f};
    static constexpr Vector4 BLUE  = {0.0f, 0.0f, 1.0f, 1.0f};
};

} // namespace fbzz::math
