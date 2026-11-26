/// @file    FluidOperatorGeometry.cpp
/// @brief   発生源と障害物の軸・画面内断面を作る。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "FluidOperatorGeometry.hpp"

namespace fbzz::fluid::operator_geometry_detail {

/// @note axis に直交する正規直交基底。どの 1 組でも形は同じなので、決まった手順で選ぶ。
void OrthonormalBasis(const math::Vector3& axis, math::Vector3& outFirstBasis, math::Vector3& outSecondBasis)
{
    const math::Vector3 helper = std::fabs(axis.x) < 0.9f ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                                                          : math::Vector3{ 0.0f, 1.0f, 0.0f };
    outFirstBasis = NormalizeOr(math::Vector3::Cross(axis, helper), math::Vector3{ 0.0f, 0.0f, 1.0f });
    outSecondBasis = math::Vector3::Cross(axis, outFirstBasis);
}

SegmentFrame MakeSegmentFrame(const math::Vector3& direction, const math::Vector3& size, float minimumSize)
{
    SegmentFrame frame;
    frame.axis = NormalizeOr(direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
    frame.radius = (std::max)(size.x, minimumSize);
    frame.halfLength = (std::max)(size.y, minimumSize);
    return frame;
}

/// @note capped = 端が平ら (円柱)。false なら端に半球が付く (カプセル)。
FlatSegment MakeFlatSegment(const SegmentFrame& frame, bool capped)
{
    FlatSegment flat;
    const math::Vector3& axis = frame.axis;
    flat.planar = std::sqrt(axis.x * axis.x + axis.y * axis.y);
    flat.depth = std::fabs(axis.z);
    if (flat.planar > 1.0e-6f) {
        flat.along = { axis.x / flat.planar, axis.y / flat.planar, 0.0f };
        flat.across = { -flat.along.y, flat.along.x, 0.0f };
    }
    if (frame.radius <= 0.0f) {
        return flat;
    }
    const float alongExtent = flat.planar > 1.0e-6f ? frame.halfLength / flat.planar : UNBOUNDED_SECTION_EXTENT;
    const float acrossExtent = flat.depth > 1.0e-6f ? frame.radius / flat.depth : UNBOUNDED_SECTION_EXTENT;
    if (capped) {
        flat.extent = (std::min)(alongExtent, acrossExtent);
        return flat;
    }
    // 端の球が断面に顔を出すのは R s > H k のときだけ (そのとき断面は «楕円を切った帯 + 端の円の一部»)。
    // 出ないときは楕円がそのまま断面になる。
    flat.extent = frame.radius * flat.planar > frame.halfLength * flat.depth
        ? frame.halfLength * flat.planar + std::sqrt((std::max)(frame.radius * frame.radius
                                                          - frame.halfLength * frame.halfLength * flat.depth * flat.depth, 0.0f))
        : acrossExtent;
    return flat;
}

/// @note 断面の α における β の半幅 (形の外なら 0)。
[[nodiscard]] float FlatSegmentHalfWidth(const SegmentFrame& frame, const FlatSegment& flat, bool capped, float alpha)
{
    const float axisDistance = alpha * flat.planar;
    if (capped) {
        if (std::fabs(axisDistance) > frame.halfLength) {
            return 0.0f;
        }
        const float widthSquared = frame.radius * frame.radius - alpha * alpha * flat.depth * flat.depth;
        return widthSquared > 0.0f ? std::sqrt(widthSquared) : 0.0f;
    }
    const float clampedAxisDistance = std::clamp(axisDistance, -frame.halfLength, frame.halfLength);
    const float offsetFromEnd = alpha - clampedAxisDistance * flat.planar;
    const float widthSquared = frame.radius * frame.radius - offsetFromEnd * offsetFromEnd - clampedAxisDistance * clampedAxisDistance * flat.depth * flat.depth;
    return widthSquared > 0.0f ? std::sqrt(widthSquared) : 0.0f;
}

} /// @note namespace fbzz::fluid::operator_geometry_detail
