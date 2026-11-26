/// @file    Quaternion.hpp
/// @brief   クォータニオン回転 (ジンバルロック回避・Slerp補間)。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Math/Api.hpp>

#include "Vector3.hpp"

namespace fbzz::math {

/// @note Quaternion.cpp が Matrix4.hpp を include するための前方宣言。
struct Matrix4;

struct Quaternion {
    float x = 0.0f; ///< クォータニオンのベクトル部の X 成分。
    float y = 0.0f; ///< クォータニオンのベクトル部の Y 成分。
    float z = 0.0f; ///< クォータニオンのベクトル部の Z 成分。
    float w = 1.0f; ///< クォータニオンのスカラー部。

    constexpr Quaternion() = default;
    constexpr Quaternion(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}

    static Quaternion Identity();
    FBZZ_MATH_API static Quaternion FromAxisAngle(const Vector3& axis, float angleRad);
    /// @note 内因回転は Yaw → Pitch → Roll (Qy * Qx * Qz)。
    FBZZ_MATH_API static Quaternion FromEuler(const Vector3& eulerRad);
    /// @note forward がほぼゼロなら単位回転。up が forward とほぼ平行なら RIGHT、次に FORWARD を代替軸に試す。
    FBZZ_MATH_API static Quaternion LookRotation(const Vector3& forward, const Vector3& up = Vector3::UP);
    /// @brief 回転行列からクォータニオンを抽出する。
    /// @param[in] matrix 各列に回転後の基底を持つ行列。左上 3×3 を使う。
    /// @pre 左上 3x3 はスケールを含まない回転で、各列が回転後の基底ベクトル。
    FBZZ_MATH_API static Quaternion FromMatrix4(const Matrix4& matrix);

    Quaternion  operator*(const Quaternion& rhs) const;
    /// @brief ベクトルにこのクォータニオンの回転を適用する。
    /// @param[in] v 回転するベクトル。
    /// @pre このクォータニオンは単位長であること。
    Vector3     operator*(const Vector3& v)      const;
    /// @brief 右辺の回転を先に適用する合成回転で自身を置き換える。
    /// @param[in] rhs 現在の回転に先立って適用する回転。
    /// @return 合成後の自身への参照。
    Quaternion& operator*=(const Quaternion& rhs);
    bool        operator==(const Quaternion& rhs) const;

    float      Length()     const;
    FBZZ_MATH_API Quaternion Normalized() const;
    Quaternion Conjugate()  const;
    FBZZ_MATH_API Quaternion Inverse()    const;

    /// @brief NLerp (正規化線形補間)。
    /// @note 等角速度ではないが Slerp より軽量。短弧補間には Slerp を使う。
    FBZZ_MATH_API static Quaternion Lerp(const Quaternion& a, const Quaternion& b, float t);
    FBZZ_MATH_API static Quaternion Slerp(const Quaternion& a, const Quaternion& b, float t);
    static float      Dot(const Quaternion& a, const Quaternion& b);
};

/// @note FBZZMath は DLL のため、.cpp に置くと呼び出しごとに DLL 境界を越えてインライン化されない。契約を報告しない小関数はヘッダーで定義する。
inline Quaternion Quaternion::Identity() { return {0.0f, 0.0f, 0.0f, 1.0f}; }

inline Quaternion Quaternion::operator*(const Quaternion& rhs) const {
    return {
        w * rhs.x + x * rhs.w + y * rhs.z - z * rhs.y,
        w * rhs.y - x * rhs.z + y * rhs.w + z * rhs.x,
        w * rhs.z + x * rhs.y - y * rhs.x + z * rhs.w,
        w * rhs.w - x * rhs.x - y * rhs.y - z * rhs.z
    };
}

inline Vector3 Quaternion::operator*(const Vector3& v) const {
    // q*v*q^-1 を v + 2w(q×v) + 2(q×(q×v)) に展開し、四元数をそのまま掛けるより乗算を減らす。
    Vector3 qv  = {x, y, z};
    Vector3 t   = Vector3::Cross(qv, v) * 2.0f;
    return v + t * w + Vector3::Cross(qv, t);
}

inline Quaternion& Quaternion::operator*=(const Quaternion& rhs) {
    *this = *this * rhs;
    return *this;
}

inline bool Quaternion::operator==(const Quaternion& rhs) const {
    return NearlyEqual(x, rhs.x) && NearlyEqual(y, rhs.y)
        && NearlyEqual(z, rhs.z) && NearlyEqual(w, rhs.w);
}

inline float Quaternion::Length() const {
    return std::sqrt(x * x + y * y + z * z + w * w);
}

inline Quaternion Quaternion::Conjugate() const { return {-x, -y, -z, w}; }

inline float Quaternion::Dot(const Quaternion& a, const Quaternion& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

}
