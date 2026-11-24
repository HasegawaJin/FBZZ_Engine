/// @file    FluidOperatorGeometry.hpp
/// @brief   発生源と障害物で共有する幾何計算。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Math/Vector3.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::fluid::operator_geometry_detail {

/// @note CPU と FluidGpuCommon.hlsli で積和の順序を揃える。

inline constexpr float FLUID_PI = 3.14159265358979323846f;
/// @note これより短い向きは «長さ 0» として既定の向きへ倒す。
inline constexpr float MIN_DIRECTION_LENGTH_SQUARED = 1.0e-12f;

[[nodiscard]] inline float Saturate(float value) { return std::clamp(value, 0.0f, 1.0f); }

[[nodiscard]] inline float ScalarDot(const math::Vector3& left, const math::Vector3& right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] inline math::Vector3 NormalizeOr(const math::Vector3& vector, const math::Vector3& fallback)
{
    const float lengthSq = ScalarDot(vector, vector);
    if (lengthSq < MIN_DIRECTION_LENGTH_SQUARED) return fallback;
    return vector * (1.0f / std::sqrt(lengthSq));
}

/// @note 芯の線分 center ± axis × half と、その周りの肉の半径。
struct SegmentFrame {
    math::Vector3 axis{ 0.0f, 1.0f, 0.0f };
    float radius = 0.0f;
    float halfLength = 0.0f;
};

/// @note z = center.z の断面を測るための面内の枠。α = 面内の軸方向、β = それに直交する向き。
/// @note 軸を a、その面内成分の長さを s、奥行き成分を k = |a.z| (s² + k² = 1) とすると、断面の点 α a∥ + β a⊥ から
/// @note 芯への距離は «中ほど (|α s| <= H) なら √(α²k² + β²)»、«端を過ぎたら √((|α| − H s)² + β² + H²k²)»。
/// @note 傾いた棒を平面で切ると細い楕円になり、寝かせれば «2D のカプセル» になる — その両方をこの式が持つ。
struct FlatSegment {
    math::Vector3 along{ 1.0f, 0.0f, 0.0f };
    math::Vector3 across{ 0.0f, 1.0f, 0.0f };
    float planar = 0.0f;   ///< @note s
    float depth = 1.0f;    ///< @note k
    float extent = 0.0f;   ///< @note |α| の上限 (断面の端)
};

/// @note 面内で解けない大きさ。s か k が 0 のとき «その向きには切れない» ことを表す。
inline constexpr float UNBOUNDED_SECTION_EXTENT = 1.0e9f;

void OrthonormalBasis(const math::Vector3& axis, math::Vector3& outFirstBasis, math::Vector3& outSecondBasis);
SegmentFrame MakeSegmentFrame(const math::Vector3& direction, const math::Vector3& size, float minimumSize);
FlatSegment MakeFlatSegment(const SegmentFrame& frame, bool capped);
[[nodiscard]] float FlatSegmentHalfWidth(const SegmentFrame& frame, const FlatSegment& flat, bool capped, float alpha);

} /// @note namespace fbzz::fluid::operator_geometry_detail
