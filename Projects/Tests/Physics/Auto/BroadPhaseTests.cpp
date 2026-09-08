/// @file    BroadPhaseTests.cpp
/// @brief   BroadPhase の候補ペア生成 — 重なり判定・重複排除・レイヤー・静止ペアの間引きを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// ここが多めにペアを出しても «少し重い» だけで絵は正しい。逆に落としすぎると
/// すり抜けになる。どちらも症状が薄いので、間引きの条件は表として固定しておく。
/// 同じ組を 2 回出すと、その接触だけインパルスが二重に入って弾みが変わる。
#include <TestKit/TestKit.hpp>

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/CollisionPair.hpp>
#include <Physics/PhysicsSolver.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/SphereCollider.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace fbzz::tests {
namespace {

/// BroadPhase は AABB しか見ないので、形状は球で足りる。
class Body {
public:
    explicit Body(const math::Vector3& position, float radius = 1.0f)
        : m_collider(std::make_unique<physics::SphereCollider>(radius))
    {
        m_collider->Update(position, math::Quaternion::Identity());
        m_body.SetMass(1.0f);
        m_body.SetPosition(position);
    }

    Body(const Body&)            = delete;
    Body& operator=(const Body&) = delete;

    physics::ColliderInstance Instance()
    {
        physics::ColliderInstance instance;
        instance.collider  = m_collider.get();
        instance.body      = m_hasBody ? &m_body : nullptr;
        instance.isTrigger = m_trigger;
        instance.layer     = m_layer;
        return instance;
    }

    Body& AsStatic()          { m_body.m_isStatic = true; return *this; }
    Body& AsSleeping()        { m_body.Sleep();           return *this; }
    Body& Awake()             { m_body.WakeUp();          return *this; }
    Body& AsTrigger()         { m_trigger = true;         return *this; }
    Body& WithoutBody()       { m_hasBody = false;        return *this; }
    Body& OnLayer(int layer)  { m_layer = layer;          return *this; }

private:
    std::unique_ptr<physics::SphereCollider> m_collider;
    physics::RigidBody                        m_body;
    bool m_hasBody = true;
    bool m_trigger = false;
    int  m_layer   = 0;
};

std::vector<physics::CollisionPair> Sweep(const std::vector<physics::ColliderInstance>& colliders,
                                          std::function<bool(int, int)> layerFilter = nullptr)
{
    physics::PhysicsSolver              solver;
    std::vector<physics::CollisionPair> pairs;
    solver.BroadPhase(colliders, pairs, layerFilter);
    return pairs;
}

} // namespace

class BroadPhaseTest : public testkit::Fixture {};

// --- 重なり -----------------------------------------------------------------

TEST_F(BroadPhaseTest, PairsUpTwoOverlappingColliders)
{
    Body a({ 0.0f, 0.0f, 0.0f });
    Body b({ 1.0f, 0.0f, 0.0f });

    EXPECT_EQ(Sweep({ a.Instance(), b.Instance() }).size(), 1u);
}

TEST_F(BroadPhaseTest, IgnoresCollidersWhoseBoundsDoNotTouch)
{
    Body a({ 0.0f, 0.0f, 0.0f });
    Body b({ 50.0f, 0.0f, 0.0f });

    EXPECT_TRUE(Sweep({ a.Instance(), b.Instance() }).empty());
}

TEST_F(BroadPhaseTest, ProducesNoPairsForASingleCollider)
{
    Body only({ 0.0f, 0.0f, 0.0f });

    EXPECT_TRUE(Sweep({ only.Instance() }).empty());
}

TEST_F(BroadPhaseTest, ProducesNoPairsForAnEmptyWorld)
{
    EXPECT_TRUE(Sweep({}).empty());
}

TEST_F(BroadPhaseTest, ReportsEachPairExactlyOnce)
{
    // 同じ組を 2 回出すと、その接触だけインパルスが二重に入る。
    // BVH の «自分自身との走査» と «兄弟ノード同士の走査» が重複しないことの確認。
    Body a({ 0.0f, 0.0f, 0.0f });
    Body b({ 0.5f, 0.0f, 0.0f });
    Body c({ 1.0f, 0.0f, 0.0f });

    EXPECT_EQ(Sweep({ a.Instance(), b.Instance(), c.Instance() }).size(), 3u);
}

TEST_F(BroadPhaseTest, FindsOnlyTheOverlappingSubsetInACrowd)
{
    Body a({ 0.0f, 0.0f, 0.0f });
    Body b({ 1.0f, 0.0f, 0.0f });
    Body far1({ 100.0f, 0.0f, 0.0f });
    Body far2({ 200.0f, 0.0f, 0.0f });

    EXPECT_EQ(Sweep({ a.Instance(), b.Instance(), far1.Instance(), far2.Instance() }).size(), 1u);
}

TEST_F(BroadPhaseTest, SkipsEntriesWithoutACollider)
{
    // コンポーネントを外した直後などに null が混ざる。走査ごと落とさないこと。
    Body a({ 0.0f, 0.0f, 0.0f });
    Body b({ 1.0f, 0.0f, 0.0f });
    physics::ColliderInstance empty;

    EXPECT_EQ(Sweep({ a.Instance(), empty, b.Instance() }).size(), 1u);
}

// --- レイヤー ---------------------------------------------------------------

TEST_F(BroadPhaseTest, DropsPairsRejectedByTheLayerFilter)
{
    Body a({ 0.0f, 0.0f, 0.0f });
    Body b({ 1.0f, 0.0f, 0.0f });
    b.OnLayer(3);

    const auto pairs = Sweep({ a.Instance(), b.Instance() },
                             [](int layerA, int layerB) { return layerA == layerB; });

    EXPECT_TRUE(pairs.empty());
}

TEST_F(BroadPhaseTest, KeepsPairsAcceptedByTheLayerFilter)
{
    Body a({ 0.0f, 0.0f, 0.0f });
    Body b({ 1.0f, 0.0f, 0.0f });
    b.OnLayer(3);

    const auto pairs = Sweep({ a.Instance(), b.Instance() },
                             [](int, int) { return true; });

    EXPECT_EQ(pairs.size(), 1u);
}

TEST_F(BroadPhaseTest, TreatsAMissingFilterAsCollideEverything)
{
    Body a({ 0.0f, 0.0f, 0.0f });
    Body b({ 1.0f, 0.0f, 0.0f });
    b.OnLayer(7);

    EXPECT_EQ(Sweep({ a.Instance(), b.Instance() }, nullptr).size(), 1u);
}

// --- 動かない組の間引き -----------------------------------------------------

TEST_F(BroadPhaseTest, SkipsTwoStaticColliders)
{
    // 解いても両方動かない。地形どうしの接触で BVH を毎 substep 叩くのを避ける。
    Body a({ 0.0f, 0.0f, 0.0f });
    Body b({ 1.0f, 0.0f, 0.0f });
    a.AsStatic();
    b.AsStatic();

    EXPECT_TRUE(Sweep({ a.Instance(), b.Instance() }).empty());
}

TEST_F(BroadPhaseTest, KeepsADynamicColliderAgainstAStaticOne)
{
    Body dynamicBody({ 0.0f, 0.0f, 0.0f });
    Body ground({ 1.0f, 0.0f, 0.0f });
    ground.AsStatic();

    EXPECT_EQ(Sweep({ dynamicBody.Instance(), ground.Instance() }).size(), 1u);
}

TEST_F(BroadPhaseTest, SkipsTwoSleepingColliders)
{
    Body a({ 0.0f, 0.0f, 0.0f });
    Body b({ 1.0f, 0.0f, 0.0f });
    a.AsSleeping();
    b.AsSleeping();

    EXPECT_TRUE(Sweep({ a.Instance(), b.Instance() }).empty());
}

TEST_F(BroadPhaseTest, SkipsASleepingColliderRestingOnStaticGeometry)
{
    // 眠った剛体が地形に載っているだけの状態。ここを毎 substep 判定すると
    // 何も起きないのに Terrain の BVH クエリだけが積み上がる。
    Body sleeper({ 0.0f, 0.0f, 0.0f });
    Body ground({ 1.0f, 0.0f, 0.0f });
    sleeper.AsSleeping();
    ground.AsStatic();

    EXPECT_TRUE(Sweep({ sleeper.Instance(), ground.Instance() }).empty());
}

TEST_F(BroadPhaseTest, WakingASleeperBringsThePairBack)
{
    // 間引きは «今眠っているか» で決まる。起こした次のステップから判定が戻ること。
    Body sleeper({ 0.0f, 0.0f, 0.0f });
    Body ground({ 1.0f, 0.0f, 0.0f });
    sleeper.AsSleeping();
    ground.AsStatic();

    sleeper.Awake();

    EXPECT_EQ(Sweep({ sleeper.Instance(), ground.Instance() }).size(), 1u);
}

TEST_F(BroadPhaseTest, SkipsColliderOnlyEntriesThatHaveNoBody)
{
    Body a({ 0.0f, 0.0f, 0.0f });
    Body b({ 1.0f, 0.0f, 0.0f });
    a.WithoutBody();
    b.WithoutBody();

    EXPECT_TRUE(Sweep({ a.Instance(), b.Instance() }).empty());
}

TEST_F(BroadPhaseTest, KeepsStaticPairsWhenEitherSideIsATrigger)
{
    // トリガーは «動かないけれど通知はしたい» 組。間引きの対象から外す。
    Body zone({ 0.0f, 0.0f, 0.0f });
    Body wall({ 1.0f, 0.0f, 0.0f });
    zone.AsStatic();
    zone.AsTrigger();
    wall.AsStatic();

    EXPECT_EQ(Sweep({ zone.Instance(), wall.Instance() }).size(), 1u);
}

TEST_F(BroadPhaseTest, AppliesTheLayerFilterEvenToTriggers)
{
    // トリガーは静止ペアの間引きだけを免除される。レイヤー行列より上位ではない。
    Body zone({ 0.0f, 0.0f, 0.0f });
    Body wall({ 1.0f, 0.0f, 0.0f });
    zone.AsTrigger();
    wall.OnLayer(3);

    const auto pairs = Sweep({ zone.Instance(), wall.Instance() },
                             [](int layerA, int layerB) { return layerA == layerB; });

    EXPECT_TRUE(pairs.empty());
}

} // namespace fbzz::tests
