/// @file    ConstraintDebugGeometryTests.cpp
/// @brief   制約デバッグ表示が、種類ごとに «何を見せる線» を返すかを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-09
///
/// 制約は動いている間しか症状が出ないため、エディターのギズモが唯一の観測手段になる。
/// «線が出ない» と «制約が効いていない» が見分けられないと、デバッグの起点そのものが失われる。
/// ここでは線の本数と端点を固定して、ギズモが制約の実体を写していることを保証する。
#include <TestKit/TestKit.hpp>

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/ChainConstraint.hpp>
#include <Physics/ConstraintDebugGeometry.hpp>
#include <Physics/DistanceConstraint.hpp>
#include <Physics/FixedConstraint.hpp>
#include <Physics/HingeConstraint.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/RopeConstraint.hpp>
#include <Physics/SliderConstraint.hpp>
#include <Physics/SpringConstraint.hpp>

#include <vector>

namespace fbzz::tests {
namespace {

physics::RigidBody BodyAt(const math::Vector3& position)
{
    physics::RigidBody body;
    body.SetMass(1.0f);
    body.SetPosition(position);
    return body;
}

/// 端点の順序は実装都合なので、どちらの向きで積まれていても «その 2 点を結ぶ線» と認める。
bool ConnectsPoints(const physics::DebugLine& line,
                    const math::Vector3&      a,
                    const math::Vector3&      b)
{
    const bool forward = (line.from - a).LengthSq() < testkit::kEpsilon &&
                         (line.to   - b).LengthSq() < testkit::kEpsilon;
    const bool reverse = (line.from - b).LengthSq() < testkit::kEpsilon &&
                         (line.to   - a).LengthSq() < testkit::kEpsilon;
    return forward || reverse;
}

} // namespace

class ConstraintDebugGeometryTest : public testkit::Fixture {};

/// @name 2 体をつなぐだけの制約

TEST_F(ConstraintDebugGeometryTest, DrawsOneLineBetweenTheBodiesOfADistanceConstraint)
{
    physics::RigidBody a = BodyAt(math::Vector3::ZERO);
    physics::RigidBody b = BodyAt(math::Vector3(0.0f, 3.0f, 0.0f));
    physics::DistanceConstraint constraint(&a, &b, 3.0f);

    const physics::ConstraintDebugGeometry geometry =
        physics::BuildConstraintDebugGeometry(constraint);

    ASSERT_EQ(geometry.lines.size(), 1u);
    EXPECT_TRUE(ConnectsPoints(geometry.lines[0], a.GetPosition(), b.GetPosition()));
}

TEST_F(ConstraintDebugGeometryTest, DrawsOneLineForSpringRopeAndFixedConstraints)
{
    physics::RigidBody a = BodyAt(math::Vector3::ZERO);
    physics::RigidBody b = BodyAt(math::Vector3(1.0f, 0.0f, 0.0f));

    physics::SpringConstraint spring(&a, &b, 1.0f, 10.0f, 0.5f);
    physics::RopeConstraint   rope(&a, &b, 1.0f);
    physics::FixedConstraint  fixed(&a, &b);

    /// @note 見た目が同じ 3 種をまとめて見るのは、どれか 1 つが switch から漏れたときに
    ///       «この種類だけ線が出ない» を 1 回の失敗で拾うため。
    EXPECT_EQ(physics::BuildConstraintDebugGeometry(spring).lines.size(), 1u);
    EXPECT_EQ(physics::BuildConstraintDebugGeometry(rope).lines.size(), 1u);
    EXPECT_EQ(physics::BuildConstraintDebugGeometry(fixed).lines.size(), 1u);
}

TEST_F(ConstraintDebugGeometryTest, DrawsNothingWhenOneBodyIsMissing)
{
    physics::RigidBody a = BodyAt(math::Vector3::ZERO);
    physics::DistanceConstraint constraint(&a, nullptr, 1.0f);

    /// @note 片側が消えた制約で原点へ線を引くと、画面の中心へ «存在しない拘束» が生える。
    EXPECT_TRUE(physics::BuildConstraintDebugGeometry(constraint).lines.empty());
}

/// @name 鎖

TEST_F(ConstraintDebugGeometryTest, DrawsOneLinePerAdjacentPairOfAChain)
{
    physics::RigidBody a = BodyAt(math::Vector3::ZERO);
    physics::RigidBody b = BodyAt(math::Vector3(0.0f, -1.0f, 0.0f));
    physics::RigidBody c = BodyAt(math::Vector3(0.0f, -2.0f, 0.0f));
    physics::ChainConstraint chain(std::vector<physics::RigidBody*>{ &a, &b, &c }, 1.0f);

    const physics::ConstraintDebugGeometry geometry =
        physics::BuildConstraintDebugGeometry(chain);

    ASSERT_EQ(geometry.lines.size(), 2u);
    EXPECT_TRUE(ConnectsPoints(geometry.lines[0], a.GetPosition(), b.GetPosition()));
    EXPECT_TRUE(ConnectsPoints(geometry.lines[1], b.GetPosition(), c.GetPosition()));
}

TEST_F(ConstraintDebugGeometryTest, SkipsChainSegmentsWithAMissingBody)
{
    physics::RigidBody a = BodyAt(math::Vector3::ZERO);
    physics::RigidBody c = BodyAt(math::Vector3(0.0f, -2.0f, 0.0f));
    physics::ChainConstraint chain(std::vector<physics::RigidBody*>{ &a, nullptr, &c }, 1.0f);

    /// @note 途中の body が消えた鎖で «飛ばして» 線を引くと、繋がっていない 2 点が繋がって見える。
    EXPECT_TRUE(physics::BuildConstraintDebugGeometry(chain).lines.empty());
}

TEST_F(ConstraintDebugGeometryTest, DrawsNothingForAChainWithASingleBody)
{
    physics::RigidBody a = BodyAt(math::Vector3::ZERO);
    physics::ChainConstraint chain(std::vector<physics::RigidBody*>{ &a }, 1.0f);

    EXPECT_TRUE(physics::BuildConstraintDebugGeometry(chain).lines.empty());
}

/// @name ヒンジ

TEST_F(ConstraintDebugGeometryTest, DrawsTheAnchorGapAndTheAxisOfAHinge)
{
    physics::RigidBody a = BodyAt(math::Vector3::ZERO);
    physics::RigidBody b = BodyAt(math::Vector3(2.0f, 0.0f, 0.0f));
    physics::HingeConstraint hinge(&a, &b,
                                   math::Vector3(1.0f, 0.0f, 0.0f),
                                   math::Vector3(-1.0f, 0.0f, 0.0f),
                                   math::Vector3(0.0f, 4.0f, 0.0f));

    const physics::ConstraintDebugGeometry geometry =
        physics::BuildConstraintDebugGeometry(hinge);

    ASSERT_EQ(geometry.lines.size(), 2u);
    /// @note 1 本目はアンカーのずれ。ここが伸びていれば拘束が解けている、と目で読める。
    EXPECT_VEC3_NEAR(geometry.lines[0].from, math::Vector3(1.0f, 0.0f, 0.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(geometry.lines[0].to,   math::Vector3(1.0f, 0.0f, 0.0f), testkit::kTolerance);
    /// @note 2 本目は軸。長さは入力の大きさに依らず正規化された固定長で出す。
    EXPECT_VEC3_NEAR(geometry.lines[1].from, math::Vector3(1.0f, -0.35f, 0.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(geometry.lines[1].to,   math::Vector3(1.0f,  0.35f, 0.0f), testkit::kTolerance);
}

TEST_F(ConstraintDebugGeometryTest, DrawsNothingForAHingeWithAMissingBody)
{
    physics::RigidBody a = BodyAt(math::Vector3::ZERO);
    physics::HingeConstraint hinge(&a, nullptr,
                                   math::Vector3::ZERO,
                                   math::Vector3::ZERO,
                                   math::Vector3::UP);

    EXPECT_TRUE(physics::BuildConstraintDebugGeometry(hinge).lines.empty());
}

/// @name スライダー

TEST_F(ConstraintDebugGeometryTest, DrawsTheSlideAxisAndTheBodyLinkOfASlider)
{
    physics::RigidBody a = BodyAt(math::Vector3::ZERO);
    physics::RigidBody b = BodyAt(math::Vector3(0.0f, 0.0f, 3.0f));
    physics::SliderConstraint slider(&a, &b, math::Vector3(0.0f, 0.0f, 8.0f));

    const physics::ConstraintDebugGeometry geometry =
        physics::BuildConstraintDebugGeometry(slider);

    ASSERT_EQ(geometry.lines.size(), 2u);
    /// @note 動ける方向は body A を中心に固定長で描く。入力軸の大きさに引きずられない。
    EXPECT_VEC3_NEAR(geometry.lines[0].from, math::Vector3(0.0f, 0.0f, -0.5f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(geometry.lines[0].to,   math::Vector3(0.0f, 0.0f,  0.5f), testkit::kTolerance);
    EXPECT_TRUE(ConnectsPoints(geometry.lines[1], a.GetPosition(), b.GetPosition()));
}

} // namespace fbzz::tests
