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

/// Body はコピーできないので、まとめて作るときは所有権ごと持ち回る。
using BodySet = std::vector<std::unique_ptr<Body>>;

std::vector<physics::ColliderInstance> InstancesOf(BodySet& bodies)
{
    std::vector<physics::ColliderInstance> instances;
    instances.reserve(bodies.size());
    for (auto& body : bodies)
        instances.push_back(body->Instance());
    return instances;
}

/// «重なった 2 個» の組を、互いに遠く離して clusterCount 組ぶん撒く。
/// 葉のしきい値を超える個数を axis 方向へ広げ、BroadPhase の BVH を分割させる。
BodySet MakeSpreadClusters(int clusterCount, int axis)
{
    BodySet bodies;
    for (int i = 0; i < clusterCount; ++i) {
        math::Vector3 left  = math::Vector3::ZERO;
        math::Vector3 right = math::Vector3::ZERO;
        (&left.x)[axis]  = static_cast<float>(i) * 100.0f;
        (&right.x)[axis] = static_cast<float>(i) * 100.0f + 1.0f;
        bodies.push_back(std::make_unique<Body>(left));
        bodies.push_back(std::make_unique<Body>(right));
    }
    return bodies;
}

/// 全員が互いに重なる密集。分割後も «組の総数» が C(n,2) から動かないことを見る。
BodySet MakeCrowd(int count, float spacing)
{
    BodySet bodies;
    for (int i = 0; i < count; ++i)
        bodies.push_back(std::make_unique<Body>(
            math::Vector3(static_cast<float>(i) * spacing, 0.0f, 0.0f)));
    return bodies;
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

// --- BVH の分割 -------------------------------------------------------------
//
// 一時 BVH は葉に 4 個までしか入れず、超えると中央値で 2 つに割る。
// 4 個以下しか試していないと «割った後の走査» が一度も走らない。実シーンは常に
// こちら側なので、以下は «本番でしか通らない経路» を手前に引っ張り出すためのもの。

TEST_F(BroadPhaseTest, KeepsFindingEveryPairAfterTheTreeSplits)
{
    BodySet bodies = MakeSpreadClusters(5, 0);

    // 離れた 5 組は別々のノードへ落ちる。ノードをまたぐ走査が抜けていると 0 組になる。
    EXPECT_EQ(Sweep(InstancesOf(bodies)).size(), 5u);
}

TEST_F(BroadPhaseTest, SplitsAlongWhicheverAxisTheCrowdIsSpreadOn)
{
    BodySet alongY = MakeSpreadClusters(5, 1);
    BodySet alongZ = MakeSpreadClusters(5, 2);

    // 分割軸は «中心の広がりが一番大きい軸»。X 決め打ちだと、縦に積んだ床や
    // 奥行きに並んだ壁で木が痩せて総当たりに戻る。
    EXPECT_EQ(Sweep(InstancesOf(alongY)).size(), 5u);
    EXPECT_EQ(Sweep(InstancesOf(alongZ)).size(), 5u);
}

TEST_F(BroadPhaseTest, ReportsEachPairExactlyOnceEvenWhenTheTreeSplits)
{
    // 6 個すべてが互いに重なる。分割後は «左の葉の中» «左と右» «右の葉の中» の
    // 3 経路で走査されるので、境界の扱いを間違えると同じ組が二重に出る。
    BodySet crowd = MakeCrowd(6, 0.2f);

    EXPECT_EQ(Sweep(InstancesOf(crowd)).size(), 15u);
}

TEST_F(BroadPhaseTest, TerminatesWhenEveryColliderSharesTheSameCentre)
{
    // 中心が 1 点に潰れると分割軸の幅が 0 になる。«幅で切る» 作りだと片側が空になり、
    // 同じ集合で無限に再帰する。中央値で必ず半分に割ることをここで縛る。
    BodySet stacked = MakeCrowd(6, 0.0f);

    EXPECT_EQ(Sweep(InstancesOf(stacked)).size(), 15u);
}

} // namespace fbzz::tests
