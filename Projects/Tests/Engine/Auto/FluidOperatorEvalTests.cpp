/// @file    FluidOperatorEvalTests.cpp
/// @brief   流体レシピの部品 (発生源の形・力・動き) の評価式と、それをソルバーが使うことを固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// 同じ式を GPU (FluidGpuCommon.hlsli) が写しで持つ。ここで固定した値が CPU 側の正本になる。

#include <TestKit/TestKit.hpp>

#include <Engine/Asset/FluidOperatorEval.hpp>
#include <Engine/Asset/FluidSolver.hpp>
#include <Engine/Core/CurlNoise.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fbzz::tests {
namespace {

using asset::FluidCollider;
using asset::FluidColliderShape;
using asset::FluidForce;
using asset::FluidForceType;
using asset::FluidSource;
using asset::FluidSourceShape;

float Length(const math::Vector3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

void ExpectVectorNear(const math::Vector3& actual, const math::Vector3& expected, float tolerance = 1.0e-5f)
{
    EXPECT_NEAR(actual.x, expected.x, tolerance);
    EXPECT_NEAR(actual.y, expected.y, tolerance);
    EXPECT_NEAR(actual.z, expected.z, tolerance);
}

FluidSource MakeSource(FluidSourceShape shape)
{
    FluidSource source;
    source.shape = shape;
    source.center = { 0.1f, -0.2f, 0.05f };
    source.direction = { 0.0f, 1.0f, 0.0f };
    switch (shape) {
    case FluidSourceShape::Sphere: source.size = { 0.3f, 0.3f, 0.3f }; break;
    case FluidSourceShape::Box:    source.size = { 0.3f, 0.2f, 0.25f }; break;
    case FluidSourceShape::Cone:   source.size = { 0.2f, 0.5f, 0.0f }; break;
    case FluidSourceShape::Ring:   source.size = { 0.4f, 0.1f, 0.0f }; break;
    case FluidSourceShape::Texture:
        source.size = { 0.4f, 0.3f, 0.05f };
        source.direction = { 0.0f, 0.0f, 1.0f };
        break;
    case FluidSourceShape::Capsule:
    case FluidSourceShape::Cylinder:
        source.size = { 0.15f, 0.4f, 0.0f };   // R = 0.15、H = 0.4 (上向き)
        break;
    }
    return source;
}

// 左半分が 1、右半分が 0 のマスク (画像を読まずに手で組む)。
asset::FluidSourceMask LeftHalfMask()
{
    const int size = asset::kFluidSourceMaskSize;
    asset::FluidSourceMask mask;
    mask.values.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), 0.0f);
    for (int row = 0; row < size; ++row)
        for (int column = 0; column < size / 2; ++column)
            mask.values[static_cast<std::size_t>(row) * static_cast<std::size_t>(size) + static_cast<std::size_t>(column)] = 1.0f;
    return mask;
}

FluidCollider MakeCollider(FluidColliderShape shape)
{
    FluidCollider collider;
    collider.shape = shape;
    collider.center = { 0.1f, -0.2f, 0.05f };
    collider.direction = { 0.0f, 2.0f, 0.0f };
    collider.size = shape == FluidColliderShape::Box ? math::Vector3{ 0.3f, 0.2f, 0.25f }
                                                     : math::Vector3{ 0.3f, 0.3f, 0.3f };
    return collider;
}

float Distance(const FluidCollider& collider, const math::Vector3& offset, bool volumetric = true, float minSize = 0.0f)
{
    return asset::FluidColliderDistance(collider, collider.center, collider.center + offset, minSize, volumetric);
}

math::Vector3 Normal(const FluidCollider& collider, const math::Vector3& offset, bool volumetric = true)
{
    return asset::FluidColliderNormal(collider, collider.center, collider.center + offset, 0.0f, volumetric);
}

asset::FluidMotion TwoKeyMotion()
{
    asset::FluidMotion motion;
    motion.keys.push_back({ 0.5f, { 1.0f, 0.0f, 0.0f } });
    motion.keys.push_back({ 1.5f, { 3.0f, 2.0f, 0.0f } });
    return motion;
}

FluidForce MakeForce(FluidForceType type)
{
    FluidForce force;
    force.type = type;
    force.center = { 0.0f, 0.0f, 0.0f };
    force.direction = { 1.0f, 0.0f, 0.0f };
    force.strength = 2.0f;
    force.radius = 0.0f;
    return force;
}

math::Vector3 Delta(const FluidForce& force, const math::Vector3& p, const math::Vector3& v, bool volumetric,
                    float dt = 0.1f, float strengthScale = 1.0f)
{
    return asset::FluidForceDelta(force, force.center, p, v, 0.0f, dt, { 0.0f, 0.0f, 0.0f }, 0, volumetric,
                                  strengthScale);
}

// 中央 (各軸の内側半分) の渦度 (∂vy/∂x − ∂vx/∂y) の総和。
// WHY 全体で足さないか: 総和は縁を一周する循環に等しく、半径の内側で閉じた渦では 0 になってしまう。
float InnerCurl(const asset::FluidGasSolver& solver)
{
    const float h = solver.CellSize();
    double total = 0.0;
    for (int y = solver.SizeY() / 4; y < solver.SizeY() * 3 / 4; ++y)
        for (int x = solver.SizeX() / 4; x < solver.SizeX() * 3 / 4; ++x) {
            const float dvy = solver.VelocityY()[solver.Index(x + 1, y, 0)] - solver.VelocityY()[solver.Index(x - 1, y, 0)];
            const float dvx = solver.VelocityX()[solver.Index(x, y + 1, 0)] - solver.VelocityX()[solver.Index(x, y - 1, 0)];
            total += static_cast<double>((dvy - dvx) / (2.0f * h));
        }
    return static_cast<float>(total);
}

float MeanX(const asset::FluidLiquidSolver& solver)
{
    double total = 0.0;
    for (const auto& particle : solver.Particles()) total += particle.x;
    return solver.Particles().empty() ? 0.0f : static_cast<float>(total / static_cast<double>(solver.Particles().size()));
}

} // namespace

// ── 動き ──

TEST(FluidOperatorEvalTest, MotionWithoutKeysDoesNotMove)
{
    const asset::FluidMotionSample sample = asset::SampleFluidMotion({}, 1.0f);
    EXPECT_EQ(Length(sample.offset), 0.0f);
    EXPECT_EQ(Length(sample.velocity), 0.0f);
}

TEST(FluidOperatorEvalTest, MotionHoldsTheEndKeysOutsideItsRange)
{
    const asset::FluidMotion motion = TwoKeyMotion();
    const asset::FluidMotionSample before = asset::SampleFluidMotion(motion, 0.0f);
    EXPECT_FLOAT_EQ(before.offset.x, 1.0f);
    EXPECT_EQ(Length(before.velocity), 0.0f);
    const asset::FluidMotionSample after = asset::SampleFluidMotion(motion, 9.0f);
    EXPECT_FLOAT_EQ(after.offset.x, 3.0f);
    EXPECT_FLOAT_EQ(after.offset.y, 2.0f);
    EXPECT_EQ(Length(after.velocity), 0.0f);

    asset::FluidMotion single;
    single.keys.push_back({ 1.0f, { 0.0f, 0.5f, 0.0f } });
    EXPECT_FLOAT_EQ(asset::SampleFluidMotion(single, 0.0f).offset.y, 0.5f);
    EXPECT_FLOAT_EQ(asset::SampleFluidMotion(single, 2.0f).offset.y, 0.5f);
    EXPECT_EQ(Length(asset::SampleFluidMotion(single, 2.0f).velocity), 0.0f);
}

TEST(FluidOperatorEvalTest, MotionInterpolatesBetweenKeysAndReportsTheSlope)
{
    const asset::FluidMotionSample sample = asset::SampleFluidMotion(TwoKeyMotion(), 1.0f);
    EXPECT_NEAR(sample.offset.x, 2.0f, 1.0e-6f);
    EXPECT_NEAR(sample.offset.y, 1.0f, 1.0e-6f);
    EXPECT_NEAR(sample.velocity.x, 2.0f, 1.0e-5f);
    EXPECT_NEAR(sample.velocity.y, 2.0f, 1.0e-5f);
    EXPECT_EQ(sample.velocity.z, 0.0f);
}

TEST(FluidOperatorEvalTest, PoseAddsTheMotionAndInheritsItsVelocityOnlyWhenAsked)
{
    FluidSource source;
    source.center = { 0.1f, 0.0f, 0.0f };
    source.motion = TwoKeyMotion();
    source.motion.inheritVelocity = true;
    asset::FluidOperatorPose pose = asset::PoseFluidSource(source, 1.0f);
    EXPECT_NEAR(pose.center.x, 2.1f, 1.0e-5f);
    EXPECT_NEAR(pose.center.y, 1.0f, 1.0e-5f);
    EXPECT_NEAR(pose.motionVelocity.x, 2.0f, 1.0e-5f);

    source.motion.inheritVelocity = false;
    pose = asset::PoseFluidSource(source, 1.0f);
    EXPECT_NEAR(pose.center.x, 2.1f, 1.0e-5f);
    EXPECT_EQ(Length(pose.motionVelocity), 0.0f);

    FluidForce force;
    force.center = { 0.0f, -1.0f, 0.0f };
    force.motion = TwoKeyMotion();
    force.motion.inheritVelocity = true;
    const asset::FluidOperatorPose forcePose = asset::PoseFluidForce(force, 1.0f);
    EXPECT_NEAR(forcePose.center.x, 2.0f, 1.0e-5f);
    EXPECT_NEAR(forcePose.center.y, 0.0f, 1.0e-5f);
    EXPECT_NEAR(forcePose.motionVelocity.y, 2.0f, 1.0e-5f);
}

TEST(FluidOperatorEvalTest, EmittingAndActiveWindows)
{
    FluidSource source;
    source.startTime = 0.5f;
    source.duration = 0.0f;
    EXPECT_FALSE(asset::FluidSourceEmitting(source, 0.4f));
    EXPECT_TRUE(asset::FluidSourceEmitting(source, 0.5f));
    EXPECT_TRUE(asset::FluidSourceEmitting(source, 100.0f));
    source.duration = 0.25f;
    EXPECT_TRUE(asset::FluidSourceEmitting(source, 0.74f));
    EXPECT_FALSE(asset::FluidSourceEmitting(source, 0.75f));
    source.enabled = false;
    EXPECT_FALSE(asset::FluidSourceEmitting(source, 0.6f));

    FluidForce force;
    force.startTime = 1.0f;
    force.duration = 0.5f;
    EXPECT_FALSE(asset::FluidForceActive(force, 0.9f));
    EXPECT_TRUE(asset::FluidForceActive(force, 1.2f));
    EXPECT_FALSE(asset::FluidForceActive(force, 1.5f));
    force.duration = 0.0f;
    EXPECT_TRUE(asset::FluidForceActive(force, 50.0f));
    force.enabled = false;
    EXPECT_FALSE(asset::FluidForceActive(force, 50.0f));
}

// ── 発生源の形 ──

TEST(FluidOperatorEvalTest, SphereWeightMatchesTheLegacyGasFormula)
{
    const FluidSource source = MakeSource(FluidSourceShape::Sphere);
    const math::Vector3 c = source.center;
    EXPECT_FLOAT_EQ(asset::FluidSourceWeight(source, c, c, 0.01f, true), 1.0f);
    const math::Vector3 points[] = { { 0.2f, -0.1f, 0.0f }, { -0.05f, -0.3f, 0.2f }, { 0.35f, -0.2f, 0.05f } };
    for (const math::Vector3& p : points) {
        // 旧 FluidGasSolver::Inject の式そのまま。
        const float r = 0.3f;
        const float dx = (p.x - c.x) / r;
        const float dy = (p.y - c.y) / r;
        const float dz = (p.z - c.z) / r;
        const float q2 = dx * dx + dy * dy + dz * dz;
        const float expected = q2 >= 1.0f ? 0.0f : (1.0f - q2) * (1.0f - q2);
        EXPECT_FLOAT_EQ(asset::FluidSourceWeight(source, c, p, 0.01f, true), expected);
    }
    EXPECT_EQ(asset::FluidSourceWeight(source, c, { 0.5f, -0.2f, 0.05f }, 0.01f, true), 0.0f);
    // 格子より小さい球はセル幅まで広げる (広げないと 1 セルにも入らない)。
    FluidSource tiny = source;
    tiny.size = { 0.001f, 0.001f, 0.001f };
    EXPECT_GT(asset::FluidSourceWeight(tiny, c, { c.x + 0.05f, c.y, c.z }, 0.1f, true), 0.0f);
}

TEST(FluidOperatorEvalTest, BoxWeightMatchesTheLegacyGasFormula)
{
    const FluidSource source = MakeSource(FluidSourceShape::Box);
    const math::Vector3 c = source.center;
    EXPECT_FLOAT_EQ(asset::FluidSourceWeight(source, c, c, 0.01f, true), 1.0f);
    const math::Vector3 points[] = { { 0.3f, -0.15f, 0.1f }, { -0.15f, -0.35f, -0.15f }, { 0.39f, -0.2f, 0.05f } };
    for (const math::Vector3& p : points) {
        const float q = (std::max)(std::fabs(p.x - c.x) / 0.3f,
                                   (std::max)(std::fabs(p.y - c.y) / 0.2f, std::fabs(p.z - c.z) / 0.25f));
        const float expected = q >= 1.0f ? 0.0f : std::clamp((1.0f - q) * 4.0f, 0.0f, 1.0f);
        EXPECT_FLOAT_EQ(asset::FluidSourceWeight(source, c, p, 0.01f, true), expected);
    }
    EXPECT_EQ(asset::FluidSourceWeight(source, c, { c.x, c.y + 0.21f, c.z }, 0.01f, true), 0.0f);
}

TEST(FluidOperatorEvalTest, ConeWeightOpensFromTheApexAlongTheDirection)
{
    const FluidSource source = MakeSource(FluidSourceShape::Cone);   // R = 0.2, L = 0.5, 上向き
    const math::Vector3 c = source.center;
    const auto at = [&](float along, float across) {
        return asset::FluidSourceWeight(source, c, { c.x + across, c.y + along, c.z }, 0.02f, true);
    };
    EXPECT_FLOAT_EQ(at(0.0f, 0.0f), 1.0f);                      // 頂点は minSize の半分の太さを持つ
    EXPECT_FLOAT_EQ(at(0.25f, 0.0f), 1.0f);
    EXPECT_NEAR(at(0.45f, 0.0f), 0.4f, 1.0e-5f);                // 底の手前でなめらかに 0 へ
    EXPECT_GT(at(0.25f, 0.05f), 0.0f);
    EXPECT_EQ(at(0.25f, 0.11f), 0.0f);                          // その高さの許容半径 0.1 の外
    EXPECT_EQ(at(-0.01f, 0.0f), 0.0f);                          // 頂点の後ろ
    EXPECT_EQ(at(0.51f, 0.0f), 0.0f);                           // 底の先
}

TEST(FluidOperatorEvalTest, RingWeightPeaksOnTheRingAndIsEmptyInTheMiddle)
{
    const FluidSource source = MakeSource(FluidSourceShape::Ring);   // R = 0.4, r = 0.1, 法線は上
    const math::Vector3 c = source.center;
    EXPECT_FLOAT_EQ(asset::FluidSourceWeight(source, c, { c.x + 0.4f, c.y, c.z }, 0.01f, true), 1.0f);
    EXPECT_FLOAT_EQ(asset::FluidSourceWeight(source, c, { c.x, c.y, c.z - 0.4f }, 0.01f, true), 1.0f);
    EXPECT_GT(asset::FluidSourceWeight(source, c, { c.x + 0.45f, c.y + 0.03f, c.z }, 0.01f, true), 0.0f);
    EXPECT_EQ(asset::FluidSourceWeight(source, c, c, 0.01f, true), 0.0f);
    EXPECT_EQ(asset::FluidSourceWeight(source, c, { c.x + 0.4f, c.y + 0.11f, c.z }, 0.01f, true), 0.0f);
}

TEST(FluidOperatorEvalTest, FlatWeightIgnoresDepth)
{
    const FluidSource source = MakeSource(FluidSourceShape::Sphere);
    const math::Vector3 c = source.center;
    const float flat = asset::FluidSourceWeight(source, c, { c.x + 0.1f, c.y, c.z }, 0.01f, false);
    EXPECT_FLOAT_EQ(asset::FluidSourceWeight(source, c, { c.x + 0.1f, c.y, c.z + 5.0f }, 0.01f, false), flat);
    EXPECT_EQ(asset::FluidSourceWeight(source, c, { c.x + 0.1f, c.y, c.z + 5.0f }, 0.01f, true), 0.0f);
}

TEST(FluidOperatorEvalTest, SampledPointsLandInsideEveryShape)
{
    std::vector<FluidSource> sources = { MakeSource(FluidSourceShape::Sphere), MakeSource(FluidSourceShape::Box),
                                         MakeSource(FluidSourceShape::Cone), MakeSource(FluidSourceShape::Ring) };
    FluidSource tiltedCone = MakeSource(FluidSourceShape::Cone);
    tiltedCone.size = { 0.4f, 0.5f, 0.0f };        // 傾けても z = center.z の断面が三角形として残る開き
    tiltedCone.direction = { 0.3f, 0.8f, 0.5f };
    sources.push_back(tiltedCone);
    FluidSource sidewaysCone = MakeSource(FluidSourceShape::Cone);
    sidewaysCone.direction = { -1.0f, 0.2f, 0.0f };
    sources.push_back(sidewaysCone);
    FluidSource flatRing = MakeSource(FluidSourceShape::Ring);
    flatRing.direction = { 0.0f, 0.0f, 1.0f };      // 画面内に寝た輪 (2D では輪帯)
    sources.push_back(flatRing);
    FluidSource tiltedRing = MakeSource(FluidSourceShape::Ring);
    tiltedRing.direction = { 0.4f, 0.7f, 0.6f };
    sources.push_back(tiltedRing);
    sources.push_back(MakeSource(FluidSourceShape::Capsule));    // 上向きのカプセル
    sources.push_back(MakeSource(FluidSourceShape::Cylinder));   // 上向きの円柱
    for (const FluidSourceShape shape : { FluidSourceShape::Capsule, FluidSourceShape::Cylinder }) {
        FluidSource lying = MakeSource(shape);
        lying.direction = { 1.0f, 0.0f, 0.0f };                  // 画面内に寝た棒 (2D の断面がいちばん大きい)
        sources.push_back(lying);
        FluidSource tilted = MakeSource(shape);
        tilted.direction = { 0.3f, 0.5f, 0.8f };                 // 奥へ傾けた棒 (2D の断面は細い楕円)
        sources.push_back(tilted);
        FluidSource facing = MakeSource(shape);
        facing.direction = { 0.0f, 0.0f, 1.0f };                 // 真奥向き (2D の断面は円)
        sources.push_back(facing);
    }
    sources.push_back(MakeSource(FluidSourceShape::Texture));   // 正面向きの板
    FluidSource lyingPlate = MakeSource(FluidSourceShape::Texture);
    lyingPlate.direction = { 0.0f, 1.0f, 0.0f };                // 寝た板 (2D では厚みの帯)
    sources.push_back(lyingPlate);
    FluidSource tiltedPlate = MakeSource(FluidSourceShape::Texture);
    tiltedPlate.direction = { 0.3f, 0.5f, 0.8f };
    sources.push_back(tiltedPlate);

    // 形の縁 (u = 0 / 1) ちょうどは重み 0 になりうるので、内側の値だけで確かめる。
    const float us[] = { 0.1f, 0.3f, 0.6f, 0.85f };
    for (std::size_t s = 0; s < sources.size(); ++s) {
        const FluidSource& source = sources[s];
        for (const bool volumetric : { false, true }) {
            for (const float u0 : us)
                for (const float u1 : us)
                    for (const float u2 : us) {
                        const math::Vector3 p = asset::SampleFluidSourcePoint(source, source.center, u0, u1, u2, volumetric);
                        EXPECT_GT(asset::FluidSourceWeight(source, source.center, p, 0.0f, volumetric), 0.0f)
                            << "source " << s << " volumetric " << volumetric << " u " << u0 << "," << u1 << "," << u2;
                        if (!volumetric) {
                            EXPECT_EQ(p.z, source.center.z);
                        }
                    }
        }
    }
}

// ── テクスチャ発生源 ──

TEST(FluidOperatorEvalTest, TexturePlateFacesTheViewerByDefault)
{
    FluidSource plate = MakeSource(FluidSourceShape::Texture);
    math::Vector3 right;
    math::Vector3 up;
    math::Vector3 normal;
    asset::FluidTextureSourceBasis(plate, right, up, normal);
    ExpectVectorNear(right, { 1.0f, 0.0f, 0.0f });
    ExpectVectorNear(up, { 0.0f, 1.0f, 0.0f });
    ExpectVectorNear(normal, { 0.0f, 0.0f, 1.0f });

    // 向き 0 は手前向き (Cone / Ring の上向きとは違う)。
    plate.direction = { 0.0f, 0.0f, 0.0f };
    asset::FluidTextureSourceBasis(plate, right, up, normal);
    ExpectVectorNear(right, { 1.0f, 0.0f, 0.0f });
    ExpectVectorNear(up, { 0.0f, 1.0f, 0.0f });
    ExpectVectorNear(normal, { 0.0f, 0.0f, 1.0f });

    // 真上向きは cross((0,1,0), n) が潰れるので right を x に固定する。
    plate.direction = { 0.0f, 3.0f, 0.0f };
    asset::FluidTextureSourceBasis(plate, right, up, normal);
    ExpectVectorNear(right, { 1.0f, 0.0f, 0.0f });
    ExpectVectorNear(up, { 0.0f, 0.0f, -1.0f });
    ExpectVectorNear(normal, { 0.0f, 1.0f, 0.0f });

    // 横向きの板でも画像の上は y のまま。
    plate.direction = { 1.0f, 0.0f, 0.0f };
    asset::FluidTextureSourceBasis(plate, right, up, normal);
    ExpectVectorNear(right, { 0.0f, 0.0f, -1.0f });
    ExpectVectorNear(up, { 0.0f, 1.0f, 0.0f });
}

TEST(FluidOperatorEvalTest, TextureWeightFollowsTheMask)
{
    const FluidSource plate = MakeSource(FluidSourceShape::Texture);   // 半幅 0.4・半高さ 0.3・厚み 0.05
    const math::Vector3 c = plate.center;
    const asset::FluidSourceMask mask = LeftHalfMask();
    const asset::FluidSourceMask missing;   // 読めていないマスクは 1 (= 板の形)
    for (const bool volumetric : { false, true }) {
        for (int i = -9; i <= 9; ++i) {
            if (i == 0) continue;   // 左右の境目は双線形で混ざる
            for (const float y : { -0.2f, 0.0f, 0.2f }) {
                const math::Vector3 p = { c.x + 0.04f * static_cast<float>(i), c.y + y, c.z };
                const float weight = asset::FluidTextureSourceWeight(plate, c, p, 0.01f, volumetric, mask);
                const float plain = asset::FluidSourceWeight(plate, c, p, 0.01f, volumetric);
                EXPECT_GT(plain, 0.0f);
                EXPECT_FLOAT_EQ(asset::FluidTextureSourceWeight(plate, c, p, 0.01f, volumetric, missing), plain);
                if (i < 0) {
                    EXPECT_NEAR(weight, plain, 1.0e-5f) << i << " " << y;
                } else {
                    EXPECT_EQ(weight, 0.0f) << i << " " << y;
                }
            }
        }
    }
    // 板の外 (横と厚み)。2D は奥行きを見ないので厚みの外でも湧く。
    EXPECT_EQ(asset::FluidTextureSourceWeight(plate, c, { c.x - 0.45f, c.y, c.z }, 0.01f, true, mask), 0.0f);
    EXPECT_EQ(asset::FluidTextureSourceWeight(plate, c, { c.x - 0.2f, c.y + 0.31f, c.z }, 0.01f, true, mask), 0.0f);
    EXPECT_EQ(asset::FluidTextureSourceWeight(plate, c, { c.x - 0.2f, c.y, c.z + 0.06f }, 0.01f, true, mask), 0.0f);
    EXPECT_GT(asset::FluidTextureSourceWeight(plate, c, { c.x - 0.2f, c.y, c.z + 0.06f }, 0.01f, false, mask), 0.0f);
}

TEST(FluidOperatorEvalTest, TextureSamplingRejectsWhereTheMaskIsEmpty)
{
    const FluidSource plate = MakeSource(FluidSourceShape::Texture);
    const asset::FluidSourceMask mask = LeftHalfMask();
    const asset::FluidSourceMask missing;
    const float us[] = { 0.1f, 0.3f, 0.6f, 0.85f };
    for (const bool volumetric : { false, true }) {
        for (const float u0 : { 0.1f, 0.3f, 0.7f, 0.9f })
            for (const float u1 : us)
                for (const float u2 : us) {
                    math::Vector3 point = { 9.0f, 9.0f, 9.0f };
                    const bool accepted = asset::SampleFluidTextureSourcePoint(plate, plate.center, mask, u0, u1, u2,
                                                                               0.0f, volumetric, point);
                    if (u0 < 0.5f) {
                        ASSERT_TRUE(accepted) << u0 << "," << u1 << "," << u2;
                        EXPECT_GT(asset::FluidTextureSourceWeight(plate, plate.center, point, 0.0f, volumetric, mask), 0.0f);
                        if (!volumetric) {
                            EXPECT_EQ(point.z, plate.center.z);
                        }
                    } else {
                        // マスク 0 は accept が 0 でも外れる。
                        EXPECT_FALSE(accepted) << u0 << "," << u1 << "," << u2;
                    }
                    // 画像が無ければ accept が 1 の手前でも必ず通る。
                    EXPECT_TRUE(asset::SampleFluidTextureSourcePoint(plate, plate.center, missing, u0, u1, u2,
                                                                     0.999f, volumetric, point));
                }
    }
}

// ── 障害物 ──

TEST(FluidOperatorEvalTest, ColliderPoseAndWindowFollowTheSourceRules)
{
    FluidCollider collider = MakeCollider(FluidColliderShape::Sphere);
    collider.motion = TwoKeyMotion();
    asset::FluidOperatorPose pose = asset::PoseFluidCollider(collider, 1.0f);
    ExpectVectorNear(pose.center, { 2.1f, 0.8f, 0.05f });
    ExpectVectorNear(pose.motionVelocity, { 2.0f, 2.0f, 0.0f });
    collider.motion.inheritVelocity = false;
    pose = asset::PoseFluidCollider(collider, 1.0f);
    EXPECT_EQ(Length(pose.motionVelocity), 0.0f);

    collider.startTime = 0.5f;
    collider.duration = 0.25f;
    EXPECT_FALSE(asset::FluidColliderActive(collider, 0.4f));
    EXPECT_TRUE(asset::FluidColliderActive(collider, 0.6f));
    EXPECT_FALSE(asset::FluidColliderActive(collider, 0.75f));
    collider.duration = 0.0f;
    EXPECT_TRUE(asset::FluidColliderActive(collider, 99.0f));
    collider.enabled = false;
    EXPECT_FALSE(asset::FluidColliderActive(collider, 0.6f));
}

TEST(FluidOperatorEvalTest, ColliderDistanceIsSignedAndCrossesZeroOnTheSurface)
{
    const FluidCollider sphere = MakeCollider(FluidColliderShape::Sphere);   // 半径 0.3
    EXPECT_NEAR(Distance(sphere, { 0.0f, 0.0f, 0.0f }), -0.3f, 1.0e-6f);
    EXPECT_NEAR(Distance(sphere, { 0.3f, 0.0f, 0.0f }), 0.0f, 1.0e-6f);
    EXPECT_LT(Distance(sphere, { 0.29f, 0.0f, 0.0f }), 0.0f);
    EXPECT_GT(Distance(sphere, { 0.31f, 0.0f, 0.0f }), 0.0f);
    EXPECT_NEAR(Distance(sphere, { 0.0f, -0.5f, 0.0f }), 0.2f, 1.0e-6f);
    // セル幅より小さい障害物は minSize まで広げる。
    FluidCollider tiny = sphere;
    tiny.size = { 0.001f, 0.001f, 0.001f };
    EXPECT_LT(Distance(tiny, { 0.05f, 0.0f, 0.0f }, true, 0.1f), 0.0f);

    const FluidCollider box = MakeCollider(FluidColliderShape::Box);          // 半分 (0.3, 0.2, 0.25)
    EXPECT_NEAR(Distance(box, { 0.0f, 0.0f, 0.0f }), -0.2f, 1.0e-6f);
    EXPECT_NEAR(Distance(box, { 0.3f, 0.0f, 0.0f }), 0.0f, 1.0e-6f);
    EXPECT_LT(Distance(box, { 0.0f, 0.19f, 0.0f }), 0.0f);
    EXPECT_GT(Distance(box, { 0.0f, 0.21f, 0.0f }), 0.0f);
    EXPECT_NEAR(Distance(box, { 0.4f, 0.0f, 0.0f }), 0.1f, 1.0e-6f);
    EXPECT_NEAR(Distance(box, { 0.4f, 0.3f, 0.0f }), std::sqrt(0.02f), 1.0e-5f);   // 角の外は角までの距離

    FluidCollider plane = MakeCollider(FluidColliderShape::Plane);            // 法線 +y (長さは問わない)
    EXPECT_NEAR(Distance(plane, { 0.3f, 0.4f, 0.0f }), 0.4f, 1.0e-6f);
    EXPECT_NEAR(Distance(plane, { 5.0f, -0.1f, 3.0f }), -0.1f, 1.0e-6f);
    EXPECT_NEAR(Distance(plane, { 5.0f, 0.0f, -2.0f }), 0.0f, 1.0e-6f);
    plane.direction = { 1.0f, 1.0f, 0.0f };
    EXPECT_NEAR(Distance(plane, { 1.0f, 0.0f, 0.0f }), std::sqrt(0.5f), 1.0e-5f);
    plane.direction = { 0.0f, 0.0f, 0.0f };                                     // 長さ 0 は上向き
    EXPECT_NEAR(Distance(plane, { 0.0f, -0.25f, 0.0f }), -0.25f, 1.0e-6f);

    // 2D は奥行きの差を見ない (z = center.z の断面の円)。
    EXPECT_NEAR(Distance(sphere, { 0.2f, 0.0f, 5.0f }, false), -0.1f, 1.0e-6f);
    EXPECT_GT(Distance(sphere, { 0.2f, 0.0f, 5.0f }, true), 0.0f);
    EXPECT_LT(Distance(box, { 0.1f, 0.1f, 5.0f }, false), 0.0f);
}

TEST(FluidOperatorEvalTest, ColliderNormalsPointOutOfTheSurface)
{
    const FluidCollider sphere = MakeCollider(FluidColliderShape::Sphere);
    ExpectVectorNear(Normal(sphere, { 0.5f, 0.0f, 0.0f }), { 1.0f, 0.0f, 0.0f });
    ExpectVectorNear(Normal(sphere, { 0.0f, -0.1f, 0.0f }), { 0.0f, -1.0f, 0.0f });
    ExpectVectorNear(Normal(sphere, { 0.0f, 0.0f, 0.0f }), { 0.0f, 1.0f, 0.0f });   // 中心は上へ倒す
    const float diagonal = std::sqrt(0.5f);

    const FluidCollider box = MakeCollider(FluidColliderShape::Box);
    ExpectVectorNear(Normal(box, { 0.4f, 0.0f, 0.0f }), { 1.0f, 0.0f, 0.0f });
    ExpectVectorNear(Normal(box, { 0.4f, 0.3f, 0.0f }), { diagonal, diagonal, 0.0f });
    // 中では最も浅い面の向き。
    ExpectVectorNear(Normal(box, { 0.25f, 0.05f, 0.0f }), { 1.0f, 0.0f, 0.0f });
    ExpectVectorNear(Normal(box, { -0.25f, 0.0f, 0.0f }), { -1.0f, 0.0f, 0.0f });
    ExpectVectorNear(Normal(box, { 0.0f, -0.15f, 0.0f }), { 0.0f, -1.0f, 0.0f });
    ExpectVectorNear(Normal(box, { 0.0f, 0.0f, 0.2f }), { 0.0f, 0.0f, 1.0f });

    FluidCollider plane = MakeCollider(FluidColliderShape::Plane);
    ExpectVectorNear(Normal(plane, { 3.0f, -1.0f, 2.0f }), { 0.0f, 1.0f, 0.0f });
    plane.direction = { 1.0f, 1.0f, 0.0f };
    ExpectVectorNear(Normal(plane, { 0.0f, 0.0f, 0.0f }), { diagonal, diagonal, 0.0f });

    // 2D は z を落として正規化し直す。潰れたら上向き。
    ExpectVectorNear(Normal(sphere, { 0.2f, 0.0f, 5.0f }, false), { 1.0f, 0.0f, 0.0f });
    ExpectVectorNear(Normal(box, { 0.4f, 0.0f, 0.5f }, false), { 1.0f, 0.0f, 0.0f });
    plane.direction = { 0.0f, 0.0f, 1.0f };
    ExpectVectorNear(Normal(plane, { 0.0f, 0.0f, 0.0f }, false), { 0.0f, 1.0f, 0.0f });
    plane.direction = { 3.0f, 0.0f, 4.0f };
    ExpectVectorNear(Normal(plane, { 0.0f, 0.0f, 0.0f }, false), { 1.0f, 0.0f, 0.0f });
    for (const math::Vector3& offset : { math::Vector3{ 0.2f, 0.1f, 0.3f }, math::Vector3{ -0.4f, 0.5f, -0.2f } }) {
        EXPECT_EQ(Normal(sphere, offset, false).z, 0.0f);
        EXPECT_NEAR(Length(Normal(sphere, offset, false)), 1.0f, 1.0e-5f);
        EXPECT_NEAR(Length(Normal(box, offset, false)), 1.0f, 1.0e-5f);
    }
}

// ── 力 ──

TEST(FluidOperatorEvalTest, WindPushesAlongItsDirection)
{
    FluidForce wind = MakeForce(FluidForceType::Wind);
    wind.direction = { 2.0f, 0.0f, 0.0f };
    wind.strength = 3.0f;
    const math::Vector3 dv = Delta(wind, { 0.3f, 0.2f, 0.0f }, { 0.0f, 0.0f, 0.0f }, true);
    EXPECT_NEAR(dv.x, 0.3f, 1.0e-6f);
    EXPECT_EQ(dv.y, 0.0f);
    EXPECT_EQ(dv.z, 0.0f);
    // 向きが 0 なら x 軸へ倒す。
    wind.direction = { 0.0f, 0.0f, 0.0f };
    EXPECT_NEAR(Delta(wind, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, true).x, 0.3f, 1.0e-6f);
    // 強さ 0 は何もしない。
    wind.strength = 0.0f;
    EXPECT_EQ(Length(Delta(wind, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, true)), 0.0f);
}

TEST(FluidOperatorEvalTest, AttractAndRepulsePointTowardAndAwayFromTheCenter)
{
    const math::Vector3 p = { 0.5f, 0.0f, 0.0f };
    const math::Vector3 attract = Delta(MakeForce(FluidForceType::Attract), p, { 0.0f, 0.0f, 0.0f }, true);
    const math::Vector3 repulse = Delta(MakeForce(FluidForceType::Repulse), p, { 0.0f, 0.0f, 0.0f }, true);
    EXPECT_NEAR(attract.x, -0.2f, 1.0e-6f);
    EXPECT_NEAR(repulse.x, 0.2f, 1.0e-6f);
    EXPECT_EQ(attract.y, 0.0f);
    // 中心ちょうどでは向きが決まらないので 0。
    EXPECT_EQ(Length(Delta(MakeForce(FluidForceType::Attract), { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, true)), 0.0f);
}

TEST(FluidOperatorEvalTest, VortexTurnsCounterClockwiseAroundItsAxis)
{
    const FluidForce vortex = MakeForce(FluidForceType::Vortex);   // 2D の軸は常に画面の奥行き
    const math::Vector3 flat = Delta(vortex, { 0.5f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, false);
    EXPECT_NEAR(flat.x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(flat.y, 0.2f, 1.0e-6f);
    const math::Vector3 flatTop = Delta(vortex, { 0.0f, 0.5f, 0.0f }, { 0.0f, 0.0f, 0.0f }, false);
    EXPECT_NEAR(flatTop.x, -0.2f, 1.0e-6f);

    FluidForce upright = vortex;
    upright.direction = { 0.0f, 1.0f, 0.0f };
    const math::Vector3 volume = Delta(upright, { 0.5f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, true);
    EXPECT_NEAR(volume.z, -0.2f, 1.0e-6f);   // cross(+y, +x) = −z
    EXPECT_NEAR(volume.y, 0.0f, 1.0e-6f);
    // 軸の上では回す向きが無い。
    EXPECT_EQ(Length(Delta(upright, { 0.0f, 0.7f, 0.0f }, { 0.0f, 0.0f, 0.0f }, true)), 0.0f);
}

TEST(FluidOperatorEvalTest, NoiseFollowsTheCurlNoiseFormula)
{
    FluidForce noise = MakeForce(FluidForceType::Noise);
    noise.noiseFrequency = 3.0f;
    noise.noiseSpeed = 1.5f;
    const math::Vector3 p = { 0.3f, -0.2f, 0.1f };
    const math::Vector3 offset = { 4.0f, 5.0f, 6.0f };
    const math::Vector3 dv = asset::FluidForceDelta(noise, noise.center, p, { 0.0f, 0.0f, 0.0f }, 0.7f, 0.1f,
                                                    offset, 2, true, 1.0f);
    const math::Vector3 q = { p.x * 3.0f + 4.0f + 2.0f * 17.31f, p.y * 3.0f + 5.0f - 0.7f * 1.5f,
                              p.z * 3.0f + 6.0f + 2.0f * 5.73f };
    const math::Vector3 curl = core::CurlNoise(q);
    EXPECT_NEAR(dv.x, curl.x * 2.0f * 0.1f, 1.0e-4f);
    EXPECT_NEAR(dv.y, curl.y * 2.0f * 0.1f, 1.0e-4f);
    EXPECT_NEAR(dv.z, curl.z * 2.0f * 0.1f, 1.0e-4f);
    EXPECT_GT(Length(dv), 0.0f);
    // 部品の添字がずれればノイズの切り出し位置も変わる。
    const math::Vector3 other = asset::FluidForceDelta(noise, noise.center, p, { 0.0f, 0.0f, 0.0f }, 0.7f, 0.1f,
                                                       offset, 3, true, 1.0f);
    EXPECT_GT(Length(dv - other), 0.0f);
}

TEST(FluidOperatorEvalTest, DragNeverOvershoots)
{
    FluidForce drag = MakeForce(FluidForceType::Drag);
    drag.strength = 5.0f;
    const math::Vector3 v = { 3.0f, -4.0f, 1.0f };
    for (const float dt : { 0.001f, 0.1f, 1.0f, 100.0f }) {
        const math::Vector3 after = v + Delta(drag, { 0.1f, 0.1f, 0.1f }, v, true, dt);
        EXPECT_LE(Length(after), Length(v) + 1.0e-6f) << dt;
        // 向きは保ったまま縮む (振り戻して逆向きにならない)。
        EXPECT_GE(after.x * v.x + after.y * v.y + after.z * v.z, 0.0f) << dt;
    }
    // 負の強さは «加速» にしない。
    drag.strength = -5.0f;
    EXPECT_EQ(Length(Delta(drag, { 0.1f, 0.1f, 0.1f }, v, true, 0.5f)), 0.0f);
}

TEST(FluidOperatorEvalTest, NegativeStrengthReversesTheFlatVortex)
{
    FluidForce vortex = MakeForce(FluidForceType::Vortex);
    vortex.strength = -2.0f;
    EXPECT_NEAR(Delta(vortex, { 0.5f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, false).y, -0.2f, 1.0e-6f);
}

TEST(FluidOperatorEvalTest, InfluenceFallsOffWithTheRadius)
{
    FluidForce wind = MakeForce(FluidForceType::Wind);
    wind.radius = 1.0f;
    wind.falloffPower = 2.0f;
    const math::Vector3 zero = { 0.0f, 0.0f, 0.0f };
    const float atCenter = Delta(wind, { 0.0f, 0.0f, 0.0f }, zero, true).x;
    const float atHalf = Delta(wind, { 0.5f, 0.0f, 0.0f }, zero, true).x;
    EXPECT_NEAR(atCenter, 0.2f, 1.0e-6f);
    EXPECT_NEAR(atHalf, 0.2f * 0.25f, 1.0e-6f);
    EXPECT_EQ(Delta(wind, { 1.0f, 0.0f, 0.0f }, zero, true).x, 0.0f);
    EXPECT_EQ(Delta(wind, { 0.0f, 1.5f, 0.0f }, zero, true).x, 0.0f);
    // 2D は奥行きの差を距離に数えない。
    EXPECT_NEAR(Delta(wind, { 0.5f, 0.0f, 3.0f }, zero, false).x, atHalf, 1.0e-6f);
}

TEST(FluidOperatorEvalTest, FlatForcesKeepDepthAtZero)
{
    const math::Vector3 p = { 0.2f, 0.3f, 0.4f };
    const math::Vector3 v = { 1.0f, 1.0f, 2.0f };
    for (const FluidForceType type : { FluidForceType::Wind, FluidForceType::Attract, FluidForceType::Repulse,
                                       FluidForceType::Vortex, FluidForceType::Noise, FluidForceType::Drag }) {
        FluidForce force = MakeForce(type);
        force.direction = { 0.3f, 0.2f, 1.0f };
        force.center = { 0.0f, 0.0f, -1.0f };
        EXPECT_EQ(Delta(force, p, v, false).z, 0.0f) << static_cast<int>(type);
    }
}

// ── ソルバーが力を使うこと ──

TEST(FluidOperatorEvalTest, GasVortexForceStirsTheGrid)
{
    asset::FluidRecipe recipe;
    recipe.gas.turbulence = 0.0f;
    recipe.gas.vorticity = 0.0f;
    recipe.gas.buoyancy = 0.0f;
    recipe.gas.weight = 0.0f;

    asset::FluidGasSolver still;
    still.Reset(recipe, 32, 32, 1);
    for (int i = 0; i < 4; ++i) still.Step(1.0f / 30.0f);

    FluidForce vortex = MakeForce(FluidForceType::Vortex);
    vortex.strength = 3.0f;
    vortex.radius = 0.8f;
    recipe.forces.push_back(vortex);
    asset::FluidGasSolver stirred;
    stirred.Reset(recipe, 32, 32, 1);
    for (int i = 0; i < 4; ++i) stirred.Step(1.0f / 30.0f);

    EXPECT_NEAR(InnerCurl(still), 0.0f, 1.0e-6f);
    EXPECT_GT(InnerCurl(stirred), 1.0f);   // 正の強さは反時計回り = 正の渦度

    // 切ってある力は効かない。
    recipe.forces[0].enabled = false;
    asset::FluidGasSolver disabled;
    disabled.Reset(recipe, 32, 32, 1);
    for (int i = 0; i < 4; ++i) disabled.Step(1.0f / 30.0f);
    EXPECT_NEAR(InnerCurl(disabled), 0.0f, 1.0e-6f);
}

TEST(FluidOperatorEvalTest, LiquidWindForcePushesTheParticles)
{
    asset::FluidRecipe recipe;
    recipe.kind = asset::FluidKind::Liquid;
    recipe.liquid.floor = true;
    recipe.liquid.floorHeight = -0.9f;
    FluidSource source;
    source.center = { 0.0f, -0.3f, 0.0f };
    source.size = { 0.1f, 0.1f, 0.1f };
    source.velocity = { 0.0f, 0.0f, 0.0f };
    source.spread = 0.0f;
    source.count = 150;
    source.startTime = 0.0f;
    source.duration = 0.0f;
    recipe.sources.push_back(source);

    const auto solve = [](const asset::FluidRecipe& r) {
        asset::FluidLiquidSolver solver;
        solver.Reset(r);
        for (int i = 0; i < 9; ++i) solver.Advance(1.0f / 30.0f);
        return solver;
    };
    const asset::FluidLiquidSolver calm = solve(recipe);
    FluidForce wind = MakeForce(FluidForceType::Wind);
    wind.strength = 4.0f;
    recipe.forces.push_back(wind);
    const asset::FluidLiquidSolver windy = solve(recipe);

    ASSERT_FALSE(calm.Particles().empty());
    ASSERT_FALSE(windy.Particles().empty());
    // 0.3 秒・加速度 4 なら ½·4·0.3² = 0.18 ずれる。床の摩擦で目減りする分を見込む。
    EXPECT_GT(MeanX(windy), MeanX(calm) + 0.05f);
    for (const auto& particle : windy.Particles()) EXPECT_EQ(particle.z, 0.0f);
}

// ── 液体の発生源の «広げ» ──

TEST(FluidOperatorEvalTest, EmitScaleWidensOnlyACrowdedSource)
{
    const asset::FluidLiquidSettings liquid;
    FluidSource roomy;
    roomy.shape = FluidSourceShape::Sphere;
    roomy.size = { 0.5f, 0.5f, 0.5f };
    roomy.velocity = { 0.0f, 0.0f, 0.0f };
    roomy.count = 20;
    roomy.duration = 0.0f;
    EXPECT_FLOAT_EQ(asset::FluidLiquidEmitScale(roomy, liquid, false), 1.0f);

    FluidSource crowded = roomy;
    crowded.size = { 0.02f, 0.02f, 0.02f };
    crowded.count = 600;
    const float scale = asset::FluidLiquidEmitScale(crowded, liquid, false);
    EXPECT_GT(scale, 1.0f);
    // 600 粒を静止密度の 2 倍まで詰めて置く面積は (600/2) × (2r)²。半径はその比の平方根だけ広がる。
    const float spacing = 2.0f * liquid.particleRadius;
    const float needed = 300.0f * spacing * spacing;
    const float area = 3.14159265f * crowded.size.x * crowded.size.x;
    EXPECT_NEAR(scale, std::sqrt(needed / area), 1.0e-2f);
}

TEST(FluidOperatorEvalTest, EmitScaleIsTheSizeTheSolverActuallyEmitsInto)
{
    // Inspector に出す «実際に湧く大きさ» がソルバーの中の広げ方とずれていないことを見る。
    asset::FluidRecipe recipe;
    recipe.kind = asset::FluidKind::Liquid;
    recipe.liquid.floor = false;
    recipe.liquid.gravity = 0.0f;
    FluidSource source;
    source.shape = FluidSourceShape::Sphere;
    source.center = { 0.0f, 0.0f, 0.0f };
    source.size = { 0.02f, 0.02f, 0.02f };
    source.velocity = { 0.0f, 0.0f, 0.0f };
    source.spread = 0.0f;
    source.count = 600;
    source.startTime = 0.0f;
    source.duration = 0.0f;
    recipe.sources.push_back(source);

    asset::FluidLiquidSolver solver;
    solver.Reset(recipe);
    solver.Advance(1.0f / 240.0f);
    ASSERT_EQ(solver.Particles().size(), 600u);

    const float fitted = source.size.x * asset::FluidLiquidEmitScale(source, recipe.liquid, false);
    float farthest = 0.0f;
    for (const auto& particle : solver.Particles())
        farthest = (std::max)(farthest, std::sqrt(particle.x * particle.x + particle.y * particle.y));
    // 600 粒を一様に撒けば縁まで届く。密度拘束の押し出し (1 反復で粒子半径の半分まで) の分を見込む。
    EXPECT_GT(farthest, fitted * 0.8f);
    EXPECT_LT(farthest, fitted + 4.0f * solver.ParticleRadius());
}

} // namespace fbzz::tests
