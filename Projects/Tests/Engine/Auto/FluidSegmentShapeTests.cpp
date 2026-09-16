/// @file    FluidSegmentShapeTests.cpp
/// @brief   流体レシピの «芯の線分を持つ形» (Capsule / Cylinder) の重み・抽選・距離・法線を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-13
///
/// 式の正本は Engine/Asset/FluidOperatorEval.hpp。GPU は Assets/Shaders/Bake/Fluid/FluidGpuCommon.hlsli
/// (気体) と LiquidCommon.hlsli (液体) に写しを持つ。シェーダーをここから走らせることはできないので、
/// «HLSL と同じ順で同じ演算を並べた C++» を Hlsl... 関数として置き、正本と一致することを縛る
/// (シェーダー側の本文をこの関数と読み比べれば、写しが合っているかが 1 か所で分かる)。
///
/// 形の番号は .fluid の shape 番号でもあるので、末尾に足した位置が動かないことも固定する。

#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>

#include <Engine/Asset/FluidGpuStep.hpp>
#include <Engine/Asset/FluidOperatorEval.hpp>
#include <Engine/Asset/FluidRecipe.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

using asset::FluidCollider;
using asset::FluidColliderShape;
using asset::FluidSource;
using asset::FluidSourceShape;

constexpr float kRadius = 0.2f;
constexpr float kHalf = 0.5f;

float Length(const math::Vector3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
float Dot(const math::Vector3& a, const math::Vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

void ExpectVectorNear(const math::Vector3& actual, const math::Vector3& expected, float tolerance = 1.0e-5f)
{
    EXPECT_NEAR(actual.x, expected.x, tolerance);
    EXPECT_NEAR(actual.y, expected.y, tolerance);
    EXPECT_NEAR(actual.z, expected.z, tolerance);
}

// 上向きの軸・半径 0.2・芯の半分の長さ 0.5 (端から端までは 1.4)。
FluidSource MakeSegmentSource(FluidSourceShape shape, const math::Vector3& direction = { 0.0f, 1.0f, 0.0f })
{
    FluidSource source;
    source.shape = shape;
    source.center = { 0.0f, 0.0f, 0.0f };
    source.size = { kRadius, kHalf, 0.0f };
    source.direction = direction;
    return source;
}

FluidCollider MakeSegmentCollider(FluidColliderShape shape, const math::Vector3& direction = { 0.0f, 1.0f, 0.0f })
{
    FluidCollider collider;
    collider.shape = shape;
    collider.center = { 0.0f, 0.0f, 0.0f };
    collider.size = { kRadius, kHalf, 0.0f };
    collider.direction = direction;
    return collider;
}

float Weight(const FluidSource& source, const math::Vector3& p, float minSize = 0.0f, bool volumetric = true)
{
    return asset::FluidSourceWeight(source, source.center, p, minSize, volumetric);
}

float Distance(const FluidCollider& collider, const math::Vector3& p, bool volumetric = true, float minSize = 0.0f)
{
    return asset::FluidColliderDistance(collider, collider.center, p, minSize, volumetric);
}

math::Vector3 Normal(const FluidCollider& collider, const math::Vector3& p, bool volumetric = true)
{
    return asset::FluidColliderNormal(collider, collider.center, p, 0.0f, volumetric);
}

// ── HLSL の写し (式を変えたらシェーダーとここを同時に直す) ──

float Saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }

// FluidOperatorEval.cpp の NormalizeOr と同じ式 (逆数を掛ける)。
// WHY math::Vector3::NormalizedOr を使わないか: あちらは長さで割るので最下位ビットが違い、
//     «式が同じか» を見たいここでは 1 ULP の差が混ざる (詰める側は Math の方を使っている)。
math::Vector3 NormalizeOr(const math::Vector3& v, const math::Vector3& fallback)
{
    const float lengthSq = Dot(v, v);
    if (lengthSq < 1.0e-12f) return fallback;
    return v * (1.0f / std::sqrt(lengthSq));
}

// FluidGpuCommon.hlsli の FluidSourceWeight (カプセル / 円柱の枝)。
// 詰める側 (PackFluidGpuStep) が size をセル幅まで広げ、軸を正規化してから渡す。
float HlslSourceWeight(bool cylinder, const math::Vector3& center, const math::Vector3& packedSize,
                       const math::Vector3& axis, const math::Vector3& p, float minSize)
{
    const math::Vector3 d = p - center;
    const math::Vector3 a = axis;
    const float segR = (std::max)(packedSize.x, minSize);
    const float segH = (std::max)(packedSize.y, minSize);
    if (!cylinder) {
        const float hc = std::clamp(Dot(d, a), -segH, segH);
        const math::Vector3 radial = d - a * hc;
        const float q2 = Dot(radial, radial) / (segR * segR);
        if (q2 >= 1.0f) return 0.0f;
        return (1.0f - q2) * (1.0f - q2);
    }
    const float h = Dot(d, a);
    if (std::fabs(h) > segH) return 0.0f;
    const math::Vector3 radial = d - a * h;
    const float q2 = Dot(radial, radial) / (segR * segR);
    if (q2 >= 1.0f) return 0.0f;
    return (1.0f - q2) * (1.0f - q2) * Saturate((1.0f - std::fabs(h) / segH) * 4.0f);
}

// FluidGpuCommon.hlsli / LiquidCommon.hlsli の ColliderDistance (カプセル / 円柱の枝)。
float HlslColliderDistance(bool cylinder, const math::Vector3& center, const math::Vector3& packedSize,
                           const math::Vector3& axis, const math::Vector3& p)
{
    const math::Vector3 d = p - center;
    const math::Vector3 a = axis;
    const float segR = packedSize.x;
    const float segH = packedSize.y;
    if (!cylinder) {
        const float hc = std::clamp(Dot(d, a), -segH, segH);
        return Length(d - a * hc) - segR;
    }
    const float h = Dot(d, a);
    const float qx = Length(d - a * h) - segR;
    const float qy = std::fabs(h) - segH;
    const float ox = (std::max)(qx, 0.0f);
    const float oy = (std::max)(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + (std::min)((std::max)(qx, qy), 0.0f);
}

// LiquidCommon.hlsli の LiquidSegmentFallbackNormal。
math::Vector3 HlslSegmentFallbackNormal(const math::Vector3& a)
{
    const math::Vector3 helper = std::fabs(a.x) < 0.9f ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                                                       : math::Vector3{ 0.0f, 1.0f, 0.0f };
    const math::Vector3 crossed = math::Vector3::Cross(a, helper);
    return crossed * (1.0f / Length(crossed));
}

// LiquidCommon.hlsli の LiquidColliderNormal (カプセル / 円柱の枝)。
math::Vector3 HlslColliderNormal(bool cylinder, const math::Vector3& center, const math::Vector3& packedSize,
                                 const math::Vector3& axis, const math::Vector3& p)
{
    const math::Vector3 d = p - center;
    const math::Vector3 a = axis;
    const float segR = packedSize.x;
    const float segH = packedSize.y;
    if (!cylinder) {
        const float hc = std::clamp(Dot(d, a), -segH, segH);
        const math::Vector3 radial = d - a * hc;
        const float len = Length(radial);
        return len < 1.0e-6f ? HlslSegmentFallbackNormal(a) : radial * (1.0f / len);
    }
    const float h = Dot(d, a);
    const math::Vector3 radial = d - a * h;
    const float len = Length(radial);
    const math::Vector3 outward = len < 1.0e-6f ? HlslSegmentFallbackNormal(a) : radial * (1.0f / len);
    const math::Vector3 endward = a * (h < 0.0f ? -1.0f : 1.0f);
    const float dr = len - segR;
    const float dh = std::fabs(h) - segH;
    if ((std::max)(dr, dh) > 0.0f) {
        const math::Vector3 blended = outward * (std::max)(dr, 0.0f) + endward * (std::max)(dh, 0.0f);
        const float lengthSq = Dot(blended, blended);
        return lengthSq < 1.0e-12f ? math::Vector3{ 0.0f, 1.0f, 0.0f } : blended * (1.0f / std::sqrt(lengthSq));
    }
    return dr >= dh ? outward : endward;
}

// 突き合わせに使う «形の内も外も端も» 通る点。
std::vector<math::Vector3> ProbePoints()
{
    return { { 0.0f, 0.0f, 0.0f },      { 0.0f, 0.3f, 0.0f },    { 0.0f, 0.5f, 0.0f },
             { 0.0f, 0.6f, 0.0f },      { 0.0f, 0.7f, 0.0f },    { 0.0f, 0.9f, 0.0f },
             { 0.1f, 0.0f, 0.0f },      { 0.2f, 0.0f, 0.0f },    { 0.35f, 0.0f, 0.0f },
             { 0.1f, 0.55f, 0.0f },     { 0.3f, 0.6f, 0.0f },    { 0.05f, -0.45f, 0.08f },
             { -0.12f, -0.62f, 0.03f }, { 0.0f, -0.9f, 0.0f },   { 0.18f, 0.42f, -0.09f } };
}

} // namespace

// ── 形の番号 (既存の .fluid がずれないこと) ──

TEST(FluidSegmentShapeTest, NewShapesSitAtTheEndOfTheEnums)
{
    EXPECT_EQ(static_cast<int>(FluidSourceShape::Sphere), 0);
    EXPECT_EQ(static_cast<int>(FluidSourceShape::Box), 1);
    EXPECT_EQ(static_cast<int>(FluidSourceShape::Cone), 2);
    EXPECT_EQ(static_cast<int>(FluidSourceShape::Ring), 3);
    EXPECT_EQ(static_cast<int>(FluidSourceShape::Texture), 4);
    EXPECT_EQ(static_cast<int>(FluidSourceShape::Capsule), 5);
    EXPECT_EQ(static_cast<int>(FluidSourceShape::Cylinder), 6);

    EXPECT_EQ(static_cast<int>(FluidColliderShape::Sphere), 0);
    EXPECT_EQ(static_cast<int>(FluidColliderShape::Box), 1);
    EXPECT_EQ(static_cast<int>(FluidColliderShape::Plane), 2);
    EXPECT_EQ(static_cast<int>(FluidColliderShape::Capsule), 3);
    EXPECT_EQ(static_cast<int>(FluidColliderShape::Cylinder), 4);
}

TEST(FluidSegmentShapeTest, ShapesRoundTripThroughTheirTomlLabels)
{
    testkit::TempDir temp{ "fluidsegment" };
    ASSERT_TRUE(temp.IsValid());

    asset::FluidRecipe recipe;
    recipe.sources.push_back(MakeSegmentSource(FluidSourceShape::Capsule, { 1.0f, 0.0f, 0.0f }));
    recipe.sources.push_back(MakeSegmentSource(FluidSourceShape::Cylinder));
    recipe.colliders.push_back(MakeSegmentCollider(FluidColliderShape::Capsule, { 0.0f, 0.0f, 1.0f }));
    recipe.colliders.push_back(MakeSegmentCollider(FluidColliderShape::Cylinder));

    const std::string path = util::FileSystem::PathToUtf8(temp.File("segment.fluid"));
    ASSERT_TRUE(asset::SaveFluidRecipe(path, recipe));
    asset::FluidRecipe loaded;
    std::string error;
    ASSERT_TRUE(asset::LoadFluidRecipe(path, loaded, &error)) << error;
    ASSERT_EQ(loaded.sources.size(), 2u);
    ASSERT_EQ(loaded.colliders.size(), 2u);
    EXPECT_EQ(loaded.sources[0].shape, FluidSourceShape::Capsule);
    EXPECT_EQ(loaded.sources[1].shape, FluidSourceShape::Cylinder);
    EXPECT_EQ(loaded.colliders[0].shape, FluidColliderShape::Capsule);
    EXPECT_EQ(loaded.colliders[1].shape, FluidColliderShape::Cylinder);
    // 軸は direction が持つ (書けても読めないと «保存したのに戻らない»)。
    ExpectVectorNear(loaded.sources[0].direction, { 1.0f, 0.0f, 0.0f });
    ExpectVectorNear(loaded.colliders[0].direction, { 0.0f, 0.0f, 1.0f });
    EXPECT_FLOAT_EQ(loaded.sources[1].size.y, kHalf);
}

// ── 発生源の重み ──

TEST(FluidSegmentShapeTest, CapsuleWeightMeasuresTheDistanceToItsCoreSegment)
{
    const FluidSource source = MakeSegmentSource(FluidSourceShape::Capsule);
    // 芯の上はどこでも 1 (球の «中心だけ 1» と違って «線» が満たされる)。
    EXPECT_FLOAT_EQ(Weight(source, { 0.0f, 0.0f, 0.0f }), 1.0f);
    EXPECT_FLOAT_EQ(Weight(source, { 0.0f, 0.3f, 0.0f }), 1.0f);
    EXPECT_FLOAT_EQ(Weight(source, { 0.0f, 0.5f, 0.0f }), 1.0f);
    EXPECT_FLOAT_EQ(Weight(source, { 0.0f, -0.5f, 0.0f }), 1.0f);
    // 半径の途中は (1 − q²)² (球と同じ落ち方)。
    EXPECT_FLOAT_EQ(Weight(source, { 0.1f, 0.0f, 0.0f }), 0.5625f);
    EXPECT_FLOAT_EQ(Weight(source, { 0.0f, 0.0f, 0.1f }), 0.5625f);
    // 端の半球も «芯の端点からの距離» なので同じ落ち方。
    EXPECT_FLOAT_EQ(Weight(source, { 0.0f, 0.6f, 0.0f }), 0.5625f);
    EXPECT_FLOAT_EQ(Weight(source, { 0.0f, -0.6f, 0.0f }), 0.5625f);
    EXPECT_NEAR(Weight(source, { 0.1f, 0.55f, 0.0f }), 0.47265625f, 1.0e-6f);
    // 表面と外 (端から端までは 2 × (H + R) = 1.4)。
    EXPECT_EQ(Weight(source, { 0.2f, 0.0f, 0.0f }), 0.0f);
    EXPECT_NEAR(Weight(source, { 0.0f, 0.7f, 0.0f }), 0.0f, 1.0e-6f);
    EXPECT_EQ(Weight(source, { 0.0f, 0.75f, 0.0f }), 0.0f);
    EXPECT_EQ(Weight(source, { 0.0f, 0.9f, 0.0f }), 0.0f);
}

TEST(FluidSegmentShapeTest, CapsuleWithAPointCoreIsTheSphereFormula)
{
    FluidSource capsule = MakeSegmentSource(FluidSourceShape::Capsule);
    capsule.size.y = 0.0f;   // 芯が 1 点 = 球
    const math::Vector3 points[] = { { 0.05f, 0.0f, 0.0f }, { 0.1f, 0.1f, 0.0f }, { 0.0f, 0.0f, 0.15f } };
    for (const math::Vector3& p : points) {
        const float q2 = (p.x * p.x + p.y * p.y + p.z * p.z) / (kRadius * kRadius);
        EXPECT_NEAR(Weight(capsule, p), (1.0f - q2) * (1.0f - q2), 1.0e-6f);
    }
    // 端がどちらの向きにも半径ぶんしか伸びない。
    EXPECT_EQ(Weight(capsule, { 0.0f, 0.2f, 0.0f }), 0.0f);
}

TEST(FluidSegmentShapeTest, CylinderIsCutFlatAtBothEndsWhereTheCapsuleBulges)
{
    const FluidSource capsule = MakeSegmentSource(FluidSourceShape::Capsule);
    const FluidSource cylinder = MakeSegmentSource(FluidSourceShape::Cylinder);

    // 芯の真ん中では同じ形 (半径の落ち方は同じ式)。
    EXPECT_FLOAT_EQ(Weight(cylinder, { 0.0f, 0.0f, 0.0f }), 1.0f);
    EXPECT_FLOAT_EQ(Weight(cylinder, { 0.1f, 0.0f, 0.0f }), Weight(capsule, { 0.1f, 0.0f, 0.0f }));

    // 端の先: カプセルは半球ぶん残り、円柱は 0。«端が半球» の証拠。
    for (const float y : { 0.55f, 0.6f, 0.65f, -0.55f, -0.6f }) {
        const math::Vector3 p{ 0.0f, y, 0.0f };
        EXPECT_GT(Weight(capsule, p), 0.0f) << "y " << y;
        EXPECT_EQ(Weight(cylinder, p), 0.0f) << "y " << y;
    }
    // 平らな端の外へ «半径ぶん横へずれた» 点も、カプセルだけが持つ。
    EXPECT_GT(Weight(capsule, { 0.1f, 0.55f, 0.0f }), 0.0f);
    EXPECT_EQ(Weight(cylinder, { 0.1f, 0.55f, 0.0f }), 0.0f);

    // 円柱の端は «なめらかに 0» (Cone の底と同じ 4 倍の傾き)。
    EXPECT_FLOAT_EQ(Weight(cylinder, { 0.0f, 0.375f, 0.0f }), 1.0f);
    EXPECT_NEAR(Weight(cylinder, { 0.0f, 0.45f, 0.0f }), 0.4f, 1.0e-6f);
    EXPECT_EQ(Weight(cylinder, { 0.0f, 0.5f, 0.0f }), 0.0f);
    // 半径の外は端の遠近に関わらず 0。
    EXPECT_EQ(Weight(cylinder, { 0.2f, 0.0f, 0.0f }), 0.0f);
}

TEST(FluidSegmentShapeTest, SegmentWeightsWidenTinySizesToTheCellWidth)
{
    // 格子より細い棒は «どのセル中心も覆えない» ので、Cone / Ring と同じくセル幅まで広げる。
    for (const FluidSourceShape shape : { FluidSourceShape::Capsule, FluidSourceShape::Cylinder }) {
        FluidSource tiny = MakeSegmentSource(shape);
        tiny.size = { 0.001f, 0.001f, 0.0f };
        EXPECT_EQ(Weight(tiny, { 0.05f, 0.0f, 0.0f }, 0.0f), 0.0f);
        EXPECT_GT(Weight(tiny, { 0.05f, 0.0f, 0.0f }, 0.1f), 0.0f);
    }
}

TEST(FluidSegmentShapeTest, FlatSegmentWeightIgnoresDepth)
{
    const FluidSource source = MakeSegmentSource(FluidSourceShape::Capsule, { 1.0f, 0.0f, 0.0f });
    const float flat = Weight(source, { 0.3f, 0.1f, 0.0f }, 0.0f, false);
    EXPECT_GT(flat, 0.0f);
    EXPECT_FLOAT_EQ(Weight(source, { 0.3f, 0.1f, 5.0f }, 0.0f, false), flat);
    EXPECT_EQ(Weight(source, { 0.3f, 0.1f, 5.0f }, 0.0f, true), 0.0f);
}

// ── 粒子の抽選 ──

TEST(FluidSegmentShapeTest, SampledPointsStayInsideAndReachTheHemisphereOnlyForTheCapsule)
{
    const float us[] = { 0.02f, 0.2f, 0.5f, 0.78f, 0.97f };
    for (const FluidSourceShape shape : { FluidSourceShape::Capsule, FluidSourceShape::Cylinder }) {
        const bool cylinder = shape == FluidSourceShape::Cylinder;
        for (const math::Vector3 axis : { math::Vector3{ 0.0f, 1.0f, 0.0f }, math::Vector3{ 1.0f, 0.0f, 0.0f },
                                          math::Vector3{ 0.3f, 0.5f, 0.8f }, math::Vector3{ 0.0f, 0.0f, 1.0f } }) {
            const FluidSource source = MakeSegmentSource(shape, axis);
            const math::Vector3 unit = NormalizeOr(axis, { 0.0f, 1.0f, 0.0f });
            for (const bool volumetric : { false, true }) {
                bool beyondTheFlatEnd = false;
                for (const float u0 : us)
                    for (const float u1 : us)
                        for (const float u2 : us) {
                            const math::Vector3 p =
                                asset::SampleFluidSourcePoint(source, source.center, u0, u1, u2, volumetric);
                            EXPECT_GT(Weight(source, p, 0.0f, volumetric), 0.0f)
                                << "shape " << static_cast<int>(shape) << " volumetric " << volumetric
                                << " u " << u0 << "," << u1 << "," << u2;
                            if (!volumetric) EXPECT_EQ(p.z, source.center.z);
                            if (std::fabs(Dot(p - source.center, unit)) > kHalf) beyondTheFlatEnd = true;
                        }
                // 円柱は芯の端より先へ 1 粒も出ない。カプセルは出る (= 端が半球になっている)。
                if (cylinder) EXPECT_FALSE(beyondTheFlatEnd) << "volumetric " << volumetric;
            }
        }
    }
}

TEST(FluidSegmentShapeTest, VolumetricCapsuleSamplesReachBothHemispheres)
{
    const FluidSource capsule = MakeSegmentSource(FluidSourceShape::Capsule);
    bool top = false;
    bool bottom = false;
    // u0 は円柱と半球の選び分け。両端の半球へ届くことを見る (片側しか出ないと «端が片方だけ丸い»)。
    for (int k = 0; k < 64; ++k) {
        const float u0 = (static_cast<float>(k) + 0.5f) / 64.0f;
        for (const float u1 : { 0.05f, 0.95f }) {
            const math::Vector3 p = asset::SampleFluidSourcePoint(capsule, capsule.center, u0, u1, 0.25f, true);
            if (p.y > kHalf) top = true;
            if (p.y < -kHalf) bottom = true;
        }
    }
    EXPECT_TRUE(top);
    EXPECT_TRUE(bottom);
}

// ── 障害物の距離と法線 ──

TEST(FluidSegmentShapeTest, SegmentColliderDistanceIsNegativeInsideAndCrossesZeroOnTheSurface)
{
    const FluidCollider capsule = MakeSegmentCollider(FluidColliderShape::Capsule);
    const FluidCollider cylinder = MakeSegmentCollider(FluidColliderShape::Cylinder);

    // 芯の上はどちらも «半径ぶん» 中。
    EXPECT_FLOAT_EQ(Distance(capsule, { 0.0f, 0.0f, 0.0f }), -kRadius);
    EXPECT_FLOAT_EQ(Distance(cylinder, { 0.0f, 0.0f, 0.0f }), -kRadius);
    // 芯の端は、カプセルはまだ半径ぶん中・円柱は平らな端の面そのもの (距離 0)。
    EXPECT_FLOAT_EQ(Distance(capsule, { 0.0f, kHalf, 0.0f }), -kRadius);
    EXPECT_FLOAT_EQ(Distance(cylinder, { 0.0f, kHalf, 0.0f }), 0.0f);
    // 円柱の中では «近いほうの面» までの深さ。
    EXPECT_NEAR(Distance(cylinder, { 0.0f, 0.4f, 0.0f }), -0.1f, 1.0e-6f);
    EXPECT_NEAR(Distance(cylinder, { 0.15f, 0.0f, 0.0f }), -0.05f, 1.0e-6f);
    // 横の表面はどちらも 0。
    EXPECT_NEAR(Distance(capsule, { kRadius, 0.0f, 0.0f }), 0.0f, 1.0e-6f);
    EXPECT_NEAR(Distance(cylinder, { kRadius, 0.0f, 0.0f }), 0.0f, 1.0e-6f);
    // 端の先: カプセルは半球の表面が 0.7、円柱は 0.5。
    EXPECT_NEAR(Distance(capsule, { 0.0f, kHalf + kRadius, 0.0f }), 0.0f, 1.0e-6f);
    EXPECT_NEAR(Distance(capsule, { 0.0f, 0.8f, 0.0f }), 0.1f, 1.0e-6f);
    EXPECT_NEAR(Distance(cylinder, { 0.0f, 0.6f, 0.0f }), 0.1f, 1.0e-6f);
    // 角の外: 円柱は «半径の外 + 端の外» の斜め、カプセルは端点からの距離。
    EXPECT_NEAR(Distance(cylinder, { 0.3f, 0.6f, 0.0f }), std::sqrt(0.02f), 1.0e-6f);
    EXPECT_NEAR(Distance(capsule, { 0.3f, 0.6f, 0.0f }), std::sqrt(0.1f) - kRadius, 1.0e-6f);
    // 軸を変えても «芯への距離» なので向きに追従する。
    const FluidCollider lying = MakeSegmentCollider(FluidColliderShape::Capsule, { 1.0f, 0.0f, 0.0f });
    EXPECT_FLOAT_EQ(Distance(lying, { kHalf, 0.0f, 0.0f }), -kRadius);
    EXPECT_NEAR(Distance(lying, { 0.0f, 0.8f, 0.0f }), 0.6f, 1.0e-6f);
}

TEST(FluidSegmentShapeTest, SegmentColliderNormalsPointOutOfTheSurface)
{
    const FluidCollider capsule = MakeSegmentCollider(FluidColliderShape::Capsule);
    const FluidCollider cylinder = MakeSegmentCollider(FluidColliderShape::Cylinder);

    ExpectVectorNear(Normal(capsule, { 0.3f, 0.0f, 0.0f }), { 1.0f, 0.0f, 0.0f });
    ExpectVectorNear(Normal(capsule, { 0.0f, 0.9f, 0.0f }), { 0.0f, 1.0f, 0.0f });
    ExpectVectorNear(Normal(capsule, { 0.3f, 0.6f, 0.0f }), { 0.94868332f, 0.31622776f, 0.0f });
    // 平らな端の外は軸向き、横は半径向き、角は 2 つの混ざり。
    ExpectVectorNear(Normal(cylinder, { 0.0f, 0.6f, 0.0f }), { 0.0f, 1.0f, 0.0f });
    ExpectVectorNear(Normal(cylinder, { 0.0f, -0.6f, 0.0f }), { 0.0f, -1.0f, 0.0f });
    ExpectVectorNear(Normal(cylinder, { 0.3f, 0.0f, 0.0f }), { 1.0f, 0.0f, 0.0f });
    ExpectVectorNear(Normal(cylinder, { 0.3f, 0.6f, 0.0f }), { 0.70710678f, 0.70710678f, 0.0f });
    // 中では «近いほうの面» の向き (Box の «同じ深さなら先の軸» と同じ規則)。
    ExpectVectorNear(Normal(cylinder, { 0.0f, 0.4f, 0.0f }), { 0.0f, 1.0f, 0.0f });
    ExpectVectorNear(Normal(cylinder, { 0.15f, 0.0f, 0.0f }), { 1.0f, 0.0f, 0.0f });

    // 芯の上は «どちら向きでもよい» ので、軸に直交する決まった 1 本を返す (上向きを返すと芯に沿って押す)。
    const math::Vector3 onCore = Normal(capsule, { 0.0f, 0.2f, 0.0f });
    EXPECT_NEAR(Length(onCore), 1.0f, 1.0e-5f);
    EXPECT_NEAR(Dot(onCore, { 0.0f, 1.0f, 0.0f }), 0.0f, 1.0e-5f);
    ExpectVectorNear(onCore, { 0.0f, 0.0f, -1.0f });

    // 2D では奥行きを持たない (押し出す向きが画面から出ていくと粒子が消える)。
    EXPECT_EQ(Normal(capsule, { 0.3f, 0.1f, 0.2f }, false).z, 0.0f);
    EXPECT_EQ(Normal(cylinder, { 0.3f, 0.6f, 0.2f }, false).z, 0.0f);

    // 法線は距離の勾配。表面の外側へ少し進めば距離が伸びる。
    for (const FluidCollider& collider : { capsule, cylinder }) {
        for (const math::Vector3& p : ProbePoints()) {
            const math::Vector3 n = Normal(collider, p);
            EXPECT_GT(Distance(collider, p + n * 0.01f), Distance(collider, p) - 1.0e-6f);
        }
    }
}

// ── CPU と HLSL の写しが同じ式であること ──

TEST(FluidSegmentShapeTest, GpuSourceWeightMirrorsTheCpuFormula)
{
    constexpr float kCellSize = 0.03125f;
    for (const FluidSourceShape shape : { FluidSourceShape::Capsule, FluidSourceShape::Cylinder }) {
        const bool cylinder = shape == FluidSourceShape::Cylinder;
        for (const math::Vector3 axis : { math::Vector3{ 0.0f, 1.0f, 0.0f }, math::Vector3{ 0.3f, 0.5f, 0.8f },
                                          math::Vector3{ -1.0f, 0.2f, 0.0f } }) {
            FluidSource source = MakeSegmentSource(shape, axis);
            source.center = { 0.1f, -0.2f, 0.05f };
            // PackFluidGpuStep と同じ詰め方 (寸法はセル幅まで広げ、軸は正規化する)。
            const math::Vector3 packed{ (std::max)(source.size.x, kCellSize), (std::max)(source.size.y, kCellSize),
                                        (std::max)(source.size.z, kCellSize) };
            const math::Vector3 unit = NormalizeOr(source.direction, { 0.0f, 1.0f, 0.0f });
            for (const math::Vector3& offset : ProbePoints()) {
                const math::Vector3 p = source.center + offset;
                EXPECT_FLOAT_EQ(HlslSourceWeight(cylinder, source.center, packed, unit, p, kCellSize),
                                Weight(source, p, kCellSize))
                    << "shape " << static_cast<int>(shape);
            }
        }
    }
}

TEST(FluidSegmentShapeTest, GpuColliderDistanceAndNormalMirrorTheCpuFormula)
{
    for (const FluidColliderShape shape : { FluidColliderShape::Capsule, FluidColliderShape::Cylinder }) {
        const bool cylinder = shape == FluidColliderShape::Cylinder;
        for (const math::Vector3 axis : { math::Vector3{ 0.0f, 1.0f, 0.0f }, math::Vector3{ 0.3f, 0.5f, 0.8f },
                                          math::Vector3{ -1.0f, 0.2f, 0.0f } }) {
            FluidCollider collider = MakeSegmentCollider(shape, axis);
            collider.center = { -0.05f, 0.15f, -0.1f };
            // 液体は寸法を広げずに詰める (LiquidCollider の «大きさは広げない»)。
            // 気体は詰める段でセル幅まで広げるだけで、式は同じ 1 本。
            const math::Vector3 packed{ (std::max)(collider.size.x, 0.0f), (std::max)(collider.size.y, 0.0f),
                                        (std::max)(collider.size.z, 0.0f) };
            const math::Vector3 unit = NormalizeOr(collider.direction, { 0.0f, 1.0f, 0.0f });
            for (const math::Vector3& offset : ProbePoints()) {
                const math::Vector3 p = collider.center + offset;
                EXPECT_FLOAT_EQ(HlslColliderDistance(cylinder, collider.center, packed, unit, p),
                                Distance(collider, p))
                    << "shape " << static_cast<int>(shape);
                ExpectVectorNear(HlslColliderNormal(cylinder, collider.center, packed, unit, p),
                                 Normal(collider, p), 1.0e-6f);
            }
        }
    }
}

TEST(FluidSegmentShapeTest, GpuStepCarriesTheShapeNumberAndTheAxis)
{
    asset::FluidRecipe recipe;
    recipe.sources.push_back(MakeSegmentSource(FluidSourceShape::Capsule, { 0.0f, 2.0f, 0.0f }));
    recipe.sources.push_back(MakeSegmentSource(FluidSourceShape::Cylinder, { 3.0f, 0.0f, 0.0f }));
    recipe.colliders.push_back(MakeSegmentCollider(FluidColliderShape::Capsule, { 0.0f, 0.0f, -4.0f }));
    recipe.colliders.push_back(MakeSegmentCollider(FluidColliderShape::Cylinder));

    const asset::FluidGpuStepConstants c = asset::PackFluidGpuStep(recipe, 64, 0.0f, 1.0f / 60.0f, 1.0f, 1.0f);
    ASSERT_EQ(c.sourceCount, 2u);
    ASSERT_EQ(c.colliderCount, 2u);
    EXPECT_FLOAT_EQ(c.sources[0].centerShape[3], 5.0f);
    EXPECT_FLOAT_EQ(c.sources[1].centerShape[3], 6.0f);
    EXPECT_FLOAT_EQ(c.colliders[0].centerShape[3], 3.0f);
    EXPECT_FLOAT_EQ(c.colliders[1].centerShape[3], 4.0f);
    // 軸は正規化して渡す (シェーダーは正規化しない)。
    ExpectVectorNear({ c.sources[0].axis[0], c.sources[0].axis[1], c.sources[0].axis[2] }, { 0.0f, 1.0f, 0.0f });
    ExpectVectorNear({ c.sources[1].axis[0], c.sources[1].axis[1], c.sources[1].axis[2] }, { 1.0f, 0.0f, 0.0f });
    ExpectVectorNear({ c.colliders[0].normal[0], c.colliders[0].normal[1], c.colliders[0].normal[2] },
                     { 0.0f, 0.0f, -1.0f });
    // 半径と «軸方向の長さの半分» は x / y に入る (テクスチャのタイル番号は使わない)。
    EXPECT_FLOAT_EQ(c.sources[0].sizeNoise[0], kRadius);
    EXPECT_FLOAT_EQ(c.sources[0].sizeNoise[1], kHalf);
    EXPECT_FLOAT_EQ(c.sources[0].axis[3], -1.0f);
    EXPECT_FLOAT_EQ(c.colliders[1].sizeActive[0], kRadius);
    EXPECT_FLOAT_EQ(c.colliders[1].sizeActive[1], kHalf);
}

// ── 既存の形が 1 ドットも変わっていないこと ──

TEST(FluidSegmentShapeTest, ExistingSourceShapeWeightsAreUnchanged)
{
    const math::Vector3 c{ 0.1f, -0.2f, 0.05f };

    FluidSource sphere;
    sphere.shape = FluidSourceShape::Sphere;
    sphere.center = c;
    sphere.size = { 0.3f, 0.3f, 0.3f };
    EXPECT_FLOAT_EQ(asset::FluidSourceWeight(sphere, c, c, 0.01f, true), 1.0f);
    EXPECT_NEAR(asset::FluidSourceWeight(sphere, c, { c.x + 0.15f, c.y, c.z }, 0.01f, true), 0.5625f, 1.0e-6f);
    EXPECT_EQ(asset::FluidSourceWeight(sphere, c, { c.x + 0.35f, c.y, c.z }, 0.01f, true), 0.0f);

    FluidSource box = sphere;
    box.shape = FluidSourceShape::Box;
    box.size = { 0.3f, 0.2f, 0.25f };
    EXPECT_NEAR(asset::FluidSourceWeight(box, c, { c.x + 0.27f, c.y, c.z }, 0.01f, true), 0.4f, 1.0e-5f);
    EXPECT_EQ(asset::FluidSourceWeight(box, c, { c.x, c.y + 0.2f, c.z }, 0.01f, true), 0.0f);

    FluidSource cone = sphere;
    cone.shape = FluidSourceShape::Cone;
    cone.size = { 0.2f, 0.5f, 0.0f };
    cone.direction = { 0.0f, 1.0f, 0.0f };
    EXPECT_FLOAT_EQ(asset::FluidSourceWeight(cone, c, { c.x, c.y + 0.25f, c.z }, 0.02f, true), 1.0f);
    EXPECT_NEAR(asset::FluidSourceWeight(cone, c, { c.x, c.y + 0.45f, c.z }, 0.02f, true), 0.4f, 1.0e-5f);
    EXPECT_EQ(asset::FluidSourceWeight(cone, c, { c.x, c.y - 0.01f, c.z }, 0.02f, true), 0.0f);

    FluidSource ring = sphere;
    ring.shape = FluidSourceShape::Ring;
    ring.size = { 0.4f, 0.1f, 0.0f };
    ring.direction = { 0.0f, 1.0f, 0.0f };
    EXPECT_FLOAT_EQ(asset::FluidSourceWeight(ring, c, { c.x + 0.4f, c.y, c.z }, 0.01f, true), 1.0f);
    EXPECT_EQ(asset::FluidSourceWeight(ring, c, c, 0.01f, true), 0.0f);
}

TEST(FluidSegmentShapeTest, ExistingColliderShapesAreUnchanged)
{
    const math::Vector3 c{ 0.1f, -0.2f, 0.05f };

    FluidCollider sphere;
    sphere.shape = FluidColliderShape::Sphere;
    sphere.center = c;
    sphere.size = { 0.3f, 0.3f, 0.3f };
    EXPECT_FLOAT_EQ(asset::FluidColliderDistance(sphere, c, c, 0.0f, true), -0.3f);
    EXPECT_NEAR(asset::FluidColliderDistance(sphere, c, { c.x + 0.5f, c.y, c.z }, 0.0f, true), 0.2f, 1.0e-6f);
    ExpectVectorNear(asset::FluidColliderNormal(sphere, c, { c.x + 0.5f, c.y, c.z }, 0.0f, true),
                     { 1.0f, 0.0f, 0.0f });

    FluidCollider box = sphere;
    box.shape = FluidColliderShape::Box;
    box.size = { 0.3f, 0.2f, 0.25f };
    EXPECT_NEAR(asset::FluidColliderDistance(box, c, { c.x + 0.4f, c.y, c.z }, 0.0f, true), 0.1f, 1.0e-6f);
    EXPECT_FLOAT_EQ(asset::FluidColliderDistance(box, c, c, 0.0f, true), -0.2f);
    // 箱の中は «いちばん浅い軸» (ここでは y の 0.2) の向き。
    ExpectVectorNear(asset::FluidColliderNormal(box, c, c, 0.0f, true), { 0.0f, 1.0f, 0.0f });

    FluidCollider plane = sphere;
    plane.shape = FluidColliderShape::Plane;
    plane.direction = { 0.0f, 2.0f, 0.0f };
    EXPECT_NEAR(asset::FluidColliderDistance(plane, c, { c.x, c.y + 0.4f, c.z }, 0.0f, true), 0.4f, 1.0e-6f);
    EXPECT_NEAR(asset::FluidColliderDistance(plane, c, { c.x, c.y - 0.4f, c.z }, 0.0f, true), -0.4f, 1.0e-6f);
    ExpectVectorNear(asset::FluidColliderNormal(plane, c, c, 0.0f, true), { 0.0f, 1.0f, 0.0f });
}

} // namespace fbzz::tests
