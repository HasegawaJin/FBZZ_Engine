/// @file    CollisionMatrixTests.cpp
/// @brief   NarrowPhase が全 8 形状の組み合わせを漏れなく捌き、法線の向きを一貫させることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// 形状の組は 8x8。dispatch は if/else の連鎖なので、«分岐の順序» ひとつで
/// ある組だけ静かに hit=false になる (実際 ConvexHull × HeightField がそうなっていた)。
/// 抜けた組は「そのオブジェクトだけ床をすり抜ける」という形でしか気づけないため、
/// 対応表そのものをテストの入力にして、追加・並べ替えのたびに全数を確かめる。
#include <TestKit/TestKit.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/CollisionPair.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/CylinderCollider.hpp>
#include <Physics/HeightFieldCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include <Physics/PhysicsSolver.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

/// 形状の一覧。ColliderType と 1 対 1 で、この列挙が «網羅すべき対応表» の軸になる。
enum class Shape { Sphere, Box, OrientedBox, Capsule, Cylinder, Hull, Mesh, Field };

constexpr Shape kAllShapes[] = {
    Shape::Sphere, Shape::Box, Shape::OrientedBox, Shape::Capsule,
    Shape::Cylinder, Shape::Hull, Shape::Mesh, Shape::Field,
};

/// 地形・静的メッシュ。動かない «面» として扱い、常に y=0 の水平面に置く。
constexpr bool IsSurface(Shape shape)
{
    return shape == Shape::Mesh || shape == Shape::Field;
}

const char* ShapeName(Shape shape)
{
    switch (shape) {
    case Shape::Sphere:      return "Sphere";
    case Shape::Box:         return "Box";
    case Shape::OrientedBox: return "OrientedBox";
    case Shape::Capsule:     return "Capsule";
    case Shape::Cylinder:    return "Cylinder";
    case Shape::Hull:        return "Hull";
    case Shape::Mesh:        return "Mesh";
    case Shape::Field:       return "Field";
    }
    return "Unknown";
}

std::vector<math::Vector3> UnitCubePoints()
{
    return {
        { -1.0f, -1.0f, -1.0f }, { 1.0f, -1.0f, -1.0f },
        { -1.0f,  1.0f, -1.0f }, { 1.0f,  1.0f, -1.0f },
        { -1.0f, -1.0f,  1.0f }, { 1.0f, -1.0f,  1.0f },
        { -1.0f,  1.0f,  1.0f }, { 1.0f,  1.0f,  1.0f },
    };
}

/// y=0 に広がる 8x8 の板。動く形状がどこに乗っても «面» に当たる大きさにする。
std::vector<math::Vector3> GroundQuadPositions()
{
    return {
        { -4.0f, 0.0f, -4.0f }, { 4.0f, 0.0f, -4.0f },
        { -4.0f, 0.0f,  4.0f }, { 4.0f, 0.0f,  4.0f },
    };
}

std::unique_ptr<physics::Collider> MakeCollider(Shape shape)
{
    switch (shape) {
    case Shape::Sphere:
        return std::make_unique<physics::SphereCollider>(1.0f);
    case Shape::Box:
        return std::make_unique<physics::AABBCollider>(math::Vector3(1.0f, 1.0f, 1.0f));
    case Shape::OrientedBox:
        return std::make_unique<physics::OBBCollider>(math::Vector3(1.0f, 1.0f, 1.0f));
    case Shape::Capsule:
        return std::make_unique<physics::CapsuleCollider>(0.5f, 1.0f);
    case Shape::Cylinder:
        return std::make_unique<physics::CylinderCollider>(1.0f, 1.0f);
    case Shape::Hull:
        return std::make_unique<physics::ConvexHullCollider>(UnitCubePoints());
    case Shape::Mesh:
        return std::make_unique<physics::TriangleMeshCollider>(
            GroundQuadPositions(), std::vector<uint32_t>{ 0, 2, 1, 1, 2, 3 });
    case Shape::Field:
        // 3x3 の平坦なグリッド。cellSize 4 なのでローカルでは [0,8]、
        // 原点を -4 ずらしてメッシュ版と同じ [-4,4] の板にする。
        return std::make_unique<physics::HeightFieldCollider>(
            std::vector<float>(9, 0.0f), 3, 3, 4.0f, 1.0f);
    }
    return nullptr;
}

/// 面の «形状としての» 原点。HeightField はローカル原点が隅にあるので中心へ寄せる。
math::Vector3 SurfaceOrigin(Shape shape)
{
    return shape == Shape::Field ? math::Vector3(-4.0f, 0.0f, -4.0f) : math::Vector3::ZERO;
}

/// コライダー・剛体・ColliderInstance を 1 組で持つ。
/// ColliderInstance が自分のメンバーを指すため、コピーも移動も禁止する。
class Actor {
public:
    Actor(Shape shape, const math::Vector3& position, bool withBody)
        : m_collider(MakeCollider(shape))
    {
        m_collider->Update(IsSurface(shape) ? SurfaceOrigin(shape) : position,
                           math::Quaternion::Identity());

        m_instance.collider = m_collider.get();
        m_instance.material = &physics::PhysicsMaterial::Default;
        if (withBody) {
            m_body.SetMass(1.0f);
            // 面の «重心» は板の中央。法線の向き合わせは剛体の位置関係で決まるため、
            // 形状のローカル原点ではなく見た目の中心を渡す。
            m_body.SetPosition(IsSurface(shape) ? math::Vector3::ZERO : position);
            m_instance.body = &m_body;
        }
    }

    /// 寸法や向きを指定したいとき用。表に載っていない配置を組むのに使う。
    Actor(std::unique_ptr<physics::Collider> collider,
          const math::Vector3&               position,
          const math::Quaternion&            rotation = math::Quaternion::Identity())
        : m_collider(std::move(collider))
    {
        m_collider->Update(position, rotation);
        m_body.SetMass(1.0f);
        m_body.SetPosition(position);
        m_body.SetRotation(rotation);
        m_instance.collider = m_collider.get();
        m_instance.body     = &m_body;
        m_instance.material = &physics::PhysicsMaterial::Default;
    }

    Actor(const Actor&)            = delete;
    Actor& operator=(const Actor&) = delete;

    const physics::ColliderInstance& Instance() const { return m_instance; }
    physics::ColliderInstance&       Instance() { return m_instance; }

private:
    std::unique_ptr<physics::Collider> m_collider;
    physics::RigidBody                 m_body;
    physics::ColliderInstance          m_instance;
};

struct Placement {
    math::Vector3 a;
    math::Vector3 b;
};

/// x 方向の «半分の厚み»。重なり量を形状の大きさに合わせるために使う。
float HalfWidthX(Shape shape)
{
    return shape == Shape::Capsule ? 0.5f : 1.0f;
}

/// 触れ合う距離の 8 割。厚みの 2 割だけ食い込ませる。
/// WHY 深く重ねないか: 片方の中心や軸が相手の内部に入ると «どちらの面から抜けるか» の
///     縮退した場合分けに入り、対応表が見たい «普通の接触» とは別の経路になる。
///     深い重なりは専用のテストで個別に見る。
float ShallowOffsetX(Shape a, Shape b)
{
    return (HalfWidthX(a) + HalfWidthX(b)) * 0.8f;
}

/// 面は常に原点の板。動かすのは «面でない側» だけで、面の中心の真上に浅く沈める。
/// 中心をぴったり面上に置くと上下の貫通量が並び、押し戻す向きが決められなくなる。
Placement Overlapping(Shape a, Shape b)
{
    if (IsSurface(b)) return { { 0.0f, 0.25f, 0.0f }, math::Vector3::ZERO };
    if (IsSurface(a)) return { math::Vector3::ZERO, { 0.0f, 0.25f, 0.0f } };
    return { math::Vector3::ZERO, { ShallowOffsetX(a, b), 0.0f, 0.0f } };
}

/// 離れた配置。BroadPhase を通さず NarrowPhase を直接叩くので、
/// 各判定関数が自力で «当たっていない» と答えられることを見る。
Placement Separated(Shape a, Shape b)
{
    if (IsSurface(b)) return { { 0.0f, 20.0f, 0.0f }, math::Vector3::ZERO };
    if (IsSurface(a)) return { math::Vector3::ZERO, { 0.0f, 20.0f, 0.0f } };
    return { math::Vector3::ZERO, { 20.0f, 0.0f, 0.0f } };
}

/// 重なりを解く向き。面なら鉛直、そうでなければずらした軸。
math::Vector3 SeparationAxis(Shape a, Shape b)
{
    return (IsSurface(a) || IsSurface(b)) ? math::Vector3::UP : math::Vector3::RIGHT;
}

std::vector<physics::ContactPoint> Collide(const Actor& a, const Actor& b)
{
    physics::PhysicsSolver             solver;
    std::vector<physics::CollisionPair> pairs{ { &a.Instance(), &b.Instance() } };
    std::vector<physics::ContactPoint>  contacts;
    solver.NarrowPhase(pairs, contacts);
    return contacts;
}

struct ShapePair {
    Shape a;
    Shape b;
};

/// 動く形状 6 種 + 面 2 種の全組み合わせから、両方が面の組 (どちらも静的で解く意味がない)
/// だけを除いた 33 組。ここが対応表そのものになる。
std::vector<ShapePair> CollidablePairs()
{
    std::vector<ShapePair> pairs;
    for (const Shape a : kAllShapes)
        for (const Shape b : kAllShapes) {
            if (IsSurface(a) && IsSurface(b)) continue;
            if (static_cast<int>(b) < static_cast<int>(a)) continue;   // 無順序の組を 1 度だけ
            pairs.push_back({ a, b });
        }
    return pairs;
}

std::string PairName(const ::testing::TestParamInfo<ShapePair>& info)
{
    return std::string(ShapeName(info.param.a)) + "Vs" + ShapeName(info.param.b);
}

} // namespace

class CollisionPairTest : public testkit::Fixture,
                          public ::testing::WithParamInterface<ShapePair> {};

// --- 当たる / 当たらない ----------------------------------------------------

TEST_P(CollisionPairTest, ReportsAContactWhenTheShapesOverlap)
{
    const ShapePair pair  = GetParam();
    const Placement place = Overlapping(pair.a, pair.b);
    const Actor a(pair.a, place.a, true);
    const Actor b(pair.b, place.b, true);

    const std::vector<physics::ContactPoint> contacts = Collide(a, b);

    EXPECT_FALSE(contacts.empty()) << "この形状の組が dispatch から漏れている";
}

TEST_P(CollisionPairTest, ReportsNoContactWhenTheShapesAreApart)
{
    const ShapePair pair  = GetParam();
    const Placement place = Separated(pair.a, pair.b);
    const Actor a(pair.a, place.a, true);
    const Actor b(pair.b, place.b, true);

    const std::vector<physics::ContactPoint> contacts = Collide(a, b);

    EXPECT_TRUE(contacts.empty()) << "離れているのに接触が生成された";
}

TEST_P(CollisionPairTest, ReportsAContactRegardlessOfTheOperandOrder)
{
    // dispatch は形状を «正規化された順» に並べ替えてから判定関数を呼ぶ。
    // 並べ替えの条件を書き間違えると、片方の順番でだけ当たらなくなる。
    const ShapePair pair  = GetParam();
    const Placement place = Overlapping(pair.a, pair.b);
    const Actor a(pair.a, place.a, true);
    const Actor b(pair.b, place.b, true);

    EXPECT_FALSE(Collide(a, b).empty());
    EXPECT_FALSE(Collide(b, a).empty());
}

// --- 接触点の中身 -----------------------------------------------------------

TEST_P(CollisionPairTest, ProducesAUnitNormalAndAPositiveDepth)
{
    const ShapePair pair  = GetParam();
    const Placement place = Overlapping(pair.a, pair.b);
    const Actor a(pair.a, place.a, true);
    const Actor b(pair.b, place.b, true);

    const std::vector<physics::ContactPoint> contacts = Collide(a, b);
    ASSERT_FALSE(contacts.empty());

    for (const physics::ContactPoint& cp : contacts) {
        EXPECT_UNIT_LENGTH(cp.normal, testkit::kLooseTolerance);
        EXPECT_GT(cp.depth, 0.0f);
    }
}

TEST_P(CollisionPairTest, OrientsTheNormalFromBodyBTowardsBodyA)
{
    // ContactPoint の規約: normal は «bodyB から bodyA へ押し戻す» 向き。
    // ソルバーはこの向き前提でインパルスを入れるので、裏返ると物体が吸い込まれる。
    const ShapePair pair  = GetParam();
    const Placement place = Overlapping(pair.a, pair.b);
    const Actor a(pair.a, place.a, true);
    const Actor b(pair.b, place.b, true);

    const std::vector<physics::ContactPoint> contacts = Collide(a, b);
    ASSERT_FALSE(contacts.empty());

    const math::Vector3 bToA = place.a - place.b;
    for (const physics::ContactPoint& cp : contacts)
        EXPECT_GT(math::Vector3::Dot(cp.normal, bToA), 0.0f);
}

TEST_P(CollisionPairTest, AlignsTheNormalWithTheAxisOfSeparation)
{
    // 貫通が一番浅い向きが法線になる。板の上なら鉛直、横にずらした組なら その軸。
    // 別の軸を向いていると、押し戻しが «横滑り» として出る。
    const ShapePair pair  = GetParam();
    const Placement place = Overlapping(pair.a, pair.b);
    const Actor a(pair.a, place.a, true);
    const Actor b(pair.b, place.b, true);

    const std::vector<physics::ContactPoint> contacts = Collide(a, b);
    ASSERT_FALSE(contacts.empty());

    const math::Vector3 axis = SeparationAxis(pair.a, pair.b);
    for (const physics::ContactPoint& cp : contacts)
        EXPECT_GT(math::Abs(math::Vector3::Dot(cp.normal, axis)), 0.7f);
}

TEST_P(CollisionPairTest, FlipsTheNormalWhenTheOperandsAreSwapped)
{
    // 剛体を付けない = 最後の «bodyDelta による向き合わせ» が働かない状態。
    // dispatch 内の «正規化して呼び、法線を反転する» 処理だけを裸で見る。
    const ShapePair pair  = GetParam();
    const Placement place = Overlapping(pair.a, pair.b);
    const Actor a(pair.a, place.a, false);
    const Actor b(pair.b, place.b, false);

    const std::vector<physics::ContactPoint> forward = Collide(a, b);
    const std::vector<physics::ContactPoint> reversed = Collide(b, a);
    ASSERT_FALSE(forward.empty());
    ASSERT_FALSE(reversed.empty());

    EXPECT_LT(math::Vector3::Dot(forward.front().normal, reversed.front().normal), -0.7f);
}

INSTANTIATE_TEST_SUITE_P(AllShapeCombinations, CollisionPairTest,
                         ::testing::ValuesIn(CollidablePairs()), PairName);

// --- 解かない組 -------------------------------------------------------------

class StaticPairTest : public testkit::Fixture {};

TEST_F(StaticPairTest, SkipsTrianglemeshAgainstTrianglemesh)
{
    // 両方とも静的な «面»。解いても何も動かないので判定ごと省く。
    const Actor a(Shape::Mesh, math::Vector3::ZERO, true);
    const Actor b(Shape::Mesh, math::Vector3::ZERO, true);

    EXPECT_TRUE(Collide(a, b).empty());
}

TEST_F(StaticPairTest, SkipsHeightfieldAgainstHeightfield)
{
    const Actor a(Shape::Field, math::Vector3::ZERO, true);
    const Actor b(Shape::Field, math::Vector3::ZERO, true);

    EXPECT_TRUE(Collide(a, b).empty());
}

TEST_F(StaticPairTest, SkipsTrianglemeshAgainstHeightfield)
{
    const Actor a(Shape::Mesh, math::Vector3::ZERO, true);
    const Actor b(Shape::Field, math::Vector3::ZERO, true);

    EXPECT_TRUE(Collide(a, b).empty());
    EXPECT_TRUE(Collide(b, a).empty());
}

// --- 接触点へ載る情報 -------------------------------------------------------

class ContactWiringTest : public testkit::Fixture {};

TEST_F(ContactWiringTest, CarriesTheCollidersAndBodiesOfThePair)
{
    // ここが取り違うと、衝突コールバックが «別のオブジェクトに当たった» と報告する。
    const Actor a(Shape::Sphere, math::Vector3::ZERO, true);
    const Actor b(Shape::Sphere, { 0.8f, 0.0f, 0.0f }, true);

    const std::vector<physics::ContactPoint> contacts = Collide(a, b);
    ASSERT_FALSE(contacts.empty());

    EXPECT_EQ(contacts.front().colliderA, a.Instance().collider);
    EXPECT_EQ(contacts.front().colliderB, b.Instance().collider);
    EXPECT_EQ(contacts.front().bodyA, a.Instance().body);
    EXPECT_EQ(contacts.front().bodyB, b.Instance().body);
    EXPECT_EQ(contacts.front().materialA, a.Instance().material);
    EXPECT_EQ(contacts.front().materialB, b.Instance().material);
}

TEST_F(ContactWiringTest, MarksTheContactAsTriggerWhenEitherSideIsATrigger)
{
    // トリガーは «通知するだけ»。片側だけの指定でも押し返してはいけない。
    Actor a(Shape::Sphere, math::Vector3::ZERO, true);
    const Actor b(Shape::Sphere, { 0.8f, 0.0f, 0.0f }, true);
    a.Instance().isTrigger = true;

    const std::vector<physics::ContactPoint> contacts = Collide(a, b);
    ASSERT_FALSE(contacts.empty());

    EXPECT_TRUE(contacts.front().isTrigger);
}

TEST_F(ContactWiringTest, LeavesTheContactNonTriggerWhenNeitherSideIsOne)
{
    const Actor a(Shape::Sphere, math::Vector3::ZERO, true);
    const Actor b(Shape::Sphere, { 0.8f, 0.0f, 0.0f }, true);

    const std::vector<physics::ContactPoint> contacts = Collide(a, b);
    ASSERT_FALSE(contacts.empty());

    EXPECT_FALSE(contacts.front().isTrigger);
}

TEST_F(ContactWiringTest, BuildsAMultiPointManifoldForFaceContacts)
{
    // 箱同士が面で当たったときに 1 点しか作らないと、接地した箱がその点を軸に揺れる。
    const Actor a(Shape::Box, math::Vector3::ZERO, true);
    const Actor b(Shape::Box, { 1.8f, 0.0f, 0.0f }, true);

    EXPECT_GT(Collide(a, b).size(), 1u);
}

TEST_F(ContactWiringTest, ReportsADeeperOverlapAsAGreaterDepth)
{
    const Actor a(Shape::Sphere, math::Vector3::ZERO, true);
    const Actor shallow(Shape::Sphere, { 1.5f, 0.0f, 0.0f }, true);
    const Actor deep(Shape::Sphere, { 0.5f, 0.0f, 0.0f }, true);

    const std::vector<physics::ContactPoint> shallowContacts = Collide(a, shallow);
    const std::vector<physics::ContactPoint> deepContacts    = Collide(a, deep);
    ASSERT_FALSE(shallowContacts.empty());
    ASSERT_FALSE(deepContacts.empty());

    EXPECT_GT(deepContacts.front().depth, shallowContacts.front().depth);
}

// --- 深く重なった配置 -------------------------------------------------------

class DeepOverlapTest : public testkit::Fixture {};

TEST_F(DeepOverlapTest, PushesASphereOutOfTheNearestFaceWhenItsCentreIsInsideABox)
{
    // 壁の中に生成された / 高速に貫通した球。最近点が中心と一致して法線が作れない縮退。
    // 抜ける先を決め打ちにすると、形状に関係なく真上へ飛び出す。
    const Actor sphere(Shape::Sphere, { 0.9f, 0.0f, 0.0f }, true);   // 中心が箱の内側
    const Actor box(Shape::Box, math::Vector3::ZERO, true);

    const std::vector<physics::ContactPoint> contacts = Collide(sphere, box);
    ASSERT_FALSE(contacts.empty());

    // +X 面までが 0.1、そこから半径 1.0 ぶん抜ける。
    EXPECT_VEC3_NEAR(contacts.front().normal, math::Vector3::RIGHT, testkit::kLooseTolerance);
    EXPECT_NEAR(contacts.front().depth, 1.1f, testkit::kLooseTolerance);
}

TEST_F(DeepOverlapTest, PushesACapsuleOutSidewaysWhenItsAxisSpansTheWholeBox)
{
    // カプセルの軸が箱を上下に貫いている。端点は上下面へ «接している» ので、
    // 最近点を 1 点だけ拾うと «そこから 0 距離で下へ抜ける» という答えになる。
    // 出口は線分全体で決めなければならない ─ 正しくは横 (+X 面) から 0.3 押し出す。
    const Actor box(Shape::Box, math::Vector3::ZERO, true);
    const Actor capsule(std::make_unique<physics::CapsuleCollider>(0.2f, 1.0f),
                        { 0.7f, 0.0f, 0.0f });

    const std::vector<physics::ContactPoint> contacts = Collide(box, capsule);
    ASSERT_FALSE(contacts.empty());

    EXPECT_VEC3_NEAR(contacts.front().normal, -math::Vector3::RIGHT, testkit::kLooseTolerance);
    EXPECT_NEAR(contacts.front().depth, 0.5f, testkit::kLooseTolerance);
}

TEST_F(DeepOverlapTest, PushesACapsuleOutAlongTheOrientedBoxAxes)
{
    // OBB 版も同じ規則。出口は箱のローカル軸で決まるので、回転を掛けても
    // «一番浅い面» の選び方は変わらない。
    const math::Quaternion turn =
        math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));
    const Actor box(std::make_unique<physics::OBBCollider>(math::Vector3(1.0f, 1.0f, 1.0f)),
                    math::Vector3::ZERO, turn);
    const Actor capsule(std::make_unique<physics::CapsuleCollider>(0.2f, 1.0f),
                        { 0.7f, 0.0f, 0.0f });

    const std::vector<physics::ContactPoint> contacts = Collide(box, capsule);
    ASSERT_FALSE(contacts.empty());

    EXPECT_VEC3_NEAR(contacts.front().normal, -math::Vector3::RIGHT, testkit::kLooseTolerance);
    EXPECT_NEAR(contacts.front().depth, 0.5f, testkit::kLooseTolerance);
}

TEST_F(DeepOverlapTest, PicksTheShallowestFaceForAnOffCentreSphereInsideABox)
{
    // 中心が上寄りなら上面が最短。抜ける面は «中心がどこに居るか» で毎回変わる。
    const Actor sphere(Shape::Sphere, { 0.0f, 0.2f, 0.0f }, true);
    const Actor box(Shape::Box, math::Vector3::ZERO, true);

    const std::vector<physics::ContactPoint> contacts = Collide(sphere, box);
    ASSERT_FALSE(contacts.empty());

    EXPECT_VEC3_NEAR(contacts.front().normal, math::Vector3::UP, testkit::kLooseTolerance);
    EXPECT_NEAR(contacts.front().depth, 1.8f, testkit::kLooseTolerance);
}

TEST_F(ContactWiringTest, PlacesTheContactPointBetweenTheTwoShapes)
{
    const Actor a(Shape::Sphere, math::Vector3::ZERO, true);
    const Actor b(Shape::Sphere, { 1.5f, 0.0f, 0.0f }, true);

    const std::vector<physics::ContactPoint> contacts = Collide(a, b);
    ASSERT_FALSE(contacts.empty());

    EXPECT_GT(contacts.front().point.x, 0.0f);
    EXPECT_LT(contacts.front().point.x, 1.5f);
}

} // namespace fbzz::tests
