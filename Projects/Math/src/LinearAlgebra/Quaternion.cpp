/// @file    Quaternion.cpp
/// @brief   クォータニオンの演算実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Math/LinearAlgebra/Quaternion.hpp"
#include "Math/Scalar/MathContract.hpp"
#include "Math/Scalar/MathUtils.hpp"
#include "Math/LinearAlgebra/Matrix4.hpp"

#include <cmath>

namespace fbzz::math {

namespace {

/// @pre 回転行列の列を受け取る。呼び出し元で正のトレースを処理済み。
/// @note Shepperd 法の対角成分を使う分岐。対角成分が最大の軸から求めると、小さな値での除算を避けられる。
Quaternion FromDominantDiagonal(const Vector3& right, const Vector3& up, const Vector3& forward) {
    if (right.x > up.y && right.x > forward.z) {
        const float fourX = 2.0f * std::sqrt(1.0f + right.x - up.y - forward.z);
        return {0.25f * fourX, (up.x + right.y) / fourX, (forward.x + right.z) / fourX,
                (up.z - forward.y) / fourX};
    }
    if (up.y > forward.z) {
        const float fourY = 2.0f * std::sqrt(1.0f + up.y - right.x - forward.z);
        return {(up.x + right.y) / fourY, 0.25f * fourY, (up.z + forward.y) / fourY,
                (forward.x - right.z) / fourY};
    }
    const float fourZ = 2.0f * std::sqrt(1.0f + forward.z - right.x - up.y);
    return {(forward.x + right.z) / fourZ, (up.z + forward.y) / fourZ, 0.25f * fourZ,
            (right.y - up.x) / fourZ};
}

}

Quaternion Quaternion::FromAxisAngle(const Vector3& axis, float angleRad) {
    // 回転は q とその逆で向きを両側から変換する。角度 θ の回転は、軸に sin(θ/2) を掛けた 3 成分と cos(θ/2) で表す。
    const float halfAngle = angleRad * 0.5f;
    const float sinHalfAngle = std::sin(halfAngle);
    const float cosHalfAngle = std::cos(halfAngle);
    const Vector3 unitAxis = axis.Normalized();
    return {unitAxis.x * sinHalfAngle, unitAxis.y * sinHalfAngle, unitAxis.z * sinHalfAngle, cosHalfAngle};
}

Quaternion Quaternion::FromEuler(const Vector3& eulerRad) {
    const float cx = std::cos(eulerRad.x * 0.5f);
    const float sx = std::sin(eulerRad.x * 0.5f);
    const float cy = std::cos(eulerRad.y * 0.5f);
    const float sy = std::sin(eulerRad.y * 0.5f);
    const float cz = std::cos(eulerRad.z * 0.5f);
    const float sz = std::sin(eulerRad.z * 0.5f);
    return {
         cy * sx * cz + sy * cx * sz,
        -cy * sx * sz + sy * cx * cz,
         cy * cx * sz - sy * sx * cz,
         cy * cx * cz + sy * sx * sz
    };
}

Quaternion Quaternion::LookRotation(const Vector3& forward, const Vector3& up) {
    if (NearlyZero(forward.LengthSq())) {
        return Identity();
    }

    const Vector3 unitForward = forward.Normalized();
    Vector3 right = Vector3::Cross(up, unitForward);
    if (NearlyZero(right.LengthSq())) {
        right = Vector3::Cross(Vector3::RIGHT, unitForward);
        if (NearlyZero(right.LengthSq())) {
            right = Vector3::Cross(Vector3::FORWARD, unitForward);
        }
    }
    const Vector3 unitRight = right.Normalized();
    const Vector3 correctedUp = Vector3::Cross(unitForward, unitRight);

    const float trace = unitRight.x + correctedUp.y + unitForward.z;
    if (trace > 0.0f) {
        // FromMatrix4 と式は等価だが、既存の丸め順序を保つため逆数を先に求める。
        const float inverseFourW = 0.5f / std::sqrt(trace + 1.0f);
        return {(correctedUp.z - unitForward.y) * inverseFourW,
                (unitForward.x - unitRight.z) * inverseFourW,
                (unitRight.y - correctedUp.x) * inverseFourW,
                0.25f / inverseFourW};
    }
    return FromDominantDiagonal(unitRight, correctedUp, unitForward);
}

Quaternion Quaternion::FromMatrix4(const Matrix4& matrix) {
    const Vector3 right{matrix.m[0][0], matrix.m[1][0], matrix.m[2][0]};
    const Vector3 up{matrix.m[0][1], matrix.m[1][1], matrix.m[2][1]};
    const Vector3 forward{matrix.m[0][2], matrix.m[1][2], matrix.m[2][2]};

    const float trace = right.x + up.y + forward.z;
    if (trace > 0.0f) {
        const float fourW = 2.0f * std::sqrt(trace + 1.0f);
        return {(up.z - forward.y) / fourW, (forward.x - right.z) / fourW,
                (right.y - up.x) / fourW, fourW * 0.25f};
    }
    return FromDominantDiagonal(right, up, forward);
}

Quaternion Quaternion::Normalized() const {
    const float length = Length();
    FBZZ_MATH_CONTRACT(!NearlyZero(length),
                       "zero-length quaternion normalized; returning identity");
    if (NearlyZero(length)) {
        return Identity();
    }
    const float inverseLength = 1.0f / length;
    return {x * inverseLength, y * inverseLength, z * inverseLength, w * inverseLength};
}

Quaternion Quaternion::Inverse() const {
    const float lengthSq = x * x + y * y + z * z + w * w;
    FBZZ_MATH_CONTRACT(!NearlyZero(lengthSq),
                       "zero quaternion inverted; returning identity");
    if (NearlyZero(lengthSq)) {
        return Identity();
    }
    const float inverseLengthSq = 1.0f / lengthSq;
    return {-x * inverseLengthSq, -y * inverseLengthSq, -z * inverseLengthSq, w * inverseLengthSq};
}

Quaternion Quaternion::Lerp(const Quaternion& a, const Quaternion& b, float t) {
    const Quaternion result = {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t
    };
    return result.Normalized();
}

Quaternion Quaternion::Slerp(const Quaternion& a, const Quaternion& b, float t) {
    float dot = Dot(a, b);

    // 遠回りを防ぐため dot が負なら b を反転する。
    Quaternion end = b;
    if (dot < 0.0f) {
        end = {-b.x, -b.y, -b.z, -b.w};
        dot = -dot;
    }

    // dot が 1 に近ければ線形補間で近似する (acos が不安定になるため)。
    if (dot > 1.0f - EPSILON) {
        return Lerp(a, end, t);
    }

    const float theta = std::acos(dot);
    const float sinTheta = std::sin(theta);
    // 二つの姿勢を結ぶ円弧上で、進んだ角度が t × θ になる重み。t が同じだけ増えるたびに同じ角度だけ回る。
    const float startWeight = std::sin((1.0f - t) * theta) / sinTheta;
    const float endWeight = std::sin(t * theta) / sinTheta;

    return {
        a.x * startWeight + end.x * endWeight,
        a.y * startWeight + end.y * endWeight,
        a.z * startWeight + end.z * endWeight,
        a.w * startWeight + end.w * endWeight
    };
}

}
