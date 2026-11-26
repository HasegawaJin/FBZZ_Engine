/// @file    Matrix4.cpp
/// @brief   クォータニオンからの回転・座標変換・ビュー行列の構築。
/// @author  Hasegawa Jin
/// @date    2026-05-21

#include "Math/LinearAlgebra/Matrix4.hpp"

namespace fbzz::math {

Matrix4 Matrix4::Rotate(const Quaternion& rotation) {
    const Quaternion normalizedRotation = rotation.Normalized();
    const float xx = normalizedRotation.x * normalizedRotation.x;
    const float yy = normalizedRotation.y * normalizedRotation.y;
    const float zz = normalizedRotation.z * normalizedRotation.z;
    const float xy = normalizedRotation.x * normalizedRotation.y;
    const float xz = normalizedRotation.x * normalizedRotation.z;
    const float yz = normalizedRotation.y * normalizedRotation.z;
    const float wx = normalizedRotation.w * normalizedRotation.x;
    const float wy = normalizedRotation.w * normalizedRotation.y;
    const float wz = normalizedRotation.w * normalizedRotation.z;

    Matrix4 result = Identity();
    result.m[0][0] = 1.0f - 2.0f * (yy + zz);
    result.m[0][1] = 2.0f * (xy - wz);
    result.m[0][2] = 2.0f * (xz + wy);
    result.m[1][0] = 2.0f * (xy + wz);
    result.m[1][1] = 1.0f - 2.0f * (xx + zz);
    result.m[1][2] = 2.0f * (yz - wx);
    result.m[2][0] = 2.0f * (xz - wy);
    result.m[2][1] = 2.0f * (yz + wx);
    result.m[2][2] = 1.0f - 2.0f * (xx + yy);
    return result;
}

Matrix4 Matrix4::TRS(const Vector3& translation, const Quaternion& rotation, const Vector3& scale) {
    // T * R * S を展開し、回転の各列にスケールを掛けて第 4 列に平行移動を置く。
    Matrix4 result = Rotate(rotation);
    for (int row = 0; row < 3; ++row) {
        result.m[row][0] *= scale.x;
        result.m[row][1] *= scale.y;
        result.m[row][2] *= scale.z;
    }
    result.m[0][3] = translation.x;
    result.m[1][3] = translation.y;
    result.m[2][3] = translation.z;
    return result;
}

Matrix4 Matrix4::LookAt(const Vector3& eye, const Vector3& target, const Vector3& up) {
    const Vector3 forward = (target - eye).Normalized();
    const Vector3 right = Vector3::Cross(up, forward).Normalized();
    const Vector3 correctedUp = Vector3::Cross(forward, right);

    Matrix4 result;
    result.m[0][0] = right.x;
    result.m[0][1] = right.y;
    result.m[0][2] = right.z;
    // 視点の位置を右軸へ投影した値を引くと、カメラ自身の横座標が 0 になる。上下・前後の軸でも同じ原理を使う。
    result.m[0][3] = -Vector3::Dot(right, eye);

    result.m[1][0] = correctedUp.x;
    result.m[1][1] = correctedUp.y;
    result.m[1][2] = correctedUp.z;
    result.m[1][3] = -Vector3::Dot(correctedUp, eye);

    result.m[2][0] = forward.x;
    result.m[2][1] = forward.y;
    result.m[2][2] = forward.z;
    result.m[2][3] = -Vector3::Dot(forward, eye);
    result.m[3][3] = 1.0f;
    return result;
}

} /// @note namespace fbzz::math
