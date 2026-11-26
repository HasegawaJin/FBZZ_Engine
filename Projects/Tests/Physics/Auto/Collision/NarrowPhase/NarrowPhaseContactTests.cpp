/// @file    NarrowPhaseContactTests.cpp
/// @brief   NarrowPhase が返す接触点の «形» — 面接触のマニフォールドと、退化した配置での 1 点 — を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-09
#include <TestKit/TestKit.hpp>

#include <Math/Scalar/MathUtils.hpp>
#include <Math/LinearAlgebra/Quaternion.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Physics/Collision/Shapes/AABBCollider.hpp>
#include <Physics/Collision/Shapes/CapsuleCollider.hpp>
#include <Physics/Collision/BroadPhase/CollisionPair.hpp>
#include <Physics/Solver/ContactPoint.hpp>
#include <Physics/Collision/Shapes/CylinderCollider.hpp>
#include <Physics/Collision/NarrowPhase/PhysicsSolver.hpp>
#include <Physics/Collision/Shapes/HeightFieldCollider.hpp>
#include <Physics/Collision/Shapes/TriangleMeshCollider.hpp>
#include <Physics/Collision/Shapes/SphereCollider.hpp>

#include <vector>

namespace fbzz::tests {
namespace {

/// @note 剛体を持たない «形状だけ» の組を 1 ペア突き合わせる。
/// @note 剛体を付けない。NarrowPhase は最後に «bodyB → bodyA» へ法線を揃え直すため、
/// @note 剛体を付けるとここで見たい «各テスト関数が返した向き» が上書きされてしまう。
std::vector<physics::ContactPoint> Collide(physics::Collider& a, physics::Collider& b)
{
    physics::ColliderInstance instanceA;
    physics::ColliderInstance instanceB;
    instanceA.collider = &a;
    instanceB.collider = &b;

    const std::vector<physics::CollisionPair> pairs{ { &instanceA, &instanceB } };
    std::vector<physics::ContactPoint>        contacts;
    physics::PhysicsSolver                    solver;
    solver.NarrowPhase(pairs, contacts);
    return contacts;
}

physics::AABBCollider UnitBox(const math::Vector3& center)
{
    physics::AABBCollider box(math::Vector3(1.0f, 1.0f, 1.0f));
    box.Update(center, math::Quaternion::Identity());
    return box;
}

/// @note 4 点がすべて別の場所にあること。同じ点を 4 つ返すと «点接触を 4 回解く» だけになる。
bool AllPointsAreDistinct(const std::vector<physics::ContactPoint>& contacts)
{
    for (std::size_t i = 0; i < contacts.size(); ++i)
        for (std::size_t j = i + 1; j < contacts.size(); ++j)
            if ((contacts[i].point - contacts[j].point).LengthSq() < testkit::kEpsilon)
                return false;
    return true;
}

} /// @note namespace

class NarrowPhaseContactTest : public testkit::Fixture {};

/// @name 箱同士の面接触
/// @note 重なりが一番浅い軸を «押し戻す向き» に選び、その軸に垂直な重なり矩形の 4 隅を返す。
/// @note 3 軸それぞれを別テストにしているのは、軸の取り違えがテスト名で分かるようにするため。

/// @brief Builds a four point manifold on the X face of two boxes.
TEST_F(NarrowPhaseContactTest, BuildsAFourPointManifoldOnTheXFaceOfTwoBoxes)
{
    physics::AABBCollider a = UnitBox(math::Vector3::ZERO);
    physics::AABBCollider b = UnitBox(math::Vector3(1.5f, 0.0f, 0.0f));

    const std::vector<physics::ContactPoint> contacts = Collide(a, b);

    ASSERT_EQ(contacts.size(), 4u);
    EXPECT_TRUE(AllPointsAreDistinct(contacts));
    EXPECT_VEC3_NEAR(contacts[0].normal, -math::Vector3::RIGHT, testkit::kTolerance);
    EXPECT_NEAR(contacts[0].depth, 0.5f, testkit::kTolerance);
}

/// @brief Builds a four point manifold on the Y face of two boxes.
TEST_F(NarrowPhaseContactTest, BuildsAFourPointManifoldOnTheYFaceOfTwoBoxes)
{
    physics::AABBCollider a = UnitBox(math::Vector3::ZERO);
    physics::AABBCollider b = UnitBox(math::Vector3(0.0f, 1.5f, 0.0f));

    const std::vector<physics::ContactPoint> contacts = Collide(a, b);

    // 床の上の箱がこれ。1 点しか返らないと、置いた瞬間に必ず傾く。
    ASSERT_EQ(contacts.size(), 4u);
    EXPECT_TRUE(AllPointsAreDistinct(contacts));
    EXPECT_VEC3_NEAR(contacts[0].normal, -math::Vector3::UP, testkit::kTolerance);
    EXPECT_NEAR(contacts[0].depth, 0.5f, testkit::kTolerance);
}

/// @brief Builds a four point manifold on the Z face of two boxes.
TEST_F(NarrowPhaseContactTest, BuildsAFourPointManifoldOnTheZFaceOfTwoBoxes)
{
    physics::AABBCollider a = UnitBox(math::Vector3::ZERO);
    physics::AABBCollider b = UnitBox(math::Vector3(0.0f, 0.0f, 1.5f));

    const std::vector<physics::ContactPoint> contacts = Collide(a, b);

    ASSERT_EQ(contacts.size(), 4u);
    EXPECT_TRUE(AllPointsAreDistinct(contacts));
    EXPECT_VEC3_NEAR(contacts[0].normal, -math::Vector3::FORWARD, testkit::kTolerance);
    EXPECT_NEAR(contacts[0].depth, 0.5f, testkit::kTolerance);
}

/// @brief Shares the position correction across the manifold.
TEST_F(NarrowPhaseContactTest, SharesThePositionCorrectionAcrossTheManifold)
{
    physics::AABBCollider a = UnitBox(math::Vector3::ZERO);
    physics::AABBCollider b = UnitBox(math::Vector3(0.0f, 1.5f, 0.0f));

    const std::vector<physics::ContactPoint> contacts = Collide(a, b);

    // 4 点それぞれが «貫通を全部戻す» と、押し戻しが 4 倍になって弾かれる。
    ASSERT_EQ(contacts.size(), 4u);
    for (const physics::ContactPoint& contact : contacts)
        EXPECT_NEAR(contact.positionCorrectionWeight, 0.25f, testkit::kTolerance);
}

/// @brief Chooses the shallowest axis as the push direction.
TEST_F(NarrowPhaseContactTest, ChoosesTheShallowestAxisAsThePushDirection)
{
    physics::AABBCollider a = UnitBox(math::Vector3::ZERO);
    // X に 1.8、Y に 0.4 だけずらす。深く食い込んだ X ではなく浅い Y へ逃がす。
    physics::AABBCollider b = UnitBox(math::Vector3(0.2f, 1.6f, 0.0f));

    const std::vector<physics::ContactPoint> contacts = Collide(a, b);

    ASSERT_FALSE(contacts.empty());
    // 深い方へ押し出すと、角でぶつかった箱が横へ飛ぶ。
    EXPECT_VEC3_NEAR(contacts[0].normal, -math::Vector3::UP, testkit::kTolerance);
    EXPECT_NEAR(contacts[0].depth, 0.4f, testkit::kLooseTolerance);
}

/// @name 中心線が退化したカプセル
/// @note カプセルは «線分同士の最近点» で判定する。線分が点に潰れる (halfHeight = 0) 配置は
/// @note エディタで高さを 0 にすれば普通に作れるので、0 除算で落ちないことが契約になる。

/// @brief Collides two capsules whose axes are both degenerate.
TEST_F(NarrowPhaseContactTest, CollidesTwoCapsulesWhoseAxesAreBothDegenerate)
{
    physics::CapsuleCollider a(0.5f, 0.0f);
    physics::CapsuleCollider b(0.5f, 0.0f);
    a.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    b.Update(math::Vector3(0.5f, 0.0f, 0.0f), math::Quaternion::Identity());

    const std::vector<physics::ContactPoint> contacts = Collide(a, b);

    // 実質は球同士。両方の線分の長さが 0 でも最近点は決まる。
    ASSERT_EQ(contacts.size(), 1u);
    EXPECT_NEAR(contacts[0].depth, 0.5f, testkit::kLooseTolerance);
    EXPECT_UNIT_LENGTH(contacts[0].normal, testkit::kLooseTolerance);
}

/// @brief Collides a degenerate capsule against an upright one.
TEST_F(NarrowPhaseContactTest, CollidesADegenerateCapsuleAgainstAnUprightOne)
{
    physics::CapsuleCollider flat(0.5f, 0.0f);
    physics::CapsuleCollider upright(0.5f, 1.0f);
    flat.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    upright.Update(math::Vector3(0.6f, 0.0f, 0.0f), math::Quaternion::Identity());

    const std::vector<physics::ContactPoint> contacts = Collide(flat, upright);

    ASSERT_EQ(contacts.size(), 1u);
    EXPECT_UNIT_LENGTH(contacts[0].normal, testkit::kLooseTolerance);
}

/// @brief Collides an upright capsule against a degenerate one.
TEST_F(NarrowPhaseContactTest, CollidesAnUprightCapsuleAgainstADegenerateOne)
{
    physics::CapsuleCollider upright(0.5f, 1.0f);
    physics::CapsuleCollider flat(0.5f, 0.0f);
    upright.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    flat.Update(math::Vector3(0.6f, 0.0f, 0.0f), math::Quaternion::Identity());

    // 潰れている側が A か B かで処理が分かれる。両方向を通す。
    const std::vector<physics::ContactPoint> contacts = Collide(upright, flat);

    ASSERT_EQ(contacts.size(), 1u);
    EXPECT_UNIT_LENGTH(contacts[0].normal, testkit::kLooseTolerance);
}

/// @brief Clamps to the near end of a crossing capsule.
TEST_F(NarrowPhaseContactTest, ClampsToTheNearEndOfACrossingCapsule)
{
    physics::CapsuleCollider upright(0.5f, 1.0f);
    physics::CapsuleCollider lying(0.5f, 1.0f);
    upright.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    // 横倒しにして、最近点が線分の «端» に来る位置へ置く。
    lying.Update(math::Vector3(1.2f, 0.0f, 0.0f),
                 math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, math::HALF_PI));

    const std::vector<physics::ContactPoint> contacts = Collide(upright, lying);

    // 線分の内側だけを探すと、T 字に交わった配置で «届いていない» と判定して抜ける。
    ASSERT_EQ(contacts.size(), 1u);
    EXPECT_GT(contacts[0].depth, 0.0f);
}

/// @brief Clamps to the far end of a crossing capsule.
TEST_F(NarrowPhaseContactTest, ClampsToTheFarEndOfACrossingCapsule)
{
    physics::CapsuleCollider upright(0.5f, 1.0f);
    physics::CapsuleCollider lying(0.5f, 1.0f);
    upright.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    lying.Update(math::Vector3(-1.2f, 0.0f, 0.0f),
                 math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, math::HALF_PI));

    // 反対側へ置くと、クランプされる端が逆になる。両端とも拾えることを見る。
    const std::vector<physics::ContactPoint> contacts = Collide(upright, lying);

    ASSERT_EQ(contacts.size(), 1u);
    EXPECT_GT(contacts[0].depth, 0.0f);
}

/// @name 円柱の内部に入り込んだ球
/// @note 中心が中に入ると «球の中心から円柱表面へのベクトル» が 0 になり、押し出す向きが
/// @note 決まらなくなる。ここで向きを決め損ねると、めり込んだ物が振動するか固まる。

/// @brief Pushes a sphere inside a cylinder out the nearest side.
TEST_F(NarrowPhaseContactTest, PushesASphereInsideACylinderOutTheNearestSide)
{
    physics::SphereCollider sphere(0.5f);
    physics::CylinderCollider cylinder(2.0f, 2.0f);
    sphere.Update(math::Vector3(1.5f, 0.0f, 0.0f), math::Quaternion::Identity());
    cylinder.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    const std::vector<physics::ContactPoint> contacts = Collide(sphere, cylinder);

    // 側面まで 0.5、円板まで 2.0。浅い側面へ逃がす。
    ASSERT_EQ(contacts.size(), 1u);
    EXPECT_VEC3_NEAR(contacts[0].normal, math::Vector3::RIGHT, testkit::kTolerance);
    EXPECT_NEAR(contacts[0].depth, 1.0f, testkit::kTolerance);
}

/// @brief Pushes a sphere on the cylinder axis out through a cap.
TEST_F(NarrowPhaseContactTest, PushesASphereOnTheCylinderAxisOutThroughACap)
{
    physics::SphereCollider sphere(0.5f);
    physics::CylinderCollider cylinder(2.0f, 2.0f);
    sphere.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    cylinder.Update(math::Vector3::ZERO, math::Quaternion::Identity());

    const std::vector<physics::ContactPoint> contacts = Collide(sphere, cylinder);

    // 軸上では «外向きの半径方向» が定義できない。正規化に突っ込むと NaN が伝播するので、
    // 必ず円板側へ逃がす。
    ASSERT_EQ(contacts.size(), 1u);
    EXPECT_VEC3_NEAR(contacts[0].normal, math::Vector3::UP, testkit::kTolerance);
    EXPECT_NEAR(contacts[0].depth, 2.5f, testkit::kTolerance);
}

TEST_F(NarrowPhaseContactTest, CapsuleMergesMeshSeamButUsesDeepestHeightFieldContact)
{
    // 同じ平面を同じ対角線で二分し、カプセルの軸を対角線から少しずらして両面に接触させる。
    physics::TriangleMeshCollider mesh(
        {{0, 0, 0}, {2, 0, 0}, {0, 0, 2}, {2, 0, 2}}, {0, 2, 1, 1, 2, 3});
    physics::HeightFieldCollider heightField({0, 0, 0, 0}, 2, 2, 2.0f, 1.0f);
    physics::CapsuleCollider capsule(0.5f, 1.0f);
    const auto rotation = math::Quaternion::Identity();
    mesh.Update(math::Vector3::ZERO, rotation);
    heightField.Update(math::Vector3::ZERO, rotation);
    capsule.Update({1.06f, 1.3f, 1.0f}, rotation);

    const auto meshContacts = Collide(capsule, mesh);
    const auto fieldContacts = Collide(capsule, heightField);
    ASSERT_EQ(meshContacts.size(), 1u);
    ASSERT_EQ(fieldContacts.size(), 1u);
    EXPECT_VEC3_NEAR(fieldContacts[0].point, math::Vector3(1.06f, 0.0f, 1.0f), testkit::kTolerance);
    EXPECT_GT(meshContacts[0].point.x, 1.03f);
    EXPECT_LT(meshContacts[0].point.x, 1.06f);
    EXPECT_GT(meshContacts[0].normal.x, 0.01f);
    EXPECT_NEAR(meshContacts[0].depth, fieldContacts[0].depth, testkit::kTolerance);
}

} /// @note namespace fbzz::tests
