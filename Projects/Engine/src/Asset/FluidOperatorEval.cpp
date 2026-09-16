/// @file    FluidOperatorEval.cpp
/// @brief   流体レシピの部品 (発生源・力・動き・障害物) の評価式の実装
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// FluidGpuCommon.hlsli と 1:1 で対応させるため、式はヘッダーのコメントの順番どおりに書く
/// (演算の順が変わると CPU と GPU の焼き結果が浮動小数点の丸めの分だけずれる)。
#include <Engine/Asset/FluidOperatorEval.hpp>

#include <Engine/Core/CurlNoise.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::asset {
namespace {

constexpr float kPi = 3.14159265358979323846f;
// これより短い向きは «長さ 0» として既定の向きへ倒す。
constexpr float kMinDirectionLengthSq = 1.0e-12f;

[[nodiscard]] float Saturate(float value) { return std::clamp(value, 0.0f, 1.0f); }

[[nodiscard]] float Dot(const math::Vector3& a, const math::Vector3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] math::Vector3 NormalizeOr(const math::Vector3& v, const math::Vector3& fallback)
{
    const float lengthSq = Dot(v, v);
    if (lengthSq < kMinDirectionLengthSq) return fallback;
    return v * (1.0f / std::sqrt(lengthSq));
}

[[nodiscard]] bool InWindow(bool enabled, float startTime, float duration, float time)
{
    if (!enabled || time < startTime) return false;
    return duration <= 0.0f || time < startTime + duration;
}

// axis に直交する正規直交基底。どの 1 組でも形は同じなので、決まった手順で選ぶ。
void OrthonormalBasis(const math::Vector3& axis, math::Vector3& outE1, math::Vector3& outE2)
{
    const math::Vector3 helper = std::fabs(axis.x) < 0.9f ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                                                          : math::Vector3{ 0.0f, 1.0f, 0.0f };
    outE1 = NormalizeOr(math::Vector3::Cross(axis, helper), math::Vector3{ 0.0f, 0.0f, 1.0f });
    outE2 = math::Vector3::Cross(axis, outE1);
}

math::Vector3 SampleCone3D(const FluidSource& source, float radius, float length, float u0, float u1, float u2)
{
    const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
    math::Vector3 e1;
    math::Vector3 e2;
    OrthonormalBasis(axis, e1, e2);
    // 断面積は t² に比例するので、t = L·u^(1/3) で体積一様になる。
    const float t = length * std::cbrt(u0);
    const float allowed = length > 0.0f ? radius * t / length : 0.0f;
    const float r = allowed * std::sqrt(u1);
    const float angle = 2.0f * kPi * u2;
    return axis * t + e1 * (r * std::cos(angle)) + e2 * (r * std::sin(angle));
}

// 頂点を通る z = center.z の平面で円錐を切ると、頂点から伸びる三角形になる。
// 軸の画面内成分を s、傾き k = R/L とすると、画面内の軸方向に長さ L/s、底の半幅 (L/s)·√((1+k²)s² − 1)。
// s = 1 (軸が画面内) なら長さ L・半幅 R の三角形。g <= 0 なら断面は頂点だけで、中心を返す。
math::Vector3 SampleCone2D(const FluidSource& source, float radius, float length, float u0, float u1)
{
    const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
    const float planar = std::sqrt(axis.x * axis.x + axis.y * axis.y);
    if (length <= 0.0f || planar < 1.0e-6f) return { 0.0f, 0.0f, 0.0f };
    const float slope = radius / length;
    const float g = (1.0f + slope * slope) * planar * planar - 1.0f;
    if (g <= 0.0f) return { 0.0f, 0.0f, 0.0f };
    const float planarLength = length / planar;
    // 三角形の幅は頂点からの距離に比例するので、√u で面積一様になる。
    const float along = planarLength * std::sqrt(u0);
    const float across = along * std::sqrt(g) * (2.0f * u1 - 1.0f);
    const float ax = axis.x / planar;
    const float ay = axis.y / planar;
    return { ax * along - ay * across, ay * along + ax * across, 0.0f };
}

math::Vector3 SampleRing3D(const FluidSource& source, float ringRadius, float tubeRadius, float u0, float u1, float u2)
{
    const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
    math::Vector3 e1;
    math::Vector3 e2;
    OrthonormalBasis(axis, e1, e2);
    const float theta = 2.0f * kPi * u0;
    const float tube = tubeRadius * std::sqrt(u1);
    const float psi = 2.0f * kPi * u2;
    const math::Vector3 outward = e1 * std::cos(theta) + e2 * std::sin(theta);
    return outward * (ringRadius + tube * std::cos(psi)) + axis * (tube * std::sin(psi));
}

// z = center.z の断面は «輪の各点を中心とする半径 r の球» を平面で切った円の和になる。
// 輪の点の奥行きは R·tilt·cos(θ − θ0) なので、|奥行き| < r の弧だけを選び、その点の断面円の中から取る。
// 輪が画面内に寝ていれば (tilt·R <= r) 弧は全周になり、輪帯になる。
math::Vector3 SampleRing2D(const FluidSource& source, float ringRadius, float tubeRadius, float u0, float u1, float u2)
{
    const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
    math::Vector3 e1;
    math::Vector3 e2;
    OrthonormalBasis(axis, e1, e2);
    const float tilt = std::sqrt(e1.z * e1.z + e2.z * e2.z);
    const float reach = ringRadius * tilt;
    const float halfArc = reach > tubeRadius ? std::asin(tubeRadius / reach) : kPi * 0.5f;
    const float phase = std::atan2(e2.z, e1.z);
    const bool second = u0 >= 0.5f;
    const float local = second ? u0 * 2.0f - 1.0f : u0 * 2.0f;
    const float theta = phase + kPi * 0.5f + (second ? kPi : 0.0f) + (2.0f * local - 1.0f) * halfArc;
    const math::Vector3 onRing = (e1 * std::cos(theta) + e2 * std::sin(theta)) * ringRadius;
    const float disc = std::sqrt((std::max)(tubeRadius * tubeRadius - onRing.z * onRing.z, 0.0f)) * std::sqrt(u1);
    const float angle = 2.0f * kPi * u2;
    return { onRing.x + disc * std::cos(angle), onRing.y + disc * std::sin(angle), 0.0f };
}

// sign(0) を +1 に倒す。0 を返すと箱の中心ちょうどで法線が長さ 0 になり、押し出す向きが無くなる。
[[nodiscard]] float SignOrPositive(float value) { return value < 0.0f ? -1.0f : 1.0f; }

// 解いた座標をこの内側に収める。ちょうど 1 だと板の縁で重みが 0 になり、湧かせた点が «形の外» 扱いになる。
constexpr float kPlateSolveLimit = 0.999f;

// Texture の板の中の点 (中心からのずれ)。3D は箱の中で一様。
// 2D は z = 0 の断面から選ぶ: z 成分が最も大きい軸の座標を «断面に乗る» ように解き、残る 2 軸は一様に取る。
// 解いた座標が板の外へ出るときは残る 2 軸を中心へ縮める (縁へ寄るぶん一様からずれるが、必ず板の中に入る)。
// 軸に沿った板 (法線が z か y) では解く座標が常に 0 になり、断面の中で一様になる。
math::Vector3 SampleTexturePlate(const FluidSource& source, float u0, float u1, float u2, bool volumetric)
{
    math::Vector3 right;
    math::Vector3 up;
    math::Vector3 normal;
    FluidTextureSourceBasis(source, right, up, normal);
    const math::Vector3 axes[3] = { right * (std::max)(source.size.x, 0.0f), up * (std::max)(source.size.y, 0.0f),
                                    normal * (std::max)(source.size.z, 0.0f) };
    float t[3] = { 2.0f * u0 - 1.0f, 2.0f * u1 - 1.0f, 2.0f * u2 - 1.0f };
    if (!volumetric) {
        int solved = 0;
        for (int k = 1; k < 3; ++k)
            if (std::fabs(axes[k].z) > std::fabs(axes[solved].z)) solved = k;
        if (std::fabs(axes[solved].z) > 1.0e-9f) {
            float rest = 0.0f;
            for (int k = 0; k < 3; ++k)
                if (k != solved) rest += axes[k].z * t[k];
            float value = -rest / axes[solved].z;
            if (std::fabs(value) > kPlateSolveLimit) {
                const float shrink = kPlateSolveLimit / std::fabs(value);
                for (int k = 0; k < 3; ++k)
                    if (k != solved) t[k] *= shrink;
                value *= shrink;
            }
            t[solved] = value;
        }
    }
    math::Vector3 offset = axes[0] * t[0] + axes[1] * t[1] + axes[2] * t[2];
    if (!volumetric) offset.z = 0.0f;
    return offset;
}

// ── カプセルと円柱 (芯の線分をもつ形) ──

// 芯の線分 center ± axis × half と、その周りの肉の半径。
struct SegmentFrame {
    math::Vector3 axis{ 0.0f, 1.0f, 0.0f };
    float radius = 0.0f;
    float half = 0.0f;
};

SegmentFrame MakeSegmentFrame(const math::Vector3& direction, const math::Vector3& size, float minSize)
{
    SegmentFrame frame;
    frame.axis = NormalizeOr(direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
    frame.radius = (std::max)(size.x, minSize);
    frame.half = (std::max)(size.y, minSize);
    return frame;
}

// 芯の上で «外向き» が決まらないときに返す向き。軸に直交する 1 本を決まった手順で選ぶ
// (up を返すと軸が上向きのとき芯に沿って押してしまう)。
math::Vector3 SegmentFallbackNormal(const math::Vector3& axis)
{
    math::Vector3 e1;
    math::Vector3 e2;
    OrthonormalBasis(axis, e1, e2);
    return e1;
}

// z = center.z の断面を測るための面内の枠。α = 面内の軸方向、β = それに直交する向き。
// 軸を a、その面内成分の長さを s、奥行き成分を k = |a.z| (s² + k² = 1) とすると、断面の点 α a∥ + β a⊥ から
// 芯への距離は «中ほど (|α s| <= H) なら √(α²k² + β²)»、«端を過ぎたら √((|α| − H s)² + β² + H²k²)»。
// 傾いた棒を平面で切ると細い楕円になり、寝かせれば «2D のカプセル» になる — その両方をこの式が持つ。
struct FlatSegment {
    math::Vector3 along{ 1.0f, 0.0f, 0.0f };
    math::Vector3 across{ 0.0f, 1.0f, 0.0f };
    float planar = 0.0f;   // s
    float depth = 1.0f;    // k
    float extent = 0.0f;   // |α| の上限 (断面の端)
};

// 面内で解けない大きさ。s か k が 0 のとき «その向きには切れない» ことを表す。
constexpr float kFlatSegmentUnbounded = 1.0e9f;

// capped = 端が平ら (円柱)。false なら端に半球が付く (カプセル)。
FlatSegment MakeFlatSegment(const SegmentFrame& frame, bool capped)
{
    FlatSegment flat;
    const math::Vector3& a = frame.axis;
    flat.planar = std::sqrt(a.x * a.x + a.y * a.y);
    flat.depth = std::fabs(a.z);
    if (flat.planar > 1.0e-6f) {
        flat.along = { a.x / flat.planar, a.y / flat.planar, 0.0f };
        flat.across = { -flat.along.y, flat.along.x, 0.0f };
    }
    if (frame.radius <= 0.0f) return flat;
    const float alongExtent = flat.planar > 1.0e-6f ? frame.half / flat.planar : kFlatSegmentUnbounded;
    const float acrossExtent = flat.depth > 1.0e-6f ? frame.radius / flat.depth : kFlatSegmentUnbounded;
    if (capped) {
        flat.extent = (std::min)(alongExtent, acrossExtent);
        return flat;
    }
    // 端の球が断面に顔を出すのは R s > H k のときだけ (そのとき断面は «楕円を切った帯 + 端の円の一部»)。
    // 出ないときは楕円がそのまま断面になる。
    flat.extent = frame.radius * flat.planar > frame.half * flat.depth
        ? frame.half * flat.planar + std::sqrt((std::max)(frame.radius * frame.radius
                                                          - frame.half * frame.half * flat.depth * flat.depth, 0.0f))
        : acrossExtent;
    return flat;
}

// 断面の α における β の半幅 (形の外なら 0)。
[[nodiscard]] float FlatSegmentHalfWidth(const SegmentFrame& frame, const FlatSegment& flat, bool capped, float alpha)
{
    const float h = alpha * flat.planar;
    if (capped) {
        if (std::fabs(h) > frame.half) return 0.0f;
        const float inner = frame.radius * frame.radius - alpha * alpha * flat.depth * flat.depth;
        return inner > 0.0f ? std::sqrt(inner) : 0.0f;
    }
    const float hc = std::clamp(h, -frame.half, frame.half);
    const float t = alpha - hc * flat.planar;
    const float inner = frame.radius * frame.radius - t * t - hc * hc * flat.depth * flat.depth;
    return inner > 0.0f ? std::sqrt(inner) : 0.0f;
}

math::Vector3 SampleSegment3D(const SegmentFrame& frame, bool capped, float u0, float u1, float u2)
{
    math::Vector3 e1;
    math::Vector3 e2;
    OrthonormalBasis(frame.axis, e1, e2);
    const auto disc = [&](float r, float u) {
        const float angle = 2.0f * kPi * u;
        return e1 * (r * std::cos(angle)) + e2 * (r * std::sin(angle));
    };
    // 断面積は半径の 2 乗に比例するので、r = R√u で面一様になる。
    const float radial = frame.radius * std::sqrt(u1);
    if (capped) return frame.axis * (frame.half * (2.0f * u0 - 1.0f)) + disc(radial, u2);
    // 両端の半球を合わせると球 1 つ。球と円柱を体積比で選び分ければ、カプセル全体で一様になる。
    const float tube = 2.0f * kPi * frame.radius * frame.radius * frame.half;
    const float ball = 4.0f / 3.0f * kPi * frame.radius * frame.radius * frame.radius;
    const float total = tube + ball;
    if (total <= 0.0f) return { 0.0f, 0.0f, 0.0f };
    const float pick = u0 * total;
    if (pick < tube) {
        const float u = tube > 0.0f ? pick / tube : 0.0f;
        return frame.axis * (frame.half * (2.0f * u - 1.0f)) + disc(radial, u2);
    }
    const float u = ball > 0.0f ? (pick - tube) / ball : 0.0f;
    const float r = frame.radius * std::cbrt(u);
    const float cosTheta = 1.0f - 2.0f * u1;
    const float sinTheta = std::sqrt((std::max)(0.0f, 1.0f - cosTheta * cosTheta));
    // 球を軸で半分に割り、軸方向の符号の側の端へ寄せる (半球が端の半球そのものになる)。
    return frame.axis * (r * cosTheta + (cosTheta >= 0.0f ? frame.half : -frame.half)) + disc(r * sinTheta, u2);
}

math::Vector3 SampleSegment2D(const SegmentFrame& frame, bool capped, float u0, float u1)
{
    const FlatSegment flat = MakeFlatSegment(frame, capped);
    if (flat.extent <= 0.0f || flat.extent >= kFlatSegmentUnbounded) return { 0.0f, 0.0f, 0.0f };
    const float alpha = flat.extent * (2.0f * u0 - 1.0f);
    const float beta = FlatSegmentHalfWidth(frame, flat, capped, alpha) * (2.0f * u1 - 1.0f);
    const math::Vector3 offset = flat.along * alpha + flat.across * beta;
    return { offset.x, offset.y, 0.0f };
}

// 発生源へ一度に詰めてよい密度 (静止密度の何倍か)。1 だと噴き上げが柱のように素直になり、
// 飛沫の «弾け» が消える。
constexpr float kEmitPacking = 2.0f;

// 形の大きさ。3D は体積、2D は z = center.z の断面の面積 (SampleFluidSourcePoint が撒く範囲)。
// size は粒子半径まで広げた後の寸法 (各成分 0 以上)。
[[nodiscard]] float ShapeMeasure(const FluidSource& source, const math::Vector3& size, bool volumetric)
{
    const float sx = size.x;
    const float sy = size.y;
    const float sz = size.z;
    switch (source.shape) {
    case FluidSourceShape::Box:
        return volumetric ? 8.0f * sx * sy * sz : 4.0f * sx * sy;
    case FluidSourceShape::Cone: {
        if (volumetric) return kPi * sx * sx * sy / 3.0f;
        // 断面は頂点から伸びる三角形 (長さ L/s・底の半幅 L/s·√g。s は軸の画面内成分)。
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
        if (volumetric) return 2.0f * kPi * kPi * sx * sy * sy;
        // 断面は «輪の各点を中心とする管の球» を平面で切った円の帯。輪に沿って幅を積分する。
        // 輪が画面から立ち上がる割合 tilt で、輪の点の奥行きは sx·tilt·cosψ になる。
        const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
        const float tilt = std::sqrt((std::max)(0.0f, 1.0f - axis.z * axis.z));
        constexpr int kSteps = 64;
        const float step = 2.0f * kPi / static_cast<float>(kSteps);
        float area = 0.0f;
        for (int k = 0; k < kSteps; ++k) {
            const float depth = sx * tilt * std::cos((static_cast<float>(k) + 0.5f) * step);
            area += 2.0f * std::sqrt((std::max)(sy * sy - depth * depth, 0.0f)) * sx * step;
        }
        return area;
    }
    case FluidSourceShape::Texture: {
        if (volumetric) return 8.0f * sx * sy * sz;
        // 板の箱の z = center.z の断面。各面の z 向きの割合で配分する (法線が z か y の板なら正確)。
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
        if (volumetric) {
            const float tube = 2.0f * kPi * sx * sx * sy;
            return capped ? tube : tube + 4.0f / 3.0f * kPi * sx * sx * sx;
        }
        // 断面の半幅は閉じた式で書けるが、面積 (その積分) は «楕円を切った帯 + 端の円の一部» なので
        // 場合分けが要る。Ring と同じく刻んで足す (SampleSegment2D が撒く範囲とも同じ式になる)。
        const FlatSegment flat = MakeFlatSegment(frame, capped);
        if (flat.extent <= 0.0f || flat.extent >= kFlatSegmentUnbounded) return 0.0f;
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
        return volumetric ? 4.0f / 3.0f * kPi * sx * sx * sx : kPi * sx * sx;
    }
}

// 形を direction (単位ベクトル) の向きに測った厚み。撃ち出した粒が発生源を抜けるまでの距離に使う。
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
        return 2.0f * (std::fabs(Dot(direction, right)) * sx
                       + std::fabs(Dot(direction, up)) * sy
                       + std::fabs(Dot(direction, normal)) * sz);
    }
    case FluidSourceShape::Cone:
    case FluidSourceShape::Ring: {
        const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
        const float along = direction.x * axis.x + direction.y * axis.y + direction.z * axis.z;
        const float across = std::sqrt((std::max)(0.0f, 1.0f - along * along));
        if (source.shape == FluidSourceShape::Ring) return 2.0f * (sx * across + sy);
        // 円錐を包む «頂点と底の円» の支持関数を両向きで足す。
        return (std::max)(0.0f, sy * along + sx * across) + (std::max)(0.0f, -sy * along + sx * across);
    }
    case FluidSourceShape::Capsule:
    case FluidSourceShape::Cylinder: {
        const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
        const float along = direction.x * axis.x + direction.y * axis.y + direction.z * axis.z;
        // 芯の線分の厚み + 肉の厚み。カプセルは肉が球なのでどの向きでも 2R、円柱は円盤なので軸に直交する分だけ。
        if (source.shape == FluidSourceShape::Capsule) return 2.0f * (sy * std::fabs(along) + sx);
        const float across = std::sqrt((std::max)(0.0f, 1.0f - along * along));
        return 2.0f * (sy * std::fabs(along) + sx * across);
    }
    case FluidSourceShape::Sphere:
    default:
        return 2.0f * sx;
    }
}

// Texture の板の座標 (a, b, c)。どれかの絶対値が 1 以上 (板の外) なら false。
[[nodiscard]] bool TexturePlateCoordinates(const FluidSource& source, const math::Vector3& center,
                                           const math::Vector3& p, float minSize, bool volumetric,
                                           float& outA, float& outB, float& outC)
{
    math::Vector3 d = p - center;
    if (!volumetric) d.z = 0.0f;
    math::Vector3 right;
    math::Vector3 up;
    math::Vector3 normal;
    FluidTextureSourceBasis(source, right, up, normal);
    const float hx = (std::max)(source.size.x, minSize);
    const float hy = (std::max)(source.size.y, minSize);
    const float hz = (std::max)(source.size.z, minSize);
    if (hx <= 0.0f || hy <= 0.0f || hz <= 0.0f) return false;
    outA = Dot(d, right) / hx;
    outB = Dot(d, up) / hy;
    outC = Dot(d, normal) / hz;
    return std::fabs(outA) < 1.0f && std::fabs(outB) < 1.0f && std::fabs(outC) < 1.0f;
}

} // namespace

FluidMotionSample SampleFluidMotion(const FluidMotion& motion, float time)
{
    FluidMotionSample sample{ { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
    const std::vector<FluidMotionKey>& keys = motion.keys;
    if (keys.empty()) return sample;
    if (time <= keys.front().time) {
        sample.offset = keys.front().offset;
        return sample;
    }
    if (time >= keys.back().time) {
        sample.offset = keys.back().offset;
        return sample;
    }
    for (std::size_t k = 0; k + 1 < keys.size(); ++k) {
        const FluidMotionKey& from = keys[k];
        const FluidMotionKey& to = keys[k + 1];
        if (time >= to.time) continue;
        const float span = to.time - from.time;
        if (span < 1.0e-6f) {
            sample.offset = from.offset;
            return sample;
        }
        const float t = (time - from.time) / span;
        const math::Vector3 delta = to.offset - from.offset;
        sample.offset = from.offset + delta * t;
        sample.velocity = delta * (1.0f / span);
        return sample;
    }
    sample.offset = keys.back().offset;
    return sample;
}

float SampleFluidAmount(const FluidAmount& amount, float time)
{
    const std::vector<FluidAmountKey>& keys = amount.keys;
    if (keys.empty()) return 1.0f;
    if (time <= keys.front().time) return keys.front().scale;
    if (time >= keys.back().time) return keys.back().scale;
    for (std::size_t k = 0; k + 1 < keys.size(); ++k) {
        const FluidAmountKey& from = keys[k];
        const FluidAmountKey& to = keys[k + 1];
        if (time >= to.time) continue;
        const float span = to.time - from.time;
        if (span < 1.0e-6f) return from.scale;
        return from.scale + (to.scale - from.scale) * ((time - from.time) / span);
    }
    return keys.back().scale;
}

FluidOperatorPose PoseFluidSource(const FluidSource& source, float time)
{
    const FluidMotionSample sample = SampleFluidMotion(source.motion, time);
    return { source.center + sample.offset,
             source.motion.inheritVelocity ? sample.velocity : math::Vector3{ 0.0f, 0.0f, 0.0f } };
}

FluidOperatorPose PoseFluidForce(const FluidForce& force, float time)
{
    const FluidMotionSample sample = SampleFluidMotion(force.motion, time);
    return { force.center + sample.offset,
             force.motion.inheritVelocity ? sample.velocity : math::Vector3{ 0.0f, 0.0f, 0.0f } };
}

float FluidSourceAmount(const FluidSource& source, float time)
{
    // WHY 発生源だけ 0 で止めるか: 掛かる先は density / temperature / fuel で、負にすると «質量を引く»
    //     ことになる。散逸も移流も «負の密度» を想定していないし、色の鍵は mass / max(carrier, ε) で
    //     割るので符号が混ざると色が飛ぶ。黒体放射も T^4 で温度の符号を失う。
    //     «煙を削る» を作りたくなったらそれは別の部品 (吸い込み) として設計する話で、倍率の裏口では作らない。
    //     力側 (FluidForceAmount) は strength が元から負を許す (2D 渦の逆回転) ので止めない。
    return (std::max)(SampleFluidAmount(source.amount, time), 0.0f);
}

float FluidForceAmount(const FluidForce& force, float time)
{
    return SampleFluidAmount(force.amount, time);
}

bool FluidSourceEmitting(const FluidSource& source, float time)
{
    return InWindow(source.enabled, source.startTime, source.duration, time);
}

bool FluidForceActive(const FluidForce& force, float time)
{
    return InWindow(force.enabled, force.startTime, force.duration, time);
}

float FluidSourceWeight(const FluidSource& source, const math::Vector3& center, const math::Vector3& p,
                        float minSize, bool volumetric)
{
    math::Vector3 d = p - center;
    if (!volumetric) d.z = 0.0f;

    switch (source.shape) {
    case FluidSourceShape::Box: {
        const float hx = (std::max)(source.size.x, minSize);
        const float hy = (std::max)(source.size.y, minSize);
        const float hz = (std::max)(source.size.z, minSize);
        // 寸法 0 の軸は 0/0 になる。広げる幅 (minSize) も 0 のときだけ起きる。
        if (hx <= 0.0f || hy <= 0.0f || hz <= 0.0f) return 0.0f;
        const float q = (std::max)(std::fabs(d.x) / hx, (std::max)(std::fabs(d.y) / hy, std::fabs(d.z) / hz));
        if (q >= 1.0f) return 0.0f;
        return Saturate((1.0f - q) * 4.0f);
    }
    case FluidSourceShape::Cone: {
        const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
        const float length = (std::max)(source.size.y, minSize);
        const float radius = (std::max)(source.size.x, minSize);
        if (length <= 0.0f) return 0.0f;
        const float t = Dot(d, axis);
        if (t < 0.0f || t > length) return 0.0f;
        const math::Vector3 radial = d - axis * t;
        const float allowed = (std::max)(radius * t / length, minSize * 0.5f);
        if (allowed <= 0.0f) return 0.0f;
        const float q2 = Dot(radial, radial) / (allowed * allowed);
        if (q2 >= 1.0f) return 0.0f;
        return (1.0f - q2) * (1.0f - q2) * Saturate((1.0f - t / length) * 4.0f);
    }
    case FluidSourceShape::Ring: {
        const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
        const float ringRadius = (std::max)(source.size.x, minSize);
        const float tubeRadius = (std::max)(source.size.y, minSize);
        if (tubeRadius <= 0.0f) return 0.0f;
        const float h = Dot(d, axis);
        const math::Vector3 radial = d - axis * h;
        const float rho = std::sqrt(Dot(radial, radial)) - ringRadius;
        const float q2 = (rho * rho + h * h) / (tubeRadius * tubeRadius);
        if (q2 >= 1.0f) return 0.0f;
        return (1.0f - q2) * (1.0f - q2);
    }
    case FluidSourceShape::Texture: {
        // マスク 1 (= 板の形そのもの)。画像の濃さを見るのは FluidTextureSourceWeight。
        float a = 0.0f;
        float b = 0.0f;
        float c = 0.0f;
        if (!TexturePlateCoordinates(source, center, p, minSize, volumetric, a, b, c)) return 0.0f;
        return Saturate((1.0f - std::fabs(c)) * 4.0f);
    }
    case FluidSourceShape::Capsule: {
        const SegmentFrame frame = MakeSegmentFrame(source.direction, source.size, minSize);
        if (frame.radius <= 0.0f) return 0.0f;
        const float hc = std::clamp(Dot(d, frame.axis), -frame.half, frame.half);
        const math::Vector3 radial = d - frame.axis * hc;
        const float q2 = Dot(radial, radial) / (frame.radius * frame.radius);
        if (q2 >= 1.0f) return 0.0f;
        return (1.0f - q2) * (1.0f - q2);
    }
    case FluidSourceShape::Cylinder: {
        const SegmentFrame frame = MakeSegmentFrame(source.direction, source.size, minSize);
        if (frame.radius <= 0.0f || frame.half <= 0.0f) return 0.0f;
        const float h = Dot(d, frame.axis);
        if (std::fabs(h) > frame.half) return 0.0f;
        const math::Vector3 radial = d - frame.axis * h;
        const float q2 = Dot(radial, radial) / (frame.radius * frame.radius);
        if (q2 >= 1.0f) return 0.0f;
        return (1.0f - q2) * (1.0f - q2) * Saturate((1.0f - std::fabs(h) / frame.half) * 4.0f);
    }
    case FluidSourceShape::Sphere:
    default: {
        const float radius = (std::max)(source.size.x, minSize);
        if (radius <= 0.0f) return 0.0f;
        // 旧ソルバーと同じく軸ごとに割ってから 2 乗和を取る (焼き結果を 1 ビットも変えない)。
        const float dx = d.x / radius;
        const float dy = d.y / radius;
        const float dz = d.z / radius;
        const float q2 = dx * dx + dy * dy + dz * dz;
        if (q2 >= 1.0f) return 0.0f;
        return (1.0f - q2) * (1.0f - q2);
    }
    }
}

math::Vector3 SampleFluidSourcePoint(const FluidSource& source, const math::Vector3& center,
                                     float u0, float u1, float u2, bool volumetric)
{
    const float sx = (std::max)(source.size.x, 0.0f);
    const float sy = (std::max)(source.size.y, 0.0f);
    const float sz = (std::max)(source.size.z, 0.0f);
    math::Vector3 offset = { 0.0f, 0.0f, 0.0f };
    switch (source.shape) {
    case FluidSourceShape::Box:
        offset = { sx * (2.0f * u0 - 1.0f), sy * (2.0f * u1 - 1.0f), volumetric ? sz * (2.0f * u2 - 1.0f) : 0.0f };
        break;
    case FluidSourceShape::Cone:
        offset = volumetric ? SampleCone3D(source, sx, sy, u0, u1, u2) : SampleCone2D(source, sx, sy, u0, u1);
        break;
    case FluidSourceShape::Ring:
        offset = volumetric ? SampleRing3D(source, sx, sy, u0, u1, u2) : SampleRing2D(source, sx, sy, u0, u1, u2);
        break;
    case FluidSourceShape::Texture:
        offset = SampleTexturePlate(source, u0, u1, u2, volumetric);
        break;
    case FluidSourceShape::Capsule:
    case FluidSourceShape::Cylinder: {
        const bool capped = source.shape == FluidSourceShape::Cylinder;
        const SegmentFrame frame = MakeSegmentFrame(source.direction, { sx, sy, sz }, 0.0f);
        offset = volumetric ? SampleSegment3D(frame, capped, u0, u1, u2) : SampleSegment2D(frame, capped, u0, u1);
        break;
    }
    case FluidSourceShape::Sphere:
    default:
        if (volumetric) {
            const float r = sx * std::cbrt(u0);
            const float cosTheta = 1.0f - 2.0f * u1;
            const float sinTheta = std::sqrt((std::max)(0.0f, 1.0f - cosTheta * cosTheta));
            const float phi = 2.0f * kPi * u2;
            offset = { r * sinTheta * std::cos(phi), r * sinTheta * std::sin(phi), r * cosTheta };
        } else {
            const float r = sx * std::sqrt(u0);
            const float angle = 2.0f * kPi * u1;
            offset = { r * std::cos(angle), r * std::sin(angle), 0.0f };
        }
        break;
    }
    math::Vector3 point = center + offset;
    if (!volumetric) point.z = center.z;
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

float FluidTextureSourceWeight(const FluidSource& source, const math::Vector3& center, const math::Vector3& p,
                               float minSize, bool volumetric, const FluidSourceMask& mask)
{
    float a = 0.0f;
    float b = 0.0f;
    float c = 0.0f;
    if (!TexturePlateCoordinates(source, center, p, minSize, volumetric, a, b, c)) return 0.0f;
    return SampleFluidSourceMask(mask, (a + 1.0f) * 0.5f, (1.0f - b) * 0.5f) * Saturate((1.0f - std::fabs(c)) * 4.0f);
}

bool SampleFluidTextureSourcePoint(const FluidSource& source, const math::Vector3& center,
                                   const FluidSourceMask& mask, float u0, float u1, float u2,
                                   float accept, bool volumetric, math::Vector3& outPoint)
{
    const math::Vector3 offset = SampleTexturePlate(source, u0, u1, u2, volumetric);
    math::Vector3 right;
    math::Vector3 up;
    math::Vector3 normal;
    FluidTextureSourceBasis(source, right, up, normal);
    const float sx = (std::max)(source.size.x, 0.0f);
    const float sy = (std::max)(source.size.y, 0.0f);
    const float a = sx > 0.0f ? Dot(offset, right) / sx : 0.0f;
    const float b = sy > 0.0f ? Dot(offset, up) / sy : 0.0f;
    if (accept >= SampleFluidSourceMask(mask, (a + 1.0f) * 0.5f, (1.0f - b) * 0.5f)) return false;
    outPoint = center + offset;
    if (!volumetric) outPoint.z = center.z;
    return true;
}

// WHY 発生源を広げるか: 決まった数を狭い形へ一度に出すと、静止密度の何十倍にも詰まった粒子を
//     密度拘束が押し広げ、発生直後に爆ぜて領域の外へ飛び去る (Blood Burst は半径 0.06 に 650 粒 =
//     入る数の 30 倍だった)。«同時に発生源の中に居る粒» が kEmitPacking まで詰めて収まるよう、
//     形を中心から相似に拡大する倍率を返す (球なら 旧 max(半径, 必要な半径) / 半径 と一致する)。
// ソルバーはここを唯一の窓口として呼ぶ (動きの速さは velocity に足した写しを渡してくる)。
float FluidLiquidEmitScale(const FluidSource& source, const FluidLiquidSettings& liquid, bool volumetric)
{
    const float radius = std::clamp(liquid.particleRadius, 0.002f, 0.1f);
    // ソルバーが Reset で寸法を粒子半径まで広げるのと同じ形で測る (既に広げてあっても結果は変わらない)。
    const math::Vector3 size = { (std::max)(source.size.x, radius), (std::max)(source.size.y, radius),
                                 (std::max)(source.size.z, radius) };
    math::Vector3 launch = source.velocity;
    if (!volumetric) launch.z = 0.0f;
    const float count = static_cast<float>((std::max)(source.count, 0));
    const float speed = launch.Length();
    const math::Vector3 direction = speed > 1.0e-6f ? launch * (1.0f / speed) : math::Vector3{ 0.0f, 1.0f, 0.0f };
    const float width = ShapeWidthAlong(source, size, direction);
    // 出している間に先頭の粒が発生源を抜けるなら、中に居るのは «抜けるまでに出た分» だけ。
    const float travel = speed * source.duration;
    float inside = (source.duration <= 0.0f || travel <= width) ? count : count * width / travel;
    inside /= kEmitPacking;
    const float spacing = 2.0f * radius;
    const float needed = volumetric ? inside * spacing * spacing * spacing : inside * spacing * spacing;
    const float measure = ShapeMeasure(source, size, volumetric);
    if (measure <= 1.0e-12f || needed <= measure) return 1.0f;
    return volumetric ? std::cbrt(needed / measure) : std::sqrt(needed / measure);
}

FluidOperatorPose PoseFluidCollider(const FluidCollider& collider, float time)
{
    const FluidMotionSample sample = SampleFluidMotion(collider.motion, time);
    return { collider.center + sample.offset,
             collider.motion.inheritVelocity ? sample.velocity : math::Vector3{ 0.0f, 0.0f, 0.0f } };
}

bool FluidColliderActive(const FluidCollider& collider, float time)
{
    return InWindow(collider.enabled, collider.startTime, collider.duration, time);
}

float FluidColliderDistance(const FluidCollider& collider, const math::Vector3& center, const math::Vector3& p,
                            float minSize, bool volumetric)
{
    math::Vector3 d = p - center;
    if (!volumetric) d.z = 0.0f;
    switch (collider.shape) {
    case FluidColliderShape::Box: {
        const float qx = std::fabs(d.x) - (std::max)(collider.size.x, minSize);
        const float qy = std::fabs(d.y) - (std::max)(collider.size.y, minSize);
        const float qz = std::fabs(d.z) - (std::max)(collider.size.z, minSize);
        const float ox = (std::max)(qx, 0.0f);
        const float oy = (std::max)(qy, 0.0f);
        const float oz = (std::max)(qz, 0.0f);
        return std::sqrt(ox * ox + oy * oy + oz * oz) + (std::min)((std::max)(qx, (std::max)(qy, qz)), 0.0f);
    }
    case FluidColliderShape::Plane:
        return Dot(d, NormalizeOr(collider.direction, math::Vector3{ 0.0f, 1.0f, 0.0f }));
    case FluidColliderShape::Capsule: {
        const SegmentFrame frame = MakeSegmentFrame(collider.direction, collider.size, minSize);
        const float hc = std::clamp(Dot(d, frame.axis), -frame.half, frame.half);
        const math::Vector3 radial = d - frame.axis * hc;
        return std::sqrt(Dot(radial, radial)) - frame.radius;
    }
    case FluidColliderShape::Cylinder: {
        const SegmentFrame frame = MakeSegmentFrame(collider.direction, collider.size, minSize);
        const float h = Dot(d, frame.axis);
        const math::Vector3 radial = d - frame.axis * h;
        // 半径の向きと軸の向きを 2 軸と見れば、Box と同じ «外の長さ + 中の深さ» で測れる。
        const float dr = std::sqrt(Dot(radial, radial)) - frame.radius;
        const float dh = std::fabs(h) - frame.half;
        const float or_ = (std::max)(dr, 0.0f);
        const float oh = (std::max)(dh, 0.0f);
        return std::sqrt(or_ * or_ + oh * oh) + (std::min)((std::max)(dr, dh), 0.0f);
    }
    case FluidColliderShape::Sphere:
    default:
        return std::sqrt(Dot(d, d)) - (std::max)(collider.size.x, minSize);
    }
}

math::Vector3 FluidColliderNormal(const FluidCollider& collider, const math::Vector3& center, const math::Vector3& p,
                                  float minSize, bool volumetric)
{
    const math::Vector3 up = { 0.0f, 1.0f, 0.0f };
    math::Vector3 d = p - center;
    if (!volumetric) d.z = 0.0f;
    math::Vector3 normal = up;
    switch (collider.shape) {
    case FluidColliderShape::Box: {
        const float qx = std::fabs(d.x) - (std::max)(collider.size.x, minSize);
        const float qy = std::fabs(d.y) - (std::max)(collider.size.y, minSize);
        const float qz = std::fabs(d.z) - (std::max)(collider.size.z, minSize);
        // 距離 > 0 ⇔ どれかの q > 0 (外側の長さが正)。
        if ((std::max)(qx, (std::max)(qy, qz)) > 0.0f) {
            normal = NormalizeOr(math::Vector3{ (std::max)(qx, 0.0f) * SignOrPositive(d.x),
                                                (std::max)(qy, 0.0f) * SignOrPositive(d.y),
                                                (std::max)(qz, 0.0f) * SignOrPositive(d.z) }, up);
        } else if (qx >= qy && qx >= qz) {
            // 同じ深さの軸が並んだら x → y → z の順に採る (GPU の写しも同じ順)。
            normal = { SignOrPositive(d.x), 0.0f, 0.0f };
        } else if (qy >= qz) {
            normal = { 0.0f, SignOrPositive(d.y), 0.0f };
        } else {
            normal = { 0.0f, 0.0f, SignOrPositive(d.z) };
        }
        break;
    }
    case FluidColliderShape::Plane:
        normal = NormalizeOr(collider.direction, up);
        break;
    case FluidColliderShape::Capsule: {
        const SegmentFrame frame = MakeSegmentFrame(collider.direction, collider.size, minSize);
        const float hc = std::clamp(Dot(d, frame.axis), -frame.half, frame.half);
        const math::Vector3 radial = d - frame.axis * hc;
        const float length = std::sqrt(Dot(radial, radial));
        normal = length < 1.0e-6f ? SegmentFallbackNormal(frame.axis) : radial * (1.0f / length);
        break;
    }
    case FluidColliderShape::Cylinder: {
        const SegmentFrame frame = MakeSegmentFrame(collider.direction, collider.size, minSize);
        const float h = Dot(d, frame.axis);
        const math::Vector3 radial = d - frame.axis * h;
        const float length = std::sqrt(Dot(radial, radial));
        const math::Vector3 outward =
            length < 1.0e-6f ? SegmentFallbackNormal(frame.axis) : radial * (1.0f / length);
        const math::Vector3 endward = frame.axis * SignOrPositive(h);
        const float dr = length - frame.radius;
        const float dh = std::fabs(h) - frame.half;
        if ((std::max)(dr, dh) > 0.0f) {
            normal = NormalizeOr(outward * (std::max)(dr, 0.0f) + endward * (std::max)(dh, 0.0f), up);
        } else {
            // 同じ深さなら半径の側を採る (Box が x → y → z の順に採るのと同じ «先の軸が勝つ» 規則)。
            normal = dr >= dh ? outward : endward;
        }
        break;
    }
    case FluidColliderShape::Sphere:
    default: {
        const float length = std::sqrt(Dot(d, d));
        normal = length < 1.0e-6f ? up : d * (1.0f / length);
        break;
    }
    }
    if (!volumetric) {
        normal.z = 0.0f;
        normal = NormalizeOr(normal, up);
    }
    return normal;
}

math::Vector3 FluidForceDelta(const FluidForce& force, const math::Vector3& center,
                              const math::Vector3& p, const math::Vector3& velocity, float time,
                              float dt, const math::Vector3& noiseOffset, int forceIndex,
                              bool volumetric, float strengthScale)
{
    const math::Vector3 zero = { 0.0f, 0.0f, 0.0f };
    // 倍率は influence より先に強さへ掛ける (GPU は定数へ詰める段で掛けてある)。
    const float strength = force.strength * strengthScale;
    if (strength == 0.0f) return zero;
    math::Vector3 d = p - center;
    if (!volumetric) d.z = 0.0f;
    const float dist = std::sqrt(Dot(d, d));
    float influence = 1.0f;
    if (force.radius > 0.0f) {
        if (dist >= force.radius) return zero;
        influence = std::pow(1.0f - dist / force.radius, force.falloffPower);
    }
    const math::Vector3 a = NormalizeOr(force.direction, math::Vector3{ 1.0f, 0.0f, 0.0f });
    const float s = strength * influence;

    math::Vector3 delta = zero;
    switch (force.type) {
    case FluidForceType::Wind:
        delta = a * s * dt;
        break;
    case FluidForceType::Attract:
        if (dist >= 1.0e-5f) delta = -(d / dist) * s * dt;
        break;
    case FluidForceType::Repulse:
        if (dist >= 1.0e-5f) delta = (d / dist) * s * dt;
        break;
    case FluidForceType::Vortex: {
        const math::Vector3 k = volumetric ? a : math::Vector3{ 0.0f, 0.0f, 1.0f };
        const math::Vector3 t = math::Vector3::Cross(k, d);
        const float length = std::sqrt(Dot(t, t));
        if (length >= 1.0e-5f) delta = (t / length) * s * dt;
        break;
    }
    case FluidForceType::Noise: {
        const float index = static_cast<float>(forceIndex);
        const math::Vector3 q = p * force.noiseFrequency + noiseOffset
            + math::Vector3{ index * 17.31f, -time * force.noiseSpeed, index * 5.73f };
        delta = core::CurlNoise(q) * s * dt;
        break;
    }
    case FluidForceType::Drag:
        // 陰的な減衰 (1 − e^{−s·dt})。dt が大きくても速度の向きを越えて振り戻さない。
        // strength は負も許す (2D の渦を逆に回す唯一の手段) が、Drag で負だと速度を増やしてしまうので 0 で止める。
        delta = -velocity * (1.0f - std::exp(-(std::max)(s, 0.0f) * dt));
        break;
    }
    if (!volumetric) delta.z = 0.0f;
    return delta;
}

} // namespace fbzz::asset
