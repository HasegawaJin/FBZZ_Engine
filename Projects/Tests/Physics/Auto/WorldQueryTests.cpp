/// @file    WorldQueryTests.cpp
/// @brief   World の空間クエリ (Raycast / RaycastAll / SphereCast / OverlapSphere) の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// エディターのピッキング、着地判定、索敵、当たり判定つきの当たり — ゲーム側から
/// «世界に何があるか» を尋ねる経路はすべてここを通る。最も近い 1 件を返す、距離順に返す、
/// フィルタで除外する、という約束が崩れると «たまに掴めない / 見えない» になる。
#include <TestKit/TestKit.hpp>

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/BodyHandle.hpp>
#include <Physics/CollisionPair.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/World.hpp>

#include <memory>
#include <vector>

namespace fbzz::tests {
namespace {

/// World へ登録する球 1 個。ColliderInstance がメンバーを指すのでコピーしない。
class Probe {
public:
    Probe(const math::Vector3& position, float radius)
        : m_collider(std::make_unique<physics::SphereCollider>(radius))
    {
        /// @note クエリはコライダーが覚えているワールド形状を見る。Step を回さないので手で同期する。
        m_collider->Update(position, math::Quaternion::Identity());
        m_body.SetMass(1.0f);
        m_body.SetPosition(position);
    }

    Probe(const Probe&)            = delete;
    Probe& operator=(const Probe&) = delete;

    physics::ColliderInstance Instance()
    {
        physics::ColliderInstance instance;
        instance.collider  = m_collider.get();
        instance.body      = &m_body;
        instance.isTrigger = m_trigger;
        instance.layer     = m_layer;
        return instance;
    }

    Probe& OnLayer(int layer) { m_layer = layer;     return *this; }
    Probe& AsTrigger()        { m_trigger = true;    return *this; }

    [[nodiscard]] const physics::Collider* Collider() const { return m_collider.get(); }

private:
    std::unique_ptr<physics::SphereCollider> m_collider;
    physics::RigidBody                       m_body;
    int  m_layer   = 0;
    bool m_trigger = false;
};

} // namespace

class WorldQueryTest : public testkit::Fixture {
protected:
    /// 登録は BeginSceneSync 〜 EndSceneSync で囲む。囲まないと «前フレームの残り» として掃除される。
    void Register(std::initializer_list<physics::ColliderInstance> colliders)
    {
        world.BeginSceneSync();
        for (const physics::ColliderInstance& instance : colliders)
            world.SyncCollider(physics::ColliderHandle{}, instance);
        world.EndSceneSync();
    }

    physics::World world;
};

/// @name Raycast

TEST_F(WorldQueryTest, RaycastHitsASphereOnItsPath)
{
    Probe target({ 0.0f, 0.0f, 10.0f }, 1.0f);
    Register({ target.Instance() });

    physics::World::RaycastHit hit;
    ASSERT_TRUE(world.Raycast(math::Vector3::ZERO, math::Vector3::FORWARD, 100.0f, hit));

    EXPECT_NEAR(hit.distance, 9.0f, testkit::kLooseTolerance);
    EXPECT_EQ(hit.collider, target.Collider());
}

TEST_F(WorldQueryTest, RaycastReportsThePointAndTheOutwardNormal)
{
    Probe target({ 0.0f, 0.0f, 10.0f }, 1.0f);
    Register({ target.Instance() });

    physics::World::RaycastHit hit;
    ASSERT_TRUE(world.Raycast(math::Vector3::ZERO, math::Vector3::FORWARD, 100.0f, hit));

    EXPECT_VEC3_NEAR(hit.point, math::Vector3(0.0f, 0.0f, 9.0f), testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(hit.normal, -math::Vector3::FORWARD, testkit::kLooseTolerance);
}

TEST_F(WorldQueryTest, RaycastMissesWhatIsBesideThePath)
{
    Probe target({ 10.0f, 0.0f, 10.0f }, 1.0f);
    Register({ target.Instance() });

    physics::World::RaycastHit hit;
    EXPECT_FALSE(world.Raycast(math::Vector3::ZERO, math::Vector3::FORWARD, 100.0f, hit));
}

TEST_F(WorldQueryTest, RaycastStopsAtTheMaximumDistance)
{
    Probe target({ 0.0f, 0.0f, 10.0f }, 1.0f);
    Register({ target.Instance() });

    physics::World::RaycastHit hit;
    EXPECT_FALSE(world.Raycast(math::Vector3::ZERO, math::Vector3::FORWARD, 5.0f, hit));
    EXPECT_TRUE(world.Raycast(math::Vector3::ZERO, math::Vector3::FORWARD, 50.0f, hit));
}

TEST_F(WorldQueryTest, RaycastReturnsTheNearestOfSeveralHits)
{
    /// @note 一番近いものを返さないと、壁の向こうの敵を掴めてしまう。
    Probe nearest({ 0.0f, 0.0f, 5.0f }, 1.0f);
    Probe farthest({ 0.0f, 0.0f, 20.0f }, 1.0f);
    /// @note 登録順は関係しない
    Register({ farthest.Instance(), nearest.Instance() });

    physics::World::RaycastHit hit;
    ASSERT_TRUE(world.Raycast(math::Vector3::ZERO, math::Vector3::FORWARD, 100.0f, hit));

    EXPECT_EQ(hit.collider, nearest.Collider());
}

TEST_F(WorldQueryTest, RaycastAcceptsAnUnnormalisedDirection)
{
    Probe target({ 0.0f, 0.0f, 10.0f }, 1.0f);
    Register({ target.Instance() });

    physics::World::RaycastHit hit;
    ASSERT_TRUE(world.Raycast(math::Vector3::ZERO, { 0.0f, 0.0f, 7.0f }, 100.0f, hit));

    /// @note 距離は正規化した向きで測る。長さを掛けたままだと maxDistance の意味が変わる。
    EXPECT_NEAR(hit.distance, 9.0f, testkit::kLooseTolerance);
}

TEST_F(WorldQueryTest, RaycastMissesWithADegenerateDirection)
{
    /// @note 「対象と重なっていて向きが決まらない」はスクリプトから普通に来る。
    Probe target({ 0.0f, 0.0f, 10.0f }, 1.0f);
    Register({ target.Instance() });

    physics::World::RaycastHit hit;
    EXPECT_FALSE(world.Raycast(math::Vector3::ZERO, math::Vector3::ZERO, 100.0f, hit));
}

TEST_F(WorldQueryTest, RaycastMissesInAnEmptyWorld)
{
    physics::World::RaycastHit hit;
    EXPECT_FALSE(world.Raycast(math::Vector3::ZERO, math::Vector3::FORWARD, 100.0f, hit));
}

TEST_F(WorldQueryTest, RaycastSkipsWhatTheFilterRejects)
{
    Probe ignored({ 0.0f, 0.0f, 5.0f }, 1.0f);
    Probe wanted({ 0.0f, 0.0f, 20.0f }, 1.0f);
    ignored.OnLayer(2);
    Register({ ignored.Instance(), wanted.Instance() });

    physics::World::RaycastHit hit;
    ASSERT_TRUE(world.Raycast(math::Vector3::ZERO, math::Vector3::FORWARD, 100.0f, hit,
                              [](const physics::ColliderInstance& i) { return i.layer != 2; }));

    EXPECT_EQ(hit.collider, wanted.Collider());
}

/// @name RaycastAll

TEST_F(WorldQueryTest, RaycastAllReturnsEveryHitInDistanceOrder)
{
    Probe first({ 0.0f, 0.0f, 5.0f }, 1.0f);
    Probe second({ 0.0f, 0.0f, 20.0f }, 1.0f);
    Probe third({ 0.0f, 0.0f, 35.0f }, 1.0f);
    Register({ third.Instance(), first.Instance(), second.Instance() });

    const std::vector<physics::World::RaycastHit> hits =
        world.RaycastAll(math::Vector3::ZERO, math::Vector3::FORWARD, 100.0f);

    ASSERT_EQ(hits.size(), 3u);
    EXPECT_EQ(hits[0].collider, first.Collider());
    EXPECT_EQ(hits[1].collider, second.Collider());
    EXPECT_EQ(hits[2].collider, third.Collider());
}

TEST_F(WorldQueryTest, RaycastAllIsEmptyWhenNothingIsOnThePath)
{
    Probe aside({ 10.0f, 0.0f, 10.0f }, 1.0f);
    Register({ aside.Instance() });

    EXPECT_TRUE(world.RaycastAll(math::Vector3::ZERO, math::Vector3::FORWARD, 100.0f).empty());
}

/// @name SphereCast

TEST_F(WorldQueryTest, SphereCastHitsWhatARayWouldMiss)
{
    /// @note 太さのある弾や «足元の接地» はこちら。半径ぶん外れていても当たる。
    Probe target({ 1.5f, 0.0f, 10.0f }, 1.0f);
    Register({ target.Instance() });

    physics::World::RaycastHit rayHit;
    physics::World::RaycastHit sweepHit;

    EXPECT_FALSE(world.Raycast(math::Vector3::ZERO, math::Vector3::FORWARD, 100.0f, rayHit));
    EXPECT_TRUE(world.SphereCast(math::Vector3::ZERO, 1.0f, math::Vector3::FORWARD, 100.0f,
                                 sweepHit));
}

TEST_F(WorldQueryTest, SphereCastStopsShortOfTheSurfaceByItsRadius)
{
    Probe target({ 0.0f, 0.0f, 10.0f }, 1.0f);
    Register({ target.Instance() });

    physics::World::RaycastHit hit;
    ASSERT_TRUE(world.SphereCast(math::Vector3::ZERO, 1.0f, math::Vector3::FORWARD, 100.0f, hit));

    /// @note 半径 1 の球が半径 1 の的に触れるのは中心間 2 = 進んだ距離 8。
    EXPECT_NEAR(hit.distance, 8.0f, testkit::kLooseTolerance);
}

TEST_F(WorldQueryTest, SphereCastSkipsWhatTheFilterRejects)
{
    Probe blocker({ 0.0f, 0.0f, 5.0f }, 1.0f);
    blocker.AsTrigger();
    Register({ blocker.Instance() });

    physics::World::RaycastHit hit;
    EXPECT_FALSE(world.SphereCast(math::Vector3::ZERO, 1.0f, math::Vector3::FORWARD, 100.0f, hit,
                                  [](const physics::ColliderInstance& i) { return !i.isTrigger; }));
}

/// @name OverlapSphere

TEST_F(WorldQueryTest, OverlapSphereFindsWhatTouchesTheQuery)
{
    Probe inside({ 1.0f, 0.0f, 0.0f }, 1.0f);
    Probe outside({ 50.0f, 0.0f, 0.0f }, 1.0f);
    Register({ inside.Instance(), outside.Instance() });

    const std::vector<const physics::ColliderInstance*> found =
        world.OverlapSphere(math::Vector3::ZERO, 2.0f);

    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found.front()->collider, inside.Collider());
}

TEST_F(WorldQueryTest, OverlapSphereIncludesShapesThatOnlyReachIn)
{
    /// @note 中心が範囲の外でも、半径で届いていれば «触れている»。索敵で取りこぼさないため。
    Probe reaching({ 4.0f, 0.0f, 0.0f }, 2.5f);
    Register({ reaching.Instance() });

    EXPECT_EQ(world.OverlapSphere(math::Vector3::ZERO, 2.0f).size(), 1u);
}

TEST_F(WorldQueryTest, OverlapSphereIsEmptyWhenNothingIsInRange)
{
    Probe outside({ 50.0f, 0.0f, 0.0f }, 1.0f);
    Register({ outside.Instance() });

    EXPECT_TRUE(world.OverlapSphere(math::Vector3::ZERO, 2.0f).empty());
}

TEST_F(WorldQueryTest, OverlapSphereSkipsWhatTheFilterRejects)
{
    Probe zone({ 1.0f, 0.0f, 0.0f }, 1.0f);
    zone.AsTrigger();
    Register({ zone.Instance() });

    const std::vector<const physics::ColliderInstance*> found = world.OverlapSphere(
        math::Vector3::ZERO, 2.0f,
        [](const physics::ColliderInstance& i) { return !i.isTrigger; });

    EXPECT_TRUE(found.empty());
}

} // namespace fbzz::tests
