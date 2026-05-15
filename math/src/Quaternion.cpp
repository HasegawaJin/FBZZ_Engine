// FBZZ Engine
// Quaternion.cpp | fbzz::math
// クォータニオンの演算実装
#include "math/Quaternion.hpp"
#include "math/MathUtils.hpp"
#include <cmath>
#include <cassert>

namespace fbzz::math {

Quaternion Quaternion::Identity() { return {0.0f, 0.0f, 0.0f, 1.0f}; }

Quaternion Quaternion::FromAxisAngle(const Vector3& axis, float angleRad) {
    float half = angleRad * 0.5f;
    float s    = std::sin(half);
    float c    = std::cos(half);
    Vector3 n  = axis.Normalized();
    return {n.x * s, n.y * s, n.z * s, c};
}

Quaternion Quaternion::FromEuler(const Vector3& eulerRad) {
    // XYZ 順 (Pitch → Yaw → Roll)
    float cx = std::cos(eulerRad.x * 0.5f), sx = std::sin(eulerRad.x * 0.5f);
    float cy = std::cos(eulerRad.y * 0.5f), sy = std::sin(eulerRad.y * 0.5f);
    float cz = std::cos(eulerRad.z * 0.5f), sz = std::sin(eulerRad.z * 0.5f);
    return {
        sx * cy * cz + cx * sy * sz,
        cx * sy * cz - sx * cy * sz,
        cx * cy * sz + sx * sy * cz,
        cx * cy * cz - sx * sy * sz
    };
}

Quaternion Quaternion::LookRotation(const Vector3& forward, const Vector3& up) {
    Vector3 f = forward.Normalized();
    Vector3 r = Vector3::Cross(up, f).Normalized();
    Vector3 u = Vector3::Cross(f, r);

    // 回転行列 → クォータニオン変換
    float trace = r.x + u.y + f.z;
    if (trace > 0.0f) {
        float s = 0.5f / std::sqrt(trace + 1.0f);
        return {(u.z - f.y) * s, (f.x - r.z) * s, (r.y - u.x) * s, 0.25f / s};
    } else if (r.x > u.y && r.x > f.z) {
        float s = 2.0f * std::sqrt(1.0f + r.x - u.y - f.z);
        return {0.25f * s, (u.x + r.y) / s, (f.x + r.z) / s, (u.z - f.y) / s};
    } else if (u.y > f.z) {
        float s = 2.0f * std::sqrt(1.0f + u.y - r.x - f.z);
        return {(u.x + r.y) / s, 0.25f * s, (u.z + f.y) / s, (f.x - r.z) / s};
    } else {
        float s = 2.0f * std::sqrt(1.0f + f.z - r.x - u.y);
        return {(f.x + r.z) / s, (u.z + f.y) / s, 0.25f * s, (r.y - u.x) / s};
    }
}

Quaternion Quaternion::operator*(const Quaternion& rhs) const {
    return {
        w * rhs.x + x * rhs.w + y * rhs.z - z * rhs.y,
        w * rhs.y - x * rhs.z + y * rhs.w + z * rhs.x,
        w * rhs.z + x * rhs.y - y * rhs.x + z * rhs.w,
        w * rhs.w - x * rhs.x - y * rhs.y - z * rhs.z
    };
}

Vector3 Quaternion::operator*(const Vector3& v) const {
    Vector3 qv  = {x, y, z};
    Vector3 t   = Vector3::Cross(qv, v) * 2.0f;
    return v + t * w + Vector3::Cross(qv, t);
}

Quaternion& Quaternion::operator*=(const Quaternion& rhs) {
    *this = *this * rhs;
    return *this;
}

bool Quaternion::operator==(const Quaternion& rhs) const {
    return NearlyEqual(x, rhs.x) && NearlyEqual(y, rhs.y)
        && NearlyEqual(z, rhs.z) && NearlyEqual(w, rhs.w);
}

float Quaternion::Length() const {
    return std::sqrt(x * x + y * y + z * z + w * w);
}

Quaternion Quaternion::Normalized() const {
    float len = Length();
    assert(!NearlyZero(len) && "Cannot normalize a zero-length quaternion");
    float inv = 1.0f / len;
    return {x * inv, y * inv, z * inv, w * inv};
}

Quaternion Quaternion::Conjugate() const { return {-x, -y, -z, w}; }

Quaternion Quaternion::Inverse() const {
    float lenSq = x * x + y * y + z * z + w * w;
    assert(!NearlyZero(lenSq) && "Cannot invert a zero quaternion");
    float inv = 1.0f / lenSq;
    return {-x * inv, -y * inv, -z * inv, w * inv};
}

float Quaternion::Dot(const Quaternion& a, const Quaternion& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

Quaternion Quaternion::Lerp(const Quaternion& a, const Quaternion& b, float t) {
    Quaternion result = {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t
    };
    return result.Normalized();
}

Quaternion Quaternion::Slerp(const Quaternion& a, const Quaternion& b, float t) {
    float dot = Dot(a, b);

    // 遠回りを防ぐため dot が負なら b を反転
    Quaternion b2 = b;
    if (dot < 0.0f) {
        b2 = {-b.x, -b.y, -b.z, -b.w};
        dot = -dot;
    }

    // dot が 1 に近ければ線形補間で近似 (acos が不安定になるため)
    if (dot > 1.0f - EPSILON) {
        return Lerp(a, b2, t);
    }

    float theta     = std::acos(dot);
    float sinTheta  = std::sin(theta);
    float wa        = std::sin((1.0f - t) * theta) / sinTheta;
    float wb        = std::sin(t * theta) / sinTheta;

    return {
        a.x * wa + b2.x * wb,
        a.y * wa + b2.y * wb,
        a.z * wa + b2.z * wb,
        a.w * wa + b2.w * wb
    };
}

} // namespace fbzz::math
