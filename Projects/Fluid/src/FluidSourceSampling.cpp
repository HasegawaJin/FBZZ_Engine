/// @file    FluidSourceSampling.cpp
/// @brief   発生源の形状とテクスチャマスクから発生点を選ぶ。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Fluid/FluidOperatorEval.hpp>
#include "FluidOperatorGeometry.hpp"
#include <algorithm>
#include <cmath>

namespace fbzz::fluid {

namespace {

using namespace operator_geometry_detail;

math::Vector3 SampleCone3D(const FluidSource& source, float radius, float length, float u0, float u1, float u2)
{
    const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
    math::Vector3 firstBasis;
    math::Vector3 secondBasis;
    OrthonormalBasis(axis, firstBasis, secondBasis);
    /// @note 断面積は t² に比例するので、t = L·u^(1/3) で体積一様になる。
    const float t = length * std::cbrt(u0);
    const float allowed = length > 0.0f ? radius * t / length : 0.0f;
    const float r = allowed * std::sqrt(u1);
    const float angle = 2.0f * FLUID_PI * u2;
    return axis * t + firstBasis * (r * std::cos(angle)) + secondBasis * (r * std::sin(angle));
}

/// @note 頂点を通る z = center.z の平面で円錐を切ると、頂点から伸びる三角形になる。
/// @note 軸の画面内成分を s、傾き k = R/L とすると、画面内の軸方向に長さ L/s、底の半幅 (L/s)·√((1+k²)s² − 1)。
/// @note s = 1 (軸が画面内) なら長さ L・半幅 R の三角形。g <= 0 なら断面は頂点だけで、中心を返す。
math::Vector3 SampleCone2D(const FluidSource& source, float radius, float length, float u0, float u1)
{
    const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
    const float planar = std::sqrt(axis.x * axis.x + axis.y * axis.y);
    if (length <= 0.0f || planar < 1.0e-6f) return { 0.0f, 0.0f, 0.0f };
    const float slope = radius / length;
    const float g = (1.0f + slope * slope) * planar * planar - 1.0f;
    if (g <= 0.0f) return { 0.0f, 0.0f, 0.0f };
    const float planarLength = length / planar;
    /// @note 三角形の幅は頂点からの距離に比例するので、√u で面積一様になる。
    const float along = planarLength * std::sqrt(u0);
    const float across = along * std::sqrt(g) * (2.0f * u1 - 1.0f);
    const float ax = axis.x / planar;
    const float ay = axis.y / planar;
    return { ax * along - ay * across, ay * along + ax * across, 0.0f };
}

math::Vector3 SampleRing3D(const FluidSource& source, float ringRadius, float tubeRadius, float u0, float u1, float u2)
{
    const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
    math::Vector3 firstBasis;
    math::Vector3 secondBasis;
    OrthonormalBasis(axis, firstBasis, secondBasis);
    const float theta = 2.0f * FLUID_PI * u0;
    const float tube = tubeRadius * std::sqrt(u1);
    const float psi = 2.0f * FLUID_PI * u2;
    const math::Vector3 outward = firstBasis * std::cos(theta) + secondBasis * std::sin(theta);
    return outward * (ringRadius + tube * std::cos(psi)) + axis * (tube * std::sin(psi));
}

/// @note z = center.z の断面は «輪の各点を中心とする半径 r の球» を平面で切った円の和になる。
/// @note 輪の点の奥行きは R·tilt·cos(θ − θ0) なので、|奥行き| < r の弧だけを選び、その点の断面円の中から取る。
/// @note 輪が画面内に寝ていれば (tilt·R <= r) 弧は全周になり、輪帯になる。
math::Vector3 SampleRing2D(const FluidSource& source, float ringRadius, float tubeRadius, float u0, float u1, float u2)
{
    const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
    math::Vector3 firstBasis;
    math::Vector3 secondBasis;
    OrthonormalBasis(axis, firstBasis, secondBasis);
    const float tilt = std::sqrt(firstBasis.z * firstBasis.z + secondBasis.z * secondBasis.z);
    const float reach = ringRadius * tilt;
    const float halfArc = reach > tubeRadius ? std::asin(tubeRadius / reach) : FLUID_PI * 0.5f;
    const float phase = std::atan2(secondBasis.z, firstBasis.z);
    const bool second = u0 >= 0.5f;
    const float local = second ? u0 * 2.0f - 1.0f : u0 * 2.0f;
    const float theta = phase + FLUID_PI * 0.5f + (second ? FLUID_PI : 0.0f) + (2.0f * local - 1.0f) * halfArc;
    const math::Vector3 onRing = (firstBasis * std::cos(theta) + secondBasis * std::sin(theta)) * ringRadius;
    const float disc = std::sqrt((std::max)(tubeRadius * tubeRadius - onRing.z * onRing.z, 0.0f)) * std::sqrt(u1);
    const float angle = 2.0f * FLUID_PI * u2;
    return { onRing.x + disc * std::cos(angle), onRing.y + disc * std::sin(angle), 0.0f };
}

/// @note 解いた座標をこの内側に収める。ちょうど 1 だと板の縁で重みが 0 になり、湧かせた点が «形の外» 扱いになる。
constexpr float PLATE_INTERIOR_LIMIT = 0.999f;

/// @note Texture の板の中の点 (中心からのずれ)。3D は箱の中で一様。
/// @note 2D は z = 0 の断面から選ぶ: z 成分が最も大きい軸の座標を «断面に乗る» ように解き、残る 2 軸は一様に取る。
/// @note 解いた座標が板の外へ出るときは残る 2 軸を中心へ縮める (縁へ寄るぶん一様からずれるが、必ず板の中に入る)。
/// @note 軸に沿った板 (法線が z か y) では解く座標が常に 0 になり、断面の中で一様になる。
math::Vector3 SampleTexturePlate(const FluidSource& source, float u0, float u1, float u2, bool isVolumetric)
{
    math::Vector3 right;
    math::Vector3 up;
    math::Vector3 normal;
    FluidTextureSourceBasis(source, right, up, normal);
    const math::Vector3 axes[3] = { right * (std::max)(source.size.x, 0.0f), up * (std::max)(source.size.y, 0.0f),
                                    normal * (std::max)(source.size.z, 0.0f) };
    float t[3] = { 2.0f * u0 - 1.0f, 2.0f * u1 - 1.0f, 2.0f * u2 - 1.0f };
    if (!isVolumetric) {
        int solvedAxis = 0;
        for (int k = 1; k < 3; ++k)
            if (std::fabs(axes[k].z) > std::fabs(axes[solvedAxis].z)) solvedAxis = k;
        if (std::fabs(axes[solvedAxis].z) > 1.0e-9f) {
            float otherAxesDepth = 0.0f;
            for (int k = 0; k < 3; ++k)
                if (k != solvedAxis) otherAxesDepth += axes[k].z * t[k];
            float value = -otherAxesDepth / axes[solvedAxis].z;
            if (std::fabs(value) > PLATE_INTERIOR_LIMIT) {
                const float plateScale = PLATE_INTERIOR_LIMIT / std::fabs(value);
                for (int k = 0; k < 3; ++k)
                    if (k != solvedAxis) t[k] *= plateScale;
                value *= plateScale;
            }
            t[solvedAxis] = value;
        }
    }
    math::Vector3 offset = axes[0] * t[0] + axes[1] * t[1] + axes[2] * t[2];
    if (!isVolumetric) offset.z = 0.0f;
    return offset;
}

math::Vector3 SampleSegment3D(const SegmentFrame& frame, bool capped, float u0, float u1, float u2)
{
    math::Vector3 firstBasis;
    math::Vector3 secondBasis;
    OrthonormalBasis(frame.axis, firstBasis, secondBasis);
    const auto disc = [&](float r, float u) {
        const float angle = 2.0f * FLUID_PI * u;
        return firstBasis * (r * std::cos(angle)) + secondBasis * (r * std::sin(angle));
    };
    /// @note 断面積は半径の 2 乗に比例するので、r = R√u で面一様になる。
    const float radial = frame.radius * std::sqrt(u1);
    if (capped) return frame.axis * (frame.halfLength * (2.0f * u0 - 1.0f)) + disc(radial, u2);
    /// @note 両端の半球を合わせると球 1 つ。球と円柱を体積比で選び分ければ、カプセル全体で一様になる。
    const float tube = 2.0f * FLUID_PI * frame.radius * frame.radius * frame.halfLength;
    const float ball = 4.0f / 3.0f * FLUID_PI * frame.radius * frame.radius * frame.radius;
    const float total = tube + ball;
    if (total <= 0.0f) return { 0.0f, 0.0f, 0.0f };
    const float pick = u0 * total;
    if (pick < tube) {
        const float u = tube > 0.0f ? pick / tube : 0.0f;
        return frame.axis * (frame.halfLength * (2.0f * u - 1.0f)) + disc(radial, u2);
    }
    const float u = ball > 0.0f ? (pick - tube) / ball : 0.0f;
    const float r = frame.radius * std::cbrt(u);
    const float cosTheta = 1.0f - 2.0f * u1;
    const float sinTheta = std::sqrt((std::max)(0.0f, 1.0f - cosTheta * cosTheta));
    /// @note 球を軸で半分に割り、軸方向の符号の側の端へ寄せる (半球が端の半球そのものになる)。
    return frame.axis * (r * cosTheta + (cosTheta >= 0.0f ? frame.halfLength : -frame.halfLength)) + disc(r * sinTheta, u2);
}

math::Vector3 SampleSegment2D(const SegmentFrame& frame, bool capped, float u0, float u1)
{
    const FlatSegment flat = MakeFlatSegment(frame, capped);
    if (flat.extent <= 0.0f || flat.extent >= UNBOUNDED_SECTION_EXTENT) return { 0.0f, 0.0f, 0.0f };
    const float alpha = flat.extent * (2.0f * u0 - 1.0f);
    const float beta = FlatSegmentHalfWidth(frame, flat, capped, alpha) * (2.0f * u1 - 1.0f);
    const math::Vector3 offset = flat.along * alpha + flat.across * beta;
    return { offset.x, offset.y, 0.0f };
}

} /// @note namespace

math::Vector3 SampleFluidSourcePoint(const FluidSource& source, const math::Vector3& center,
                                     float u0, float u1, float u2, bool isVolumetric)
{
    const float sx = (std::max)(source.size.x, 0.0f);
    const float sy = (std::max)(source.size.y, 0.0f);
    const float sz = (std::max)(source.size.z, 0.0f);
    math::Vector3 offset = { 0.0f, 0.0f, 0.0f };
    switch (source.shape) {
    case FluidSourceShape::Box:
        offset = { sx * (2.0f * u0 - 1.0f), sy * (2.0f * u1 - 1.0f), isVolumetric ? sz * (2.0f * u2 - 1.0f) : 0.0f };
        break;
    case FluidSourceShape::Cone:
        offset = isVolumetric ? SampleCone3D(source, sx, sy, u0, u1, u2) : SampleCone2D(source, sx, sy, u0, u1);
        break;
    case FluidSourceShape::Ring:
        offset = isVolumetric ? SampleRing3D(source, sx, sy, u0, u1, u2) : SampleRing2D(source, sx, sy, u0, u1, u2);
        break;
    case FluidSourceShape::Texture:
        offset = SampleTexturePlate(source, u0, u1, u2, isVolumetric);
        break;
    case FluidSourceShape::Capsule:
    case FluidSourceShape::Cylinder: {
        const bool capped = source.shape == FluidSourceShape::Cylinder;
        const SegmentFrame frame = MakeSegmentFrame(source.direction, { sx, sy, sz }, 0.0f);
        offset = isVolumetric ? SampleSegment3D(frame, capped, u0, u1, u2) : SampleSegment2D(frame, capped, u0, u1);
        break;
    }
    case FluidSourceShape::Sphere:
    default:
        if (isVolumetric) {
            const float r = sx * std::cbrt(u0);
            const float cosTheta = 1.0f - 2.0f * u1;
            const float sinTheta = std::sqrt((std::max)(0.0f, 1.0f - cosTheta * cosTheta));
            const float phi = 2.0f * FLUID_PI * u2;
            offset = { r * sinTheta * std::cos(phi), r * sinTheta * std::sin(phi), r * cosTheta };
        } else {
            const float r = sx * std::sqrt(u0);
            const float angle = 2.0f * FLUID_PI * u1;
            offset = { r * std::cos(angle), r * std::sin(angle), 0.0f };
        }
        break;
    }
    math::Vector3 point = center + offset;
    if (!isVolumetric) point.z = center.z;
    return point;
}

void FluidTextureSourceBasis(const FluidSource& source, math::Vector3& outRight, math::Vector3& outUp,
                             math::Vector3& outNormal)
{
    outNormal = NormalizeOr(source.direction, math::Vector3{ 0.0f, 0.0f, 1.0f });
    if (std::fabs(outNormal.y) < 0.99f)
        outRight = NormalizeOr(math::Vector3::Cross(math::Vector3{ 0.0f, 1.0f, 0.0f }, outNormal),
                               math::Vector3{ 1.0f, 0.0f, 0.0f });
    else
        outRight = { 1.0f, 0.0f, 0.0f };
    outUp = math::Vector3::Cross(outNormal, outRight);
}

bool SampleFluidTextureSourcePoint(const FluidSource& source, const math::Vector3& center,
                                   const FluidSourceMask& mask, float u0, float u1, float u2,
                                   float accept, bool isVolumetric, math::Vector3& outPoint)
{
    const math::Vector3 offset = SampleTexturePlate(source, u0, u1, u2, isVolumetric);
    math::Vector3 right;
    math::Vector3 up;
    math::Vector3 normal;
    FluidTextureSourceBasis(source, right, up, normal);
    const float sx = (std::max)(source.size.x, 0.0f);
    const float sy = (std::max)(source.size.y, 0.0f);
    const float a = sx > 0.0f ? ScalarDot(offset, right) / sx : 0.0f;
    const float b = sy > 0.0f ? ScalarDot(offset, up) / sy : 0.0f;
    if (accept >= SampleFluidSourceMask(mask, (a + 1.0f) * 0.5f, (1.0f - b) * 0.5f)) return false;
    outPoint = center + offset;
    if (!isVolumetric) outPoint.z = center.z;
    return true;
}

} /// @note namespace fbzz::fluid
