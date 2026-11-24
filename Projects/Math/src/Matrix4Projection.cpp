/// @file    Matrix4Projection.cpp
/// @brief   透視投影・正射影と Reversed-Z の構築。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "Math/Matrix4.hpp"
#include "Math/MathContract.hpp"
#include "Math/MathUtils.hpp"
#include <cmath>

namespace fbzz::math {

Matrix4 Matrix4::Perspective(float fovY, float aspect, float nearZ, float farZ) {
    FBZZ_MATH_CONTRACT(aspect > EPSILON, "degenerate aspect; clamped to 1.0");
    if (!(aspect > EPSILON)) {
        aspect = 1.0f;
    }
    FBZZ_MATH_CONTRACT(farZ > nearZ, "far <= near; far pushed past near");
    if (!(farZ > nearZ)) {
        farZ = nearZ + 1.0f;
    }

    /// @note 視野の半角の tan は、奥行き 1 における画面の半分の高さ。その逆数を掛けると上端が投影後の座標 1 に写る。
    const float yScale = 1.0f / std::tan(fovY * 0.5f);
    const float xScale = yScale / aspect;
    const float depthRange = farZ - nearZ;

    Matrix4 result;
    result.m[0][0] = xScale;
    result.m[1][1] = yScale;
    result.m[2][2] = farZ / depthRange;
    result.m[2][3] = -nearZ * farZ / depthRange;
    result.m[3][2] = 1.0f;
    return result;
}

Matrix4 Matrix4::Orthographic(float left, float right,
                             float bottom, float top,
                             float nearZ, float farZ) {
    const float width = right - left;
    const float height = top - bottom;
    const float depthRange = farZ - nearZ;

    Matrix4 result;
    result.m[0][0] = 2.0f / width;
    result.m[1][1] = 2.0f / height;
    result.m[2][2] = 1.0f / depthRange;
    result.m[0][3] = -(right + left) / width;
    result.m[1][3] = -(top + bottom) / height;
    result.m[2][3] = -nearZ / depthRange;
    result.m[3][3] = 1.0f;
    return result;
}

Matrix4 Matrix4::PerspectiveReversedZ(float fovY, float aspect, float nearZ, float farZ) {
    /// @note Perspective の z 行を (w - z) に置き換えた形。z_ndc = n (f - z) / (z (f - n))。
    /// @see https://developer.nvidia.com/content/depth-precision-visualized (NVIDIA, "Depth Precision Visualized")
    Matrix4 result = Perspective(fovY, aspect, nearZ, farZ);
    if (!(farZ > nearZ)) {
        farZ = nearZ + 1.0f;
    }
    const float depthRange = farZ - nearZ;
    result.m[2][2] = -nearZ / depthRange;
    result.m[2][3] = nearZ * farZ / depthRange;
    return result;
}

Matrix4 Matrix4::OrthographicReversedZ(float left, float right,
                                      float bottom, float top,
                                      float nearZ, float farZ) {
    Matrix4 result = Orthographic(left, right, bottom, top, nearZ, farZ);
    const float depthRange = farZ - nearZ;
    result.m[2][2] = -1.0f / depthRange;
    result.m[2][3] = farZ / depthRange;
    return result;
}

} /// @note namespace fbzz::math
