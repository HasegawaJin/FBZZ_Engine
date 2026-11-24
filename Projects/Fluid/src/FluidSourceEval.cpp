/// @file    FluidSourceEval.cpp
/// @brief   発生源の重みと液体粒子を収める寸法を求める。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Fluid/FluidOperatorEval.hpp>
#include "FluidOperatorGeometry.hpp"
#include <algorithm>
#include <cmath>

namespace fbzz::fluid {

namespace {

using namespace operator_geometry_detail;

/// @note 発生源へ一度に詰めてよい密度 (静止密度の何倍か)。1 だと噴き上げが柱のように素直になり、
/// @note 飛沫の «弾け» が消える。
constexpr float EMISSION_PACKING = 2.0f;

/// @note 形の大きさ。3D は体積、2D は z = center.z の断面の面積 (SampleFluidSourcePoint が撒く範囲)。
/// @note size は粒子半径まで広げた後の寸法 (各成分 0 以上)。
[[nodiscard]] float ShapeMeasure(const FluidSource& source, const math::Vector3& size, bool isVolumetric)
{
    const float sx = size.x;
    const float sy = size.y;
    const float sz = size.z;
    switch (source.shape) {
    case FluidSourceShape::Box:
        return isVolumetric ? 8.0f * sx * sy * sz : 4.0f * sx * sy;
    case FluidSourceShape::Cone: {
        if (isVolumetric) return FLUID_PI * sx * sx * sy / 3.0f;
        /// @note 断面は頂点から伸びる三角形 (長さ L/s・底の半幅 L/s·√g。s は軸の画面内成分)。
        const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
        const float planar = std::sqrt(axis.x * axis.x + axis.y * axis.y);
        if (sy <= 0.0f || planar < 1.0e-6f) return 0.0f;
        const float slope = sx / sy;
        const float g = (1.0f + slope * slope) * planar * planar - 1.0f;
        if (g <= 0.0f) return 0.0f;
        const float planarLength = sy / planar;
        return planarLength * planarLength * std::sqrt(g);
    }
    case FluidSourceShape::Ring: {
        if (isVolumetric) return 2.0f * FLUID_PI * FLUID_PI * sx * sy * sy;
        /// @note 断面は «輪の各点を中心とする管の球» を平面で切った円の帯。輪に沿って幅を積分する。
        /// @note 輪が画面から立ち上がる割合 tilt で、輪の点の奥行きは sx·tilt·cosψ になる。
        const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
        const float tilt = std::sqrt((std::max)(0.0f, 1.0f - axis.z * axis.z));
        constexpr int kSteps = 64;
        const float step = 2.0f * FLUID_PI / static_cast<float>(kSteps);
        float area = 0.0f;
        for (int k = 0; k < kSteps; ++k) {
            const float depth = sx * tilt * std::cos((static_cast<float>(k) + 0.5f) * step);
            area += 2.0f * std::sqrt((std::max)(sy * sy - depth * depth, 0.0f)) * sx * step;
        }
        return area;
    }
    case FluidSourceShape::Texture: {
        if (isVolumetric) return 8.0f * sx * sy * sz;
        /// @note 板の箱の z = center.z の断面。各面の z 向きの割合で配分する (法線が z か y の板なら正確)。
        math::Vector3 right;
        math::Vector3 up;
        math::Vector3 normal;
        FluidTextureSourceBasis(source, right, up, normal);
        return 4.0f * (sx * sy * std::fabs(normal.z) + sx * sz * std::fabs(up.z) + sy * sz * std::fabs(right.z));
    }
    case FluidSourceShape::Capsule:
    case FluidSourceShape::Cylinder: {
        const bool capped = source.shape == FluidSourceShape::Cylinder;
        const SegmentFrame frame = MakeSegmentFrame(source.direction, size, 0.0f);
        if (isVolumetric) {
            const float tube = 2.0f * FLUID_PI * sx * sx * sy;
            return capped ? tube : tube + 4.0f / 3.0f * FLUID_PI * sx * sx * sx;
        }
        /// @note 断面の半幅は閉じた式で書けるが、面積 (その積分) は «楕円を切った帯 + 端の円の一部» なので
        /// @note 場合分けが要る。Ring と同じく刻んで足す (SampleSegment2D が撒く範囲とも同じ式になる)。
        const FlatSegment flat = MakeFlatSegment(frame, capped);
        if (flat.extent <= 0.0f || flat.extent >= UNBOUNDED_SECTION_EXTENT) return 0.0f;
        constexpr int kSteps = 64;
        const float step = 2.0f * flat.extent / static_cast<float>(kSteps);
        float area = 0.0f;
        for (int k = 0; k < kSteps; ++k) {
            const float alpha = -flat.extent + (static_cast<float>(k) + 0.5f) * step;
            area += 2.0f * FlatSegmentHalfWidth(frame, flat, capped, alpha) * step;
        }
        return area;
    }
    case FluidSourceShape::Sphere:
    default:
        return isVolumetric ? 4.0f / 3.0f * FLUID_PI * sx * sx * sx : FLUID_PI * sx * sx;
    }
}

/// @note 形を direction (単位ベクトル) の向きに測った厚み。撃ち出した粒が発生源を抜けるまでの距離に使う。
[[nodiscard]] float ShapeWidthAlong(const FluidSource& source, const math::Vector3& size,
                                    const math::Vector3& direction)
{
    const float sx = size.x;
    const float sy = size.y;
    const float sz = size.z;
    switch (source.shape) {
    case FluidSourceShape::Box:
        return 2.0f * (std::fabs(direction.x) * sx + std::fabs(direction.y) * sy + std::fabs(direction.z) * sz);
    case FluidSourceShape::Texture: {
        math::Vector3 right;
        math::Vector3 up;
        math::Vector3 normal;
        FluidTextureSourceBasis(source, right, up, normal);
        return 2.0f * (std::fabs(ScalarDot(direction, right)) * sx
                       + std::fabs(ScalarDot(direction, up)) * sy
                       + std::fabs(ScalarDot(direction, normal)) * sz);
    }
    case FluidSourceShape::Cone:
    case FluidSourceShape::Ring: {
        const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
        const float along = direction.x * axis.x + direction.y * axis.y + direction.z * axis.z;
        const float across = std::sqrt((std::max)(0.0f, 1.0f - along * along));
        if (source.shape == FluidSourceShape::Ring) return 2.0f * (sx * across + sy);
        /// @note 円錐を包む «頂点と底の円» の支持関数を両向きで足す。
        return (std::max)(0.0f, sy * along + sx * across) + (std::max)(0.0f, -sy * along + sx * across);
    }
    case FluidSourceShape::Capsule:
    case FluidSourceShape::Cylinder: {
        const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
        const float along = direction.x * axis.x + direction.y * axis.y + direction.z * axis.z;
        /// @note 芯の線分の厚み + 肉の厚み。カプセルは肉が球なのでどの向きでも 2R、円柱は円盤なので軸に直交する分だけ。
        if (source.shape == FluidSourceShape::Capsule) return 2.0f * (sy * std::fabs(along) + sx);
        const float across = std::sqrt((std::max)(0.0f, 1.0f - along * along));
        return 2.0f * (sy * std::fabs(along) + sx * across);
    }
    case FluidSourceShape::Sphere:
    default:
        return 2.0f * sx;
    }
}

/// @note Texture の板の座標 (a, b, c)。どれかの絶対値が 1 以上 (板の外) なら false。
[[nodiscard]] bool TexturePlateCoordinates(const FluidSource& source, const math::Vector3& center,
                                           const math::Vector3& point, float minimumSize, bool isVolumetric,
                                           float& outPlateX, float& outPlateY, float& outPlateDepth)
{
    math::Vector3 centerOffset = point - center;
    if (!isVolumetric) centerOffset.z = 0.0f;
    math::Vector3 right;
    math::Vector3 up;
    math::Vector3 normal;
    FluidTextureSourceBasis(source, right, up, normal);
    const float halfWidth = (std::max)(source.size.x, minimumSize);
    const float halfHeight = (std::max)(source.size.y, minimumSize);
    const float halfDepth = (std::max)(source.size.z, minimumSize);
    if (halfWidth <= 0.0f || halfHeight <= 0.0f || halfDepth <= 0.0f) return false;
    outPlateX = ScalarDot(centerOffset, right) / halfWidth;
    outPlateY = ScalarDot(centerOffset, up) / halfHeight;
    outPlateDepth = ScalarDot(centerOffset, normal) / halfDepth;
    return std::fabs(outPlateX) < 1.0f && std::fabs(outPlateY) < 1.0f && std::fabs(outPlateDepth) < 1.0f;
}

} /// @note namespace

float FluidSourceWeight(const FluidSource& source, const math::Vector3& center, const math::Vector3& point,
                        float minimumSize, bool isVolumetric)
{
    math::Vector3 centerOffset = point - center;
    if (!isVolumetric) centerOffset.z = 0.0f;

    switch (source.shape) {
    case FluidSourceShape::Box: {
        const float halfWidth = (std::max)(source.size.x, minimumSize);
        const float halfHeight = (std::max)(source.size.y, minimumSize);
        const float halfDepth = (std::max)(source.size.z, minimumSize);
        /// @note 寸法 0 の軸は 0/0 になる。広げる幅 (minSize) も 0 のときだけ起きる。
        if (halfWidth <= 0.0f || halfHeight <= 0.0f || halfDepth <= 0.0f) return 0.0f;
        const float q = (std::max)(std::fabs(centerOffset.x) / halfWidth, (std::max)(std::fabs(centerOffset.y) / halfHeight, std::fabs(centerOffset.z) / halfDepth));
        if (q >= 1.0f) return 0.0f;
        return Saturate((1.0f - q) * 4.0f);
    }
    case FluidSourceShape::Cone: {
        const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
        const float length = (std::max)(source.size.y, minimumSize);
        const float radius = (std::max)(source.size.x, minimumSize);
        if (length <= 0.0f) return 0.0f;
        const float t = ScalarDot(centerOffset, axis);
        if (t < 0.0f || t > length) return 0.0f;
        const math::Vector3 radial = centerOffset - axis * t;
        const float localRadius = (std::max)(radius * t / length, minimumSize * 0.5f);
        if (localRadius <= 0.0f) return 0.0f;
        const float normalizedDistanceSquared = ScalarDot(radial, radial) / (localRadius * localRadius);
        if (normalizedDistanceSquared >= 1.0f) return 0.0f;
        return (1.0f - normalizedDistanceSquared) * (1.0f - normalizedDistanceSquared) * Saturate((1.0f - t / length) * 4.0f);
    }
    case FluidSourceShape::Ring: {
        const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
        const float ringRadius = (std::max)(source.size.x, minimumSize);
        const float tubeRadius = (std::max)(source.size.y, minimumSize);
        if (tubeRadius <= 0.0f) return 0.0f;
        const float axisDistance = ScalarDot(centerOffset, axis);
        const math::Vector3 radial = centerOffset - axis * axisDistance;
        const float distanceFromTubeCenter = std::sqrt(ScalarDot(radial, radial)) - ringRadius;
        const float normalizedDistanceSquared = (distanceFromTubeCenter * distanceFromTubeCenter + axisDistance * axisDistance) / (tubeRadius * tubeRadius);
        if (normalizedDistanceSquared >= 1.0f) return 0.0f;
        return (1.0f - normalizedDistanceSquared) * (1.0f - normalizedDistanceSquared);
    }
    case FluidSourceShape::Texture: {
        /// @note マスク 1 (= 板の形そのもの)。画像の濃さを見るのは FluidTextureSourceWeight。
        float a = 0.0f;
        float b = 0.0f;
        float c = 0.0f;
        if (!TexturePlateCoordinates(source, center, point, minimumSize, isVolumetric, a, b, c)) return 0.0f;
        return Saturate((1.0f - std::fabs(c)) * 4.0f);
    }
    case FluidSourceShape::Capsule: {
        const SegmentFrame frame = MakeSegmentFrame(source.direction, source.size, minimumSize);
        if (frame.radius <= 0.0f) return 0.0f;
        const float clampedAxisDistance = std::clamp(ScalarDot(centerOffset, frame.axis), -frame.halfLength, frame.halfLength);
        const math::Vector3 radial = centerOffset - frame.axis * clampedAxisDistance;
        const float normalizedDistanceSquared = ScalarDot(radial, radial) / (frame.radius * frame.radius);
        if (normalizedDistanceSquared >= 1.0f) return 0.0f;
        return (1.0f - normalizedDistanceSquared) * (1.0f - normalizedDistanceSquared);
    }
    case FluidSourceShape::Cylinder: {
        const SegmentFrame frame = MakeSegmentFrame(source.direction, source.size, minimumSize);
        if (frame.radius <= 0.0f || frame.halfLength <= 0.0f) return 0.0f;
        const float axisDistance = ScalarDot(centerOffset, frame.axis);
        if (std::fabs(axisDistance) > frame.halfLength) return 0.0f;
        const math::Vector3 radial = centerOffset - frame.axis * axisDistance;
        const float normalizedDistanceSquared = ScalarDot(radial, radial) / (frame.radius * frame.radius);
        if (normalizedDistanceSquared >= 1.0f) return 0.0f;
        return (1.0f - normalizedDistanceSquared) * (1.0f - normalizedDistanceSquared) * Saturate((1.0f - std::fabs(axisDistance) / frame.halfLength) * 4.0f);
    }
    case FluidSourceShape::Sphere:
    default: {
        const float radius = (std::max)(source.size.x, minimumSize);
        if (radius <= 0.0f) return 0.0f;
        /// @note 旧ソルバーと同じく軸ごとに割ってから 2 乗和を取る (焼き結果を 1 ビットも変えない)。
        const float dx = centerOffset.x / radius;
        const float dy = centerOffset.y / radius;
        const float dz = centerOffset.z / radius;
        const float normalizedDistanceSquared = dx * dx + dy * dy + dz * dz;
        if (normalizedDistanceSquared >= 1.0f) return 0.0f;
        return (1.0f - normalizedDistanceSquared) * (1.0f - normalizedDistanceSquared);
    }
    }
}

float FluidTextureSourceWeight(const FluidSource& source, const math::Vector3& center, const math::Vector3& point,
                               float minimumSize, bool isVolumetric, const FluidSourceMask& mask)
{
    float a = 0.0f;
    float b = 0.0f;
    float c = 0.0f;
    if (!TexturePlateCoordinates(source, center, point, minimumSize, isVolumetric, a, b, c)) return 0.0f;
    return SampleFluidSourceMask(mask, (a + 1.0f) * 0.5f, (1.0f - b) * 0.5f) * Saturate((1.0f - std::fabs(c)) * 4.0f);
}

/// @brief 発生源を広げる倍率。«同時に発生源の中に居る粒» が kEmitPacking まで詰めて収まるよう、形を
/// @note 中心から相似に拡大する倍率を返す (球なら max(半径, 必要な半径) / 半径 と一致)。ソルバーは
/// @note ここを唯一の窓口として呼ぶ (動きの速さは velocity に足した写しを渡す)。
/// @note 決まった数を狭い形へ一度に出すと、静止密度の何十倍にも詰まった粒子を密度拘束が押し広げ、
/// @note 発生直後に爆ぜて領域外へ飛び去る (Blood Burst は半径 0.06 に 650 粒 = 入る数の 30 倍だった)。
float FluidLiquidEmitScale(const FluidSource& source, const FluidLiquidSettings& liquid, bool isVolumetric)
{
    const float radius = std::clamp(liquid.particleRadius, 0.002f, 0.1f);
    /// @note ソルバーが Reset で寸法を粒子半径まで広げるのと同じ形で測る (既に広げてあっても結果は変わらない)。
    const math::Vector3 size = { (std::max)(source.size.x, radius), (std::max)(source.size.y, radius),
                                 (std::max)(source.size.z, radius) };
    math::Vector3 launchVelocity = source.velocity;
    if (!isVolumetric) launchVelocity.z = 0.0f;
    const float count = static_cast<float>((std::max)(source.count, 0));
    const float speed = launchVelocity.Length();
    const math::Vector3 direction = speed > 1.0e-6f ? launchVelocity * (1.0f / speed) : math::Vector3{ 0.0f, 1.0f, 0.0f };
    const float width = ShapeWidthAlong(source, size, direction);
    /// @note 出している間に先頭の粒が発生源を抜けるなら、中に居るのは «抜けるまでに出た分» だけ。
    const float travel = speed * source.duration;
    float particlesInside = (source.duration <= 0.0f || travel <= width) ? count : count * width / travel;
    particlesInside /= EMISSION_PACKING;
    const float spacing = 2.0f * radius;
    const float requiredMeasure = isVolumetric ? particlesInside * spacing * spacing * spacing : particlesInside * spacing * spacing;
    const float measure = ShapeMeasure(source, size, isVolumetric);
    if (measure <= 1.0e-12f || requiredMeasure <= measure) return 1.0f;
    return isVolumetric ? std::cbrt(requiredMeasure / measure) : std::sqrt(requiredMeasure / measure);
}

} /// @note namespace fbzz::fluid
