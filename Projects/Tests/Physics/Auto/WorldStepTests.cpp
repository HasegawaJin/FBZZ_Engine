/// @file    WorldStepTests.cpp
/// @brief   World::Step の 1 フレーム分のパイプライン (力・Volume・制約・積分・Sleep・CCD) を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-09
///
/// Step は «誰が誰を何の順番で触るか» が全部入った 1 本の関数で、順番を 1 つ入れ替えるだけで
/// 見た目は動いているのに数値が変わる。重力が二重に掛かる、Volume の時間スケールが積分へ
/// 届かない、Sleep したはずの物体が動き続ける — どれも «全体としては動いている» ので
/// 目視では気づけない。段ごとの効果をここで 1 つずつ縛る。
///
/// 衝突イベントの分類は `WorldCollisionEventTests` が見る。ここでは接触を作らない。
#include <TestKit/TestKit.hpp>

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/ChainConstraint.hpp>
#include <Physics/CollisionPair.hpp>
#include <Physics/DistanceConstraint.hpp>
#include <Physics/HeightFieldCollider.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/SpringConstraint.hpp>
#include <Physics/Volume.hpp>
#include <Physics/World.hpp>

#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

/// 呼ばれ方を外から観測でき、効果を 1 つずつ切り替えられる Volume。
/// 所有権は World に渡すので、テスト側は生存中の raw ポインタだけを持つ。
class ScriptedVolume final : public physics::Volume {
public:
    bool          contains         = true;
    bool          overridesGravity = false;
    bool          expired          = false;
    float         timeScale        = 1.0f;
    math::Vector3 force            = math::Vector3::ZERO;

    int   applyCount = 0;
    int   tickCount  = 0;
    float lastDt     = 0.0f;

    bool Contains(const math::Vector3&) const override { return contains; }

    void Apply(physics::RigidBody& body, float dt) override
    {
        ++applyCount;
        lastDt = dt;
        body.ApplyForceNoWake(force);
    }

    float GetTimeScale()     const override { return timeScale; }
    bool  OverridesGravity() const override { return overridesGravity; }
    bool  IsExpired()        const override { return expired; }
    void  Tick(float) override { ++tickCount; }
};

/// 効果を «足していない» Volume。基底の既定実装だけで成り立つことを確かめるため、
/// Contains と Apply 以外は意図的に override しない。
class NeutralVolume final : public physics::Volume {
public:
    int applyCount = 0;

    bool Contains(const math::Vector3&) const override { return true; }
    void Apply(physics::RigidBody&, float) override { ++applyCount; }
};

/// 剛体と Volume を «同じ» 同期ブロックで流し込む。別々の Begin/EndSceneSync に分けると、
/// 後のブロックで流れて来なかった側が «シーンから消えた» とみなされて破棄される。
void SyncScene(physics::World&                            world,
               std::initializer_list<physics::RigidBody*> bodies,
               std::unique_ptr<physics::Volume>           volume = nullptr)
{
    world.BeginSceneSync();
    for (physics::RigidBody* body : bodies)
        world.SyncBody({}, body);
    if (volume)
        world.SyncVolume({}, std::move(volume));
    world.EndSceneSync();
}

} // namespace

class WorldStepTest : public testkit::Fixture {
protected:
    /// 重力以外の段を見たいテストが大半なので、既定は «無重力» から始める。
    /// 重力そのものを見るテストだけが SetGravity を呼ぶ。
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        world.SetGravity(math::Vector3::ZERO);
    }

    physics::World world;
};

// --- 設定値 -----------------------------------------------------------------

TEST_F(WorldStepTest, GravityIsReadBackAsSet)
{
    world.SetGravity(math::Vector3(0.0f, -3.0f, 0.0f));

    EXPECT_VEC3_NEAR(world.GetGravity(), math::Vector3(0.0f, -3.0f, 0.0f), testkit::kTolerance);
}

TEST_F(WorldStepTest, SubstepCountIsClampedToTheSupportedRange)
{
    // 0 以下で割ると dt が inf になり、1 フレームで世界が消し飛ぶ。上限は計算量の歯止め。
    world.SetSubsteps(0);
    EXPECT_EQ(world.GetSubsteps(), 1);

    world.SetSubsteps(1000);
    EXPECT_EQ(world.GetSubsteps(), 32);
}

// --- 重力 -------------------------------------------------------------------

TEST_F(WorldStepTest, AppliesWorldGravityAsAnAccelerationIndependentOfMass)
{
    physics::RigidBody body;
    body.SetMass(2.0f);
    world.SetGravity(math::Vector3(0.0f, -10.0f, 0.0f));
    SyncScene(world, { &body });

    world.Step(0.1f);

    // 力は質量倍で入れて積分で割り戻す。どこかで片方を忘れると重い物ほど遅く落ちる。
    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3(0.0f, -1.0f, 0.0f), testkit::kTolerance);
}

TEST_F(WorldStepTest, LeavesStaticBodiesUntouchedByGravity)
{
    physics::RigidBody body;
    body.m_isStatic = true;
    body.SetMass(1.0f);
    world.SetGravity(math::Vector3(0.0f, -10.0f, 0.0f));
    SyncScene(world, { &body });

    world.Step(0.1f);

    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(WorldStepTest, ScalesGravityPerBody)
{
    physics::RigidBody body;
    body.SetMass(1.0f);
    body.m_gravityScale = 0.25f;
    world.SetGravity(math::Vector3(0.0f, -10.0f, 0.0f));
    SyncScene(world, { &body });

    world.Step(0.1f);

    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3(0.0f, -0.25f, 0.0f), testkit::kTolerance);
}

TEST_F(WorldStepTest, SkipsGravityForABodyThatOptsOut)
{
    physics::RigidBody body;
    body.SetMass(1.0f);
    body.m_useGravity = false;
    world.SetGravity(math::Vector3(0.0f, -10.0f, 0.0f));
    SyncScene(world, { &body });

    world.Step(0.1f);

    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

// --- サブステップ -----------------------------------------------------------

TEST_F(WorldStepTest, SubstepsRefineThePathWithoutChangingTheEndingVelocity)
{
    physics::RigidBody single;
    physics::RigidBody split;
    single.SetMass(1.0f);
    split.SetMass(1.0f);

    physics::World refined;
    refined.SetGravity(math::Vector3(0.0f, -10.0f, 0.0f));
    refined.SetSubsteps(8);
    world.SetGravity(math::Vector3(0.0f, -10.0f, 0.0f));

    SyncScene(world, { &single });
    SyncScene(refined, { &split });

    world.Step(0.1f);
    refined.Step(0.1f);

    // 速度は «加速度 × 経過時間» なので刻み方に依らない。ここがずれるなら
    // subDt の割り方か、substep ごとの力のリセットが壊れている。
    EXPECT_VEC3_NEAR(split.GetVelocity(), single.GetVelocity(), testkit::kLooseTolerance);
    // 位置は半陰的オイラーの誤差ぶんだけ違う。細かく刻むほど落下量は小さくなる。
    EXPECT_GT(split.GetPosition().y, single.GetPosition().y);
}

// --- Sleep ------------------------------------------------------------------

TEST_F(WorldStepTest, PutsAMotionlessBodyToSleepAfterTheSettleTime)
{
    physics::RigidBody body;
    body.SetMass(1.0f);
    SyncScene(world, { &body });

    testkit::StepFixed([&](float dt) { world.Step(dt); }, 60);

    // 眠らせないと、静止したシーンでも毎フレーム全段が走り続ける。
    EXPECT_TRUE(body.IsSleeping());
}

TEST_F(WorldStepTest, KeepsAFallingBodyAwake)
{
    physics::RigidBody body;
    body.SetMass(1.0f);
    world.SetGravity(math::Vector3(0.0f, -10.0f, 0.0f));
    SyncScene(world, { &body });

    testkit::StepFixed([&](float dt) { world.Step(dt); }, 60);

    EXPECT_FALSE(body.IsSleeping());
}

TEST_F(WorldStepTest, SkipsTheWholePipelineForASettledScene)
{
    physics::RigidBody body;
    body.SetMass(1.0f);
    body.SetPosition(math::Vector3(0.0f, 5.0f, 0.0f));
    world.SetGravity(math::Vector3(0.0f, -10.0f, 0.0f));
    SyncScene(world, { &body });
    body.Sleep();

    // 追加も削除もない同期。これで «接触集合が変わり得る変更» が無いことが確定する。
    SyncScene(world, { &body });
    world.Step(0.1f);

    // 眠っているものしか居ないフレームでは何も動かない。ここが動くなら early-out の
    // 判定が緩く、Terrain のあるシーンで毎フレーム BVH クエリが走っている。
    EXPECT_VEC3_NEAR(body.GetPosition(), math::Vector3(0.0f, 5.0f, 0.0f), testkit::kTolerance);
    EXPECT_TRUE(body.IsSleeping());
}

// --- Volume -----------------------------------------------------------------

TEST_F(WorldStepTest, AppliesAVolumeToTheBodiesInsideIt)
{
    physics::RigidBody body;
    body.SetMass(1.0f);
    auto owned = std::make_unique<ScriptedVolume>();
    ScriptedVolume& volume = *owned;
    volume.force = math::Vector3(0.0f, 4.0f, 0.0f);
    SyncScene(world, { &body }, std::move(owned));

    world.Step(1.0f);

    EXPECT_EQ(volume.applyCount, 1);
    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3(0.0f, 4.0f, 0.0f), testkit::kTolerance);
}

TEST_F(WorldStepTest, LeavesBodiesOutsideAVolumeAlone)
{
    physics::RigidBody body;
    body.SetMass(1.0f);
    auto owned = std::make_unique<ScriptedVolume>();
    ScriptedVolume& volume = *owned;
    volume.contains = false;
    volume.force    = math::Vector3(0.0f, 100.0f, 0.0f);
    SyncScene(world, { &body }, std::move(owned));

    world.Step(1.0f);

    EXPECT_EQ(volume.applyCount, 0);
    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(WorldStepTest, DropsExpiredVolumesBeforeTheyAreApplied)
{
    physics::RigidBody body;
    body.SetMass(1.0f);
    auto owned = std::make_unique<ScriptedVolume>();
    ScriptedVolume& volume = *owned;
    volume.expired = true;
    volume.force   = math::Vector3(0.0f, 100.0f, 0.0f);
    SyncScene(world, { &body }, std::move(owned));

    world.Step(1.0f);

    // 寿命切れの爆風が «消したはずのフレーム» でもう一度効くと、1 回のはずの
    // ノックバックが 2 回入る。
    EXPECT_EQ(volume.applyCount, 0);
    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(WorldStepTest, AVolumeThatOverridesGravitySuppressesTheWorldGravity)
{
    physics::RigidBody body;
    body.SetMass(1.0f);
    world.SetGravity(math::Vector3(0.0f, -10.0f, 0.0f));
    auto owned = std::make_unique<ScriptedVolume>();
    ScriptedVolume& volume = *owned;
    volume.overridesGravity = true;
    SyncScene(world, { &body }, std::move(owned));

    world.Step(1.0f);

    // 上書きせずに «足し算» にすると、重力反転 Volume の中で下向きが残って浮かない。
    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(WorldStepTest, ATimeDilationVolumeShortensTheIntegrationStep)
{
    physics::RigidBody body;
    body.SetMass(1.0f);
    world.SetGravity(math::Vector3(0.0f, -10.0f, 0.0f));
    auto owned = std::make_unique<ScriptedVolume>();
    ScriptedVolume& volume = *owned;
    volume.timeScale = 0.5f;
    SyncScene(world, { &body }, std::move(owned));

    world.Step(1.0f);

    // 時間スケールは «力» ではなく «その body の dt» に効く。力側で割ると
    // 重力以外の外力 (すでに乗っている速度) が減速しない。
    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3(0.0f, -5.0f, 0.0f), testkit::kTolerance);
}

TEST_F(WorldStepTest, TicksVolumesOncePerSubstep)
{
    physics::RigidBody body;
    body.SetMass(1.0f);
    // 力を受けない body は 0.75 秒で眠り、眠った body には Volume が掛からなくなる。
    // ここで見たいのは «substep ごとに 1 回» なので、Sleep を混ぜない。
    body.m_allowSleeping = false;
    world.SetSubsteps(4);
    auto owned = std::make_unique<ScriptedVolume>();
    ScriptedVolume& volume = *owned;
    SyncScene(world, { &body }, std::move(owned));

    world.Step(1.0f);

    // Tick は Volume の寿命を進める。フレームごとに 1 回しか刻まないと、
    // substep を増やしたときだけ持続時間が伸びる。
    EXPECT_EQ(volume.tickCount, 4);
    EXPECT_EQ(volume.applyCount, 4);
}

TEST_F(WorldStepTest, StopsApplyingVolumesToABodyOnceItFallsAsleep)
{
    physics::RigidBody body;
    body.SetMass(1.0f);
    world.SetSubsteps(4);
    auto owned = std::make_unique<ScriptedVolume>();
    ScriptedVolume& volume = *owned;
    SyncScene(world, { &body }, std::move(owned));

    // subDt 0.25 × 4。3 回目の終わりに Sleep の閾値 (0.75 秒) へ届く。
    world.Step(1.0f);

    // 眠っている body へ環境力を掛け続けると、Sleep しても負荷が下がらない。
    // Tick は Volume 自身の寿命なので、body が眠っても止めない。
    EXPECT_TRUE(body.IsSleeping());
    EXPECT_EQ(volume.tickCount, 4);
    EXPECT_EQ(volume.applyCount, 3);
}

TEST_F(WorldStepTest, LeavesGravityAndTimeUntouchedForAVolumeThatAddsNoEffect)
{
    // 効果を 1 つだけ持つ Volume は、残りを基底の既定実装に任せて書く (WorldSceneSyncTests の
    // TrackingVolume が実際にそう書かれている)。既定が中立でなくなると «書いていない効果» が
    // 掛かることになり、書いていないぶん派生側を読んでも原因に辿り着けない。
    physics::RigidBody body;
    body.SetMass(1.0f);
    world.SetGravity(math::Vector3(0.0f, -10.0f, 0.0f));
    auto           owned  = std::make_unique<NeutralVolume>();
    NeutralVolume& volume = *owned;
    SyncScene(world, { &body }, std::move(owned));

    world.Step(1.0f);

    // 重力はそのまま (OverridesGravity → false)、時間も等倍 (GetTimeScale → 1)。
    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3(0.0f, -10.0f, 0.0f), testkit::kTolerance);
    // 寿命を持たない (IsExpired → false) ので、生成したフレームで捨てられない。
    EXPECT_EQ(volume.applyCount, 1);
}

// --- 制約 -------------------------------------------------------------------

TEST_F(WorldStepTest, SolvesConstraintPositionsAfterIntegrating)
{
    physics::RigidBody a;
    physics::RigidBody b;
    a.SetMass(1.0f);
    b.SetMass(1.0f);
    b.SetPosition(math::Vector3(3.0f, 0.0f, 0.0f));
    world.AddConstraint(std::make_unique<physics::DistanceConstraint>(&a, &b, 1.0f));
    SyncScene(world, { &a, &b });

    world.Step(testkit::kFixedDeltaTime);

    EXPECT_NEAR((b.GetPosition() - a.GetPosition()).Length(), 1.0f, testkit::kLooseTolerance);
}

TEST_F(WorldStepTest, AppliesConstraintForcesBeforeIntegrating)
{
    physics::RigidBody a;
    physics::RigidBody b;
    a.SetMass(1.0f);
    b.SetMass(1.0f);
    b.SetPosition(math::Vector3(3.0f, 0.0f, 0.0f));
    world.AddConstraint(std::make_unique<physics::SpringConstraint>(&a, &b, 1.0f, 10.0f, 0.0f));
    SyncScene(world, { &a, &b });

    world.Step(testkit::kFixedDeltaTime);

    // 積分の «後» に力を足すと、そのフレームの速度へ反映されず 1 フレーム遅れる。
    EXPECT_GT(a.GetVelocity().x, 0.0f);
    EXPECT_LT(b.GetVelocity().x, 0.0f);
}

TEST_F(WorldStepTest, SolvesAChainAsPositionsOnlyWithoutInjectingVelocity)
{
    // 鎖は «位置だけ» を直す制約で、力の段では何もしない。ここで速度が出るということは
    // ApplyForce と SolvePosition の両方から距離を詰めているということで、鎖が縮んだ
    // 反動で先端が弾き飛ぶ。段の分担そのものを縛る。
    physics::RigidBody a;
    physics::RigidBody b;
    physics::RigidBody c;
    a.SetMass(1.0f);
    b.SetMass(1.0f);
    c.SetMass(1.0f);
    b.SetPosition(math::Vector3(3.0f, 0.0f, 0.0f));
    c.SetPosition(math::Vector3(6.0f, 0.0f, 0.0f));
    world.AddConstraint(std::make_unique<physics::ChainConstraint>(
        std::vector<physics::RigidBody*>{ &a, &b, &c }, 1.0f, 32));
    SyncScene(world, { &a, &b, &c });

    world.Step(testkit::kFixedDeltaTime);

    EXPECT_NEAR((b.GetPosition() - a.GetPosition()).Length(), 1.0f, testkit::kLooseTolerance);
    EXPECT_NEAR((c.GetPosition() - b.GetPosition()).Length(), 1.0f, testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(a.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_VEC3_NEAR(c.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

// --- N 体重力 ---------------------------------------------------------------

TEST_F(WorldStepTest, PullsGravitationalSourcesTowardEachOther)
{
    physics::RigidBody a;
    physics::RigidBody b;
    a.SetMass(1.0f);
    b.SetMass(1.0f);
    b.SetPosition(math::Vector3(1.0f, 0.0f, 0.0f));
    for (physics::RigidBody* body : { &a, &b }) {
        body->m_isGravitationalSource = true;
        body->m_gravitationalMass     = 1.0e6f;
    }
    SyncScene(world, { &a, &b });

    world.Step(1.0f);

    // 作用・反作用。片方だけに入れると系全体が勝手に加速して飛んでいく。
    EXPECT_GT(a.GetVelocity().x, 0.0f);
    EXPECT_LT(b.GetVelocity().x, 0.0f);
    EXPECT_NEAR(a.GetVelocity().x + b.GetVelocity().x, 0.0f, testkit::kTolerance);
}

TEST_F(WorldStepTest, IgnoresBodiesThatAreNotGravitationalSources)
{
    physics::RigidBody source;
    physics::RigidBody plain;
    source.SetMass(1.0f);
    plain.SetMass(1.0f);
    plain.SetPosition(math::Vector3(1.0f, 0.0f, 0.0f));
    source.m_isGravitationalSource = true;
    source.m_gravitationalMass     = 1.0e6f;
    SyncScene(world, { &source, &plain });

    world.Step(1.0f);

    // 引力は «お互いが源であるとき» だけ。片側で成立させると、ただの小石が
    // 惑星に吸い寄せられる一方で反作用が消える。
    EXPECT_VEC3_NEAR(source.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_VEC3_NEAR(plain.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

// --- コライダー同期 ---------------------------------------------------------

TEST_F(WorldStepTest, MovesCollidersOntoTheirBodiesIncludingTheCenterOffset)
{
    physics::RigidBody body;
    body.SetMass(1.0f);
    body.SetPosition(math::Vector3(5.0f, 0.0f, 0.0f));
    physics::SphereCollider sphere(1.0f);

    physics::ColliderInstance instance;
    instance.collider     = &sphere;
    instance.body         = &body;
    instance.centerOffset = math::Vector3(0.0f, 2.0f, 0.0f);

    world.BeginSceneSync();
    world.SyncBody({}, &body);
    world.SyncCollider({}, instance);
    world.EndSceneSync();

    world.Step(testkit::kFixedDeltaTime);

    // オフセットを忘れると、見えている形状と当たり判定が «ちょうどオフセットぶん» ずれる。
    EXPECT_VEC3_NEAR(sphere.GetAABB().Center(),
                     math::Vector3(5.0f, 2.0f, 0.0f),
                     testkit::kLooseTolerance);
}

TEST_F(WorldStepTest, PairsABodyWithABodylessHeightField)
{
    // 地形は剛体を持たないコライダーとして World に載る。BroadPhase は AABB だけで
    // 候補を作るので、地形が AABB を返さない・剛体が無い側を飛ばす、のどちらでも
    // «NarrowPhase は正しいのに地形だけすり抜ける» になる。ペアが立つことを直接見る。
    physics::HeightFieldCollider field(std::vector<float>(25, 0.0f), 5, 5, 1.0f, 1.0f);
    field.Update(math::Vector3(-2.0f, 0.0f, -2.0f), math::Quaternion::Identity());

    physics::RigidBody body;
    body.SetMass(1.0f);
    body.SetPosition(math::Vector3(0.0f, 0.4f, 0.0f));   // 半径 0.5 なので地形へ食い込む
    physics::SphereCollider sphere(0.5f);

    physics::ColliderInstance ground;
    ground.collider = &field;

    physics::ColliderInstance falling;
    falling.collider = &sphere;
    falling.body     = &body;

    world.BeginSceneSync();
    world.SyncBody({}, &body);
    world.SyncCollider({}, ground);
    world.SyncCollider({}, falling);
    world.EndSceneSync();

    world.Step(testkit::kFixedDeltaTime);

    ASSERT_FALSE(world.GetEnterEvents().empty());
    const physics::CollisionEvent& event = world.GetEnterEvents().front();
    EXPECT_TRUE(event.colliderA == &field || event.colliderB == &field);
}

// --- CCD --------------------------------------------------------------------

TEST_F(WorldStepTest, ClampsTheVelocityOfAFastBodyToItsTimeOfImpact)
{
    physics::RigidBody fast;
    fast.SetMass(1.0f);
    fast.SetPosition(math::Vector3(-5.0f, 0.0f, 0.0f));
    fast.SetVelocity(math::Vector3(100.0f, 0.0f, 0.0f));
    fast.m_useCCD    = true;
    fast.m_ccdRadius = 0.5f;

    physics::RigidBody wall;
    wall.m_isStatic = true;
    wall.SetMass(1.0f);

    physics::SphereCollider fastShape(0.5f);
    physics::SphereCollider wallShape(0.5f);
    fastShape.Update(fast.GetPosition(), math::Quaternion::Identity());
    wallShape.Update(wall.GetPosition(), math::Quaternion::Identity());

    physics::ColliderInstance fastInstance;
    fastInstance.collider = &fastShape;
    fastInstance.body     = &fast;
    physics::ColliderInstance wallInstance;
    wallInstance.collider = &wallShape;
    wallInstance.body     = &wall;

    world.BeginSceneSync();
    world.SyncBody({}, &fast);
    world.SyncBody({}, &wall);
    world.SyncCollider({}, fastInstance);
    world.SyncCollider({}, wallInstance);
    world.EndSceneSync();

    world.Step(0.1f);

    // このフレームの移動量は 10、壁までは 4。TOI で速度を切らないと通り抜ける。
    EXPECT_NEAR(fast.GetPosition().x, -1.0f, testkit::kLooseTolerance);
}

TEST_F(WorldStepTest, TunnelsThroughTheWallWhenCcdIsDisabled)
{
    physics::RigidBody fast;
    fast.SetMass(1.0f);
    fast.SetPosition(math::Vector3(-5.0f, 0.0f, 0.0f));
    fast.SetVelocity(math::Vector3(100.0f, 0.0f, 0.0f));

    physics::RigidBody wall;
    wall.m_isStatic = true;
    wall.SetMass(1.0f);

    physics::SphereCollider fastShape(0.5f);
    physics::SphereCollider wallShape(0.5f);
    fastShape.Update(fast.GetPosition(), math::Quaternion::Identity());
    wallShape.Update(wall.GetPosition(), math::Quaternion::Identity());

    physics::ColliderInstance fastInstance;
    fastInstance.collider = &fastShape;
    fastInstance.body     = &fast;
    physics::ColliderInstance wallInstance;
    wallInstance.collider = &wallShape;
    wallInstance.body     = &wall;

    world.BeginSceneSync();
    world.SyncBody({}, &fast);
    world.SyncBody({}, &wall);
    world.SyncCollider({}, fastInstance);
    world.SyncCollider({}, wallInstance);
    world.EndSceneSync();

    world.Step(0.1f);

    // CCD は既定で切ってある (全物体に掛けると重い)。«切ると抜ける» ことを
    // 明示しておかないと、上のテストが何を守っているのか読めない。
    EXPECT_GT(fast.GetPosition().x, 0.0f);
}

} // namespace fbzz::tests
