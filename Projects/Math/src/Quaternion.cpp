/// @file    Quaternion.cpp
/// @brief   クォータニオンの演算実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Math/Quaternion.hpp"
#include "Math/Matrix4.hpp"
#include "Math/MathUtils.hpp"
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
    // YXZ 内因順 (Qy * Qx * Qz): Y(Yaw) → X(Pitch) → Z(Roll)
    // X が中間角になるため Pitch が ±90° に制約され、Yaw は任意範囲を扱える。
    float cx = std::cos(eulerRad.x * 0.5f), sx = std::sin(eulerRad.x * 0.5f);
    float cy = std::cos(eulerRad.y * 0.5f), sy = std::sin(eulerRad.y * 0.5f);
    float cz = std::cos(eulerRad.z * 0.5f), sz = std::sin(eulerRad.z * 0.5f);
    return {
         cy * sx * cz + sy * cx * sz,
        -cy * sx * sz + sy * cx * cz,
         cy * cx * sz - sy * sx * cz,
         cy * cx * cz + sy * sx * sz
    };
}

Quaternion Quaternion::LookRotation(const Vector3& forward, const Vector3& up) {
    // 「向く先が自分と同じ位置」は呼び出し側で普通に起きる (追従対象へ重なった、
    // 速度が 0 になった等)。基底を作れないので回さないだけにし、assert で止めない。
    if (NearlyZero(forward.LengthSq())) return Identity();
    Vector3 f = forward.Normalized();
    // forward と up が平行なとき (縮退ケース) は代替 up を使う
    // RIGHT も平行なら FORWARD を使う (forward が RIGHT 方向のとき)
    Vector3 safeUp = up;
    if (NearlyZero(Vector3::Cross(up, f).LengthSq()))
        safeUp = NearlyZero(Vector3::Cross(Vector3::RIGHT, f).LengthSq())
               ? Vector3::FORWARD : Vector3::RIGHT;
    Vector3 r = Vector3::Cross(safeUp, f).Normalized();
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

// Shepperd's method: extracts rotation from a pure rotation matrix (no scale/translation).
// m[row][col], column-vector convention: column i = rotated basis vector i.
Quaternion Quaternion::FromMatrix4(const Matrix4& m) {
    float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
    if (trace > 0.0f) {
        float s = 2.0f * std::sqrt(trace + 1.0f); // s = 4w
        return { (m.m[2][1] - m.m[1][2]) / s,
                 (m.m[0][2] - m.m[2][0]) / s,
                 (m.m[1][0] - m.m[0][1]) / s,
                 s * 0.25f };
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        float s = 2.0f * std::sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]); // s = 4x
        return { s * 0.25f,
                 (m.m[0][1] + m.m[1][0]) / s,
                 (m.m[0][2] + m.m[2][0]) / s,
                 (m.m[2][1] - m.m[1][2]) / s };
    } else if (m.m[1][1] > m.m[2][2]) {
        float s = 2.0f * std::sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]); // s = 4y
        return { (m.m[0][1] + m.m[1][0]) / s,
                 s * 0.25f,
                 (m.m[1][2] + m.m[2][1]) / s,
                 (m.m[0][2] - m.m[2][0]) / s };
    } else {
        float s = 2.0f * std::sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]); // s = 4z
        return { (m.m[0][2] + m.m[2][0]) / s,
                 (m.m[1][2] + m.m[2][1]) / s,
                 s * 0.25f,
                 (m.m[1][0] - m.m[0][1]) / s };
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

// q*v*q^-1 の展開形: v + 2w(q×v) + 2(q×(q×v)) — フル四元数乗算より乗算回数が少ない
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
