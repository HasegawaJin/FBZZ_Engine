/// @file    FluidColliderEval.cpp
/// @brief   障害物の符号付き距離と液体を押し出す向き。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Fluid/Operators/FluidOperatorEval.hpp>
#include "FluidOperatorGeometry.hpp"
#include <algorithm>
#include <cmath>

namespace fbzz::fluid {

namespace {

using namespace operator_geometry_detail;

/// @note sign(0) を +1 に倒す。0 を返すと箱の中心ちょうどで法線が長さ 0 になり、押し出す向きが無くなる。
[[nodiscard]] float SignOrPositive(float value) { return value < 0.0f ? -1.0f : 1.0f; }

/// @note 芯の上で «外向き» が決まらないときに返す向き。軸に直交する 1 本を決まった手順で選ぶ
/// @note (up を返すと軸が上向きのとき芯に沿って押してしまう)。
math::Vector3 SegmentFallbackNormal(const math::Vector3& axis)
{
    math::Vector3 firstBasis;
    math::Vector3 secondBasis;
    OrthonormalBasis(axis, firstBasis, secondBasis);
    return firstBasis;
}

math::Vector3 BoxFaceDistances(const math::Vector3& offset, const math::Vector3& halfExtents, float minimumSize)
{
    return {
        std::fabs(offset.x) - (std::max)(halfExtents.x, minimumSize),
        std::fabs(offset.y) - (std::max)(halfExtents.y, minimumSize),
        std::fabs(offset.z) - (std::max)(halfExtents.z, minimumSize)
    };
}

} /// @note namespace

float FluidColliderDistance(const FluidCollider& collider, const math::Vector3& center, const math::Vector3& point,
                            float minimumSize, bool isVolumetric)
{
    math::Vector3 centerOffset = point - center;
    if (!isVolumetric) {
        centerOffset.z = 0.0f;
    }
    switch (collider.shape) {
    case FluidColliderShape::Box: {
        const auto [faceDistanceX, faceDistanceY, faceDistanceZ] = BoxFaceDistances(centerOffset, collider.size, minimumSize);
        const float outsideX = (std::max)(faceDistanceX, 0.0f);
        const float outsideY = (std::max)(faceDistanceY, 0.0f);
        const float outsideZ = (std::max)(faceDistanceZ, 0.0f);
        return std::sqrt(outsideX * outsideX + outsideY * outsideY + outsideZ * outsideZ) + (std::min)((std::max)(faceDistanceX, (std::max)(faceDistanceY, faceDistanceZ)), 0.0f);
    }
    case FluidColliderShape::Plane:
        return ScalarDot(centerOffset, NormalizeOr(collider.direction, math::Vector3{ 0.0f, 1.0f, 0.0f }));
    case FluidColliderShape::Capsule: {
        const SegmentFrame frame = MakeSegmentFrame(collider.direction, collider.size, minimumSize);
        const float clampedAxisDistance = std::clamp(ScalarDot(centerOffset, frame.axis), -frame.halfLength, frame.halfLength);
        const math::Vector3 radial = centerOffset - frame.axis * clampedAxisDistance;
        return std::sqrt(ScalarDot(radial, radial)) - frame.radius;
    }
    case FluidColliderShape::Cylinder: {
        const SegmentFrame frame = MakeSegmentFrame(collider.direction, collider.size, minimumSize);
        const float axisDistance = ScalarDot(centerOffset, frame.axis);
        const math::Vector3 radial = centerOffset - frame.axis * axisDistance;
        // 半径の向きと軸の向きを 2 軸と見れば、Box と同じ «外の長さ + 中の深さ» で測れる。
        const float radialDistance = std::sqrt(ScalarDot(radial, radial)) - frame.radius;
        const float capDistance = std::fabs(axisDistance) - frame.halfLength;
        const float outsideRadius = (std::max)(radialDistance, 0.0f);
        const float outsideCap = (std::max)(capDistance, 0.0f);
        return std::sqrt(outsideRadius * outsideRadius + outsideCap * outsideCap) + (std::min)((std::max)(radialDistance, capDistance), 0.0f);
    }
    case FluidColliderShape::Sphere:
    default:
        return std::sqrt(ScalarDot(centerOffset, centerOffset)) - (std::max)(collider.size.x, minimumSize);
    }
}

math::Vector3 FluidColliderNormal(const FluidCollider& collider, const math::Vector3& center, const math::Vector3& point,
                                  float minimumSize, bool isVolumetric)
{
    const math::Vector3 up = { 0.0f, 1.0f, 0.0f };
    math::Vector3 centerOffset = point - center;
    if (!isVolumetric) {
        centerOffset.z = 0.0f;
    }
    math::Vector3 normal = up;
    switch (collider.shape) {
    case FluidColliderShape::Box: {
        const auto [faceDistanceX, faceDistanceY, faceDistanceZ] = BoxFaceDistances(centerOffset, collider.size, minimumSize);
        // 距離 > 0 ⇔ どれかの q > 0 (外側の長さが正)。
        if ((std::max)(faceDistanceX, (std::max)(faceDistanceY, faceDistanceZ)) > 0.0f) {
            normal = NormalizeOr(math::Vector3{ (std::max)(faceDistanceX, 0.0f) * SignOrPositive(centerOffset.x),
                                                (std::max)(faceDistanceY, 0.0f) * SignOrPositive(centerOffset.y),
                                                (std::max)(faceDistanceZ, 0.0f) * SignOrPositive(centerOffset.z) }, up);
        } else if (faceDistanceX >= faceDistanceY && faceDistanceX >= faceDistanceZ) {
            // 同じ深さの軸が並んだら x → y → z の順に採る (GPU の写しも同じ順)。
            normal = { SignOrPositive(centerOffset.x), 0.0f, 0.0f };
        } else if (faceDistanceY >= faceDistanceZ) {
            normal = { 0.0f, SignOrPositive(centerOffset.y), 0.0f };
        } else {
            normal = { 0.0f, 0.0f, SignOrPositive(centerOffset.z) };
        }
        break;
    }
    case FluidColliderShape::Plane:
        normal = NormalizeOr(collider.direction, up);
        break;
    case FluidColliderShape::Capsule: {
        const SegmentFrame frame = MakeSegmentFrame(collider.direction, collider.size, minimumSize);
        const float clampedAxisDistance = std::clamp(ScalarDot(centerOffset, frame.axis), -frame.halfLength, frame.halfLength);
        const math::Vector3 radial = centerOffset - frame.axis * clampedAxisDistance;
        const float length = std::sqrt(ScalarDot(radial, radial));
        normal = length < 1.0e-6f ? SegmentFallbackNormal(frame.axis) : radial * (1.0f / length);
        break;
    }
    case FluidColliderShape::Cylinder: {
        const SegmentFrame frame = MakeSegmentFrame(collider.direction, collider.size, minimumSize);
        const float axisDistance = ScalarDot(centerOffset, frame.axis);
        const math::Vector3 radial = centerOffset - frame.axis * axisDistance;
        const float length = std::sqrt(ScalarDot(radial, radial));
        const math::Vector3 radialNormal =
            length < 1.0e-6f ? SegmentFallbackNormal(frame.axis) : radial * (1.0f / length);
        const math::Vector3 capNormal = frame.axis * SignOrPositive(axisDistance);
        const float radialDistance = length - frame.radius;
        const float capDistance = std::fabs(axisDistance) - frame.halfLength;
        if ((std::max)(radialDistance, capDistance) > 0.0f) {
            normal = NormalizeOr(radialNormal * (std::max)(radialDistance, 0.0f) + capNormal * (std::max)(capDistance, 0.0f), up);
        } else {
            // 同じ深さなら半径の側を採る (Box が x → y → z の順に採るのと同じ «先の軸が勝つ» 規則)。
            normal = radialDistance >= capDistance ? radialNormal : capNormal;
        }
        break;
    }
    case FluidColliderShape::Sphere:
    default: {
        const float length = std::sqrt(ScalarDot(centerOffset, centerOffset));
        normal = length < 1.0e-6f ? up : centerOffset * (1.0f / length);
        break;
    }
    }
    if (!isVolumetric) {
        normal.z = 0.0f;
        normal = NormalizeOr(normal, up);
    }
    return normal;
}

} /// @note namespace fbzz::fluid
