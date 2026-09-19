/// @file    WorldCollisionEventTests.cpp
/// @brief   World が Step の末尾で作る衝突イベント (Enter / Stay / Exit と «ぶつかった強さ») を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-09
///
/// ゲーム側のダメージ・SE・ヒットストップは、すべてこのイベントを入口にする。Enter が
/// 毎フレーム飛べば «殴り続けている» ことになり、Exit が来なければ «踏み続けている» ことになる。
/// 衝突の強さ (approachSpeed / normalImpulse) は Resolve が速度を書き換える前後でしか取れず、
/// 取り逃がすと «全部同じ手応え» に潰れる。分類と強さの両方をここで固定する。
#include <TestKit/TestKit.hpp>

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/CollisionPair.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/World.hpp>

#include <initializer_list>

namespace fbzz::tests {
namespace {

/// 球 1 個ぶんの剛体と形状。World は非所有参照しか持たないので、寿命はテスト側が持つ。
class Ball {
public:
    Ball(float radius, const math::Vector3& position) : m_shape(radius)
    {
        m_body.SetMass(1.0f);
        m_body.SetPosition(position);
        /// @note Step を回す前のクエリ・BroadPhase は «コライダーが覚えている場所» を見る。
        m_shape.Update(position, math::Quaternion::Identity());
    }

    Ball(const Ball&)            = delete;
    Ball& operator=(const Ball&) = delete;

    Ball& AsStaticTrigger()
    {
        m_body.m_isStatic = true;
        m_body.SetMass(1.0f);
        m_isTrigger = true;
        return *this;
    }

    Ball& AsStaticWall()
    {
        m_body.m_isStatic = true;
        m_body.SetMass(1.0f);
        return *this;
    }

    physics::ColliderInstance Instance()
    {
        physics::ColliderInstance instance;
        instance.collider  = &m_shape;
        instance.body      = &m_body;
        instance.isTrigger = m_isTrigger;
        return instance;
    }

    physics::RigidBody&      Body()  { return m_body; }
    const physics::Collider* Shape() const { return &m_shape; }

private:
    physics::SphereCollider m_shape;
    physics::RigidBody      m_body;
    bool                    m_isTrigger = false;
};

} // namespace

class WorldCollisionEventTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        /// @note 落下は接触の «継続» を壊す。イベント分類だけを見たいので重力は切る。
        world.SetGravity(math::Vector3::ZERO);
    }

    /// 同期はテストごとに 1 回だけ行う。2 回目以降は «変更なし» になり、
    /// 静止したシーンでは Step が early-out して分類しか走らなくなる。
    void Sync(std::initializer_list<Ball*> balls)
    {
        world.BeginSceneSync();
        for (Ball* ball : balls) {
            world.SyncBody({}, &ball->Body());
            world.SyncCollider({}, ball->Instance());
        }
        world.EndSceneSync();
    }

    physics::World world;
};

/// @name Enter / Stay / Exit

TEST_F(WorldCollisionEventTest, ReportsEnterOnTheFirstFrameOfAnOverlap)
{
    Ball probe(1.0f, math::Vector3::ZERO);
    Ball zone(1.0f, math::Vector3(1.5f, 0.0f, 0.0f));
    zone.AsStaticTrigger();
    Sync({ &probe, &zone });

    world.Step(testkit::kFixedDeltaTime);

    ASSERT_EQ(world.GetEnterEvents().size(), 1u);
    EXPECT_TRUE(world.GetStayEvents().empty());
    EXPECT_TRUE(world.GetExitEvents().empty());
}

TEST_F(WorldCollisionEventTest, DowngradesEnterToStayWhileTheOverlapContinues)
{
    Ball probe(1.0f, math::Vector3::ZERO);
    Ball zone(1.0f, math::Vector3(1.5f, 0.0f, 0.0f));
    zone.AsStaticTrigger();
    Sync({ &probe, &zone });

    world.Step(testkit::kFixedDeltaTime);
    world.Step(testkit::kFixedDeltaTime);

    /// @note Enter が毎フレーム飛ぶと «触れた瞬間だけ» のはずの処理が連射される。
    EXPECT_TRUE(world.GetEnterEvents().empty());
    EXPECT_EQ(world.GetStayEvents().size(), 1u);
}

TEST_F(WorldCollisionEventTest, ReportsExitOnTheFrameTheOverlapEnds)
{
    Ball probe(1.0f, math::Vector3::ZERO);
    Ball zone(1.0f, math::Vector3(1.5f, 0.0f, 0.0f));
    zone.AsStaticTrigger();
    Sync({ &probe, &zone });

    world.Step(testkit::kFixedDeltaTime);
    zone.Body().SetPosition(math::Vector3(50.0f, 0.0f, 0.0f));
    world.Step(testkit::kFixedDeltaTime);

    ASSERT_EQ(world.GetExitEvents().size(), 1u);
    EXPECT_TRUE(world.GetStayEvents().empty());
}

TEST_F(WorldCollisionEventTest, ReportsExitOnlyOnceAfterTheOverlapEnds)
{
    Ball probe(1.0f, math::Vector3::ZERO);
    Ball zone(1.0f, math::Vector3(1.5f, 0.0f, 0.0f));
    zone.AsStaticTrigger();
    Sync({ &probe, &zone });

    world.Step(testkit::kFixedDeltaTime);
    zone.Body().SetPosition(math::Vector3(50.0f, 0.0f, 0.0f));
    world.Step(testkit::kFixedDeltaTime);
    world.Step(testkit::kFixedDeltaTime);

    /// @note 前フレームの記録を消し忘れると、離れて以降ずっと Exit が飛び続ける。
    EXPECT_TRUE(world.GetExitEvents().empty());
}

TEST_F(WorldCollisionEventTest, NamesBothCollidersOnTheEvent)
{
    Ball probe(1.0f, math::Vector3::ZERO);
    Ball zone(1.0f, math::Vector3(1.5f, 0.0f, 0.0f));
    zone.AsStaticTrigger();
    Sync({ &probe, &zone });

    world.Step(testkit::kFixedDeltaTime);

    ASSERT_EQ(world.GetEnterEvents().size(), 1u);
    const physics::CollisionEvent& event = world.GetEnterEvents()[0];
    /// @note どちらが A でどちらが B かは実装都合。«2 つとも載っている» ことだけを契約にする。
    const bool namesBoth =
        (event.colliderA == probe.Shape() && event.colliderB == zone.Shape()) ||
        (event.colliderA == zone.Shape() && event.colliderB == probe.Shape());
    EXPECT_TRUE(namesBoth);
    EXPECT_NE(event.bodyA, nullptr);
    EXPECT_NE(event.bodyB, nullptr);
}

/// @name Trigger

TEST_F(WorldCollisionEventTest, MarksAnOverlapWithATriggerAsATriggerEvent)
{
    Ball probe(1.0f, math::Vector3::ZERO);
    Ball zone(1.0f, math::Vector3(1.5f, 0.0f, 0.0f));
    zone.AsStaticTrigger();
    Sync({ &probe, &zone });

    world.Step(testkit::kFixedDeltaTime);

    ASSERT_EQ(world.GetEnterEvents().size(), 1u);
    EXPECT_TRUE(world.GetEnterEvents()[0].isTrigger);
}

TEST_F(WorldCollisionEventTest, LeavesASolidContactUnmarked)
{
    Ball probe(1.0f, math::Vector3::ZERO);
    Ball wall(1.0f, math::Vector3(1.5f, 0.0f, 0.0f));
    wall.AsStaticWall();
    Sync({ &probe, &wall });

    world.Step(testkit::kFixedDeltaTime);

    /// @note ここが true になると、押し返されているのに «すり抜ける当たり» として扱われる。
    ASSERT_EQ(world.GetEnterEvents().size(), 1u);
    EXPECT_FALSE(world.GetEnterEvents()[0].isTrigger);
}

TEST_F(WorldCollisionEventTest, DoesNotPushABodyOutOfATrigger)
{
    Ball probe(1.0f, math::Vector3::ZERO);
    Ball zone(1.0f, math::Vector3(1.5f, 0.0f, 0.0f));
    zone.AsStaticTrigger();
    Sync({ &probe, &zone });

    world.Step(testkit::kFixedDeltaTime);

    EXPECT_VEC3_NEAR(probe.Body().GetPosition(), math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_VEC3_NEAR(probe.Body().GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

/// @name 衝突の強さ

TEST_F(WorldCollisionEventTest, ReportsTheApproachSpeedMeasuredBeforeResolve)
{
    /// @note 接触した瞬間に «まだ減速していない» 速度を残す。Resolve の後で測ると、
    ///       止められた後の 0 に近い値になり、衝突ダメージが常に最小になる。
    Ball probe(1.0f, math::Vector3(-0.5f, 0.0f, 0.0f));
    Ball wall(1.0f, math::Vector3(1.5f, 0.0f, 0.0f));
    wall.AsStaticWall();
    probe.Body().SetVelocity(math::Vector3(5.0f, 0.0f, 0.0f));
    Sync({ &probe, &wall });

    world.Step(testkit::kFixedDeltaTime);

    ASSERT_EQ(world.GetEnterEvents().size(), 1u);
    EXPECT_NEAR(world.GetEnterEvents()[0].approachSpeed, 5.0f, testkit::kLooseTolerance);
}

TEST_F(WorldCollisionEventTest, ReportsTheNormalImpulseAppliedByResolve)
{
    Ball probe(1.0f, math::Vector3(-0.5f, 0.0f, 0.0f));
    Ball wall(1.0f, math::Vector3(1.5f, 0.0f, 0.0f));
    wall.AsStaticWall();
    probe.Body().SetVelocity(math::Vector3(5.0f, 0.0f, 0.0f));
    Sync({ &probe, &wall });

    world.Step(testkit::kFixedDeltaTime);

    /// @note 質量込みの «手応え»。軽い敵と重い敵で反応を変えたいときに読む値。
    ASSERT_EQ(world.GetEnterEvents().size(), 1u);
    EXPECT_GT(world.GetEnterEvents()[0].normalImpulse, 0.0f);
}

TEST_F(WorldCollisionEventTest, ReportsNoImpactForAnOverlapThatIsNotClosing)
{
    Ball probe(1.0f, math::Vector3::ZERO);
    Ball wall(1.0f, math::Vector3(1.5f, 0.0f, 0.0f));
    wall.AsStaticWall();
    Sync({ &probe, &wall });

    world.Step(testkit::kFixedDeltaTime);

    /// @note 床に載っているだけの接触に «衝突の強さ» を付けると、立っているだけで
    ///       ダメージが入り続ける。
    ASSERT_EQ(world.GetEnterEvents().size(), 1u);
    EXPECT_FLOAT_EQ(world.GetEnterEvents()[0].approachSpeed, 0.0f);
    EXPECT_FLOAT_EQ(world.GetEnterEvents()[0].normalImpulse, 0.0f);
}

TEST_F(WorldCollisionEventTest, ClearsTheImpactValuesOnExitEvents)
{
    Ball probe(1.0f, math::Vector3(-0.5f, 0.0f, 0.0f));
    Ball wall(1.0f, math::Vector3(1.5f, 0.0f, 0.0f));
    wall.AsStaticWall();
    probe.Body().SetVelocity(math::Vector3(5.0f, 0.0f, 0.0f));
    Sync({ &probe, &wall });

    world.Step(testkit::kFixedDeltaTime);
    ASSERT_GT(world.GetEnterEvents().size(), 0u);

    wall.Body().SetPosition(math::Vector3(50.0f, 0.0f, 0.0f));
    world.Step(testkit::kFixedDeltaTime);

    /// @note 離れた «瞬間» に強さは無い。前フレームの値を持ち越すと、Exit を見ている
    ///       スクリプトが古い衝突速度でダメージを出す。
    ASSERT_EQ(world.GetExitEvents().size(), 1u);
    const physics::CollisionEvent& exitEvent = world.GetExitEvents()[0];
    EXPECT_FLOAT_EQ(exitEvent.approachSpeed, 0.0f);
    EXPECT_FLOAT_EQ(exitEvent.normalImpulse, 0.0f);
    EXPECT_VEC3_NEAR(exitEvent.relativeVelocity, math::Vector3::ZERO, testkit::kTolerance);
}

} // namespace fbzz::tests
