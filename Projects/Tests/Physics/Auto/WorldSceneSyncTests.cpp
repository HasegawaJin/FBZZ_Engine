/// @file    WorldSceneSyncTests.cpp
/// @brief   World の毎フレーム同期 (BeginSceneSync 〜 EndSceneSync) が、ハンドルと寿命を正しく扱うことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-09
///
/// Scene 側は毎フレーム «今このシーンに居るもの» を World へ流し込み、流れてこなかったものは
/// 消えたとみなされる。この同期が緩むと、削除したはずの剛体が当たり判定に残る / 生きている
/// コンポーネントが毎フレーム新しい slot を掴んで pool が肥大化する、という形で壊れる。
/// どちらもフレームを跨いだ後にしか症状が出ないため、ハンドルの再利用規則をここで固定する。
#include <TestKit/TestKit.hpp>

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/BodyHandle.hpp>
#include <Physics/CollisionPair.hpp>
#include <Physics/DistanceConstraint.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/Volume.hpp>
#include <Physics/World.hpp>

#include <memory>
#include <utility>

namespace fbzz::tests {
namespace {

/// 生存数を外から数えられる Volume。World が所有権を持つため、破棄の観測は
/// ポインタではなくカウンタで行う (破棄後のポインタは触れない)。
class TrackingVolume final : public physics::Volume {
public:
    explicit TrackingVolume(int& aliveCount) : m_aliveCount(aliveCount) { ++m_aliveCount; }
    ~TrackingVolume() override { --m_aliveCount; }

    TrackingVolume(const TrackingVolume&)            = delete;
    TrackingVolume& operator=(const TrackingVolume&) = delete;

    bool Contains(const math::Vector3&) const override { return false; }
    void Apply(physics::RigidBody&, float) override {}

private:
    int& m_aliveCount;
};

physics::ColliderInstance Instance(physics::Collider& collider, physics::RigidBody* body = nullptr)
{
    physics::ColliderInstance instance;
    instance.collider = &collider;
    instance.body     = body;
    return instance;
}

} // namespace

class WorldSceneSyncTest : public testkit::Fixture {
protected:
    physics::World world;
};

// --- 剛体 -------------------------------------------------------------------

TEST_F(WorldSceneSyncTest, SyncBodyRejectsANullBody)
{
    world.BeginSceneSync();
    const physics::BodyHandle handle = world.SyncBody({}, nullptr);
    world.EndSceneSync();

    EXPECT_FALSE(handle.IsValid());
}

TEST_F(WorldSceneSyncTest, SyncBodyKeepsTheHandleAcrossFrames)
{
    physics::RigidBody body;

    world.BeginSceneSync();
    const physics::BodyHandle first = world.SyncBody({}, &body);
    world.EndSceneSync();

    world.BeginSceneSync();
    const physics::BodyHandle second = world.SyncBody(first, &body);
    world.EndSceneSync();

    ASSERT_TRUE(first.IsValid());
    EXPECT_EQ(second.slot, first.slot);
    EXPECT_EQ(second.generation, first.generation);
}

TEST_F(WorldSceneSyncTest, SyncBodyReattachesAKnownBodyWhenTheHandleIsLost)
{
    physics::RigidBody body;

    world.BeginSceneSync();
    const physics::BodyHandle first = world.SyncBody({}, &body);
    world.EndSceneSync();

    // Component のコピーや初期化順でハンドルが失われることがある。同じ RigidBody* が
    // すでに World に居るなら、新しい slot を切らずに元の slot へ繋ぎ直す。
    world.BeginSceneSync();
    const physics::BodyHandle rebound = world.SyncBody(physics::BodyHandle{ 99u, 99u }, &body);
    world.EndSceneSync();

    EXPECT_EQ(rebound.slot, first.slot);
}

TEST_F(WorldSceneSyncTest, EndSceneSyncReleasesTheSlotOfABodyThatStoppedSyncing)
{
    physics::RigidBody removed;
    physics::RigidBody added;

    world.BeginSceneSync();
    const physics::BodyHandle first = world.SyncBody({}, &removed);
    world.EndSceneSync();

    world.BeginSceneSync();   // removed を流さない = シーンから消えた
    world.EndSceneSync();

    world.BeginSceneSync();
    const physics::BodyHandle reused = world.SyncBody({}, &added);
    world.EndSceneSync();

    // slot は再利用するが世代を上げる。上げないと、消えた側の古いハンドルが
    // 新しい剛体を指してしまい «別のオブジェクトを操作する» ことになる。
    EXPECT_EQ(reused.slot, first.slot);
    EXPECT_NE(reused.generation, first.generation);
}

// --- コライダー -------------------------------------------------------------

TEST_F(WorldSceneSyncTest, SyncColliderRejectsAnInstanceWithoutAShape)
{
    world.BeginSceneSync();
    const physics::ColliderHandle handle = world.SyncCollider({}, physics::ColliderInstance{});
    world.EndSceneSync();

    EXPECT_FALSE(handle.IsValid());
}

TEST_F(WorldSceneSyncTest, SyncColliderKeepsTheHandleAcrossFrames)
{
    physics::SphereCollider sphere(1.0f);

    world.BeginSceneSync();
    const physics::ColliderHandle first = world.SyncCollider({}, Instance(sphere));
    world.EndSceneSync();

    world.BeginSceneSync();
    const physics::ColliderHandle second = world.SyncCollider(first, Instance(sphere));
    world.EndSceneSync();

    ASSERT_TRUE(first.IsValid());
    EXPECT_EQ(second.slot, first.slot);
    EXPECT_EQ(second.generation, first.generation);
}

TEST_F(WorldSceneSyncTest, SyncColliderReattachesAKnownShapeWhenTheHandleIsLost)
{
    physics::SphereCollider sphere(1.0f);

    world.BeginSceneSync();
    const physics::ColliderHandle first = world.SyncCollider({}, Instance(sphere));
    world.EndSceneSync();

    // 繋ぎ直さないと、毎フレーム新しい slot が積まれて同じ形状が二重に衝突判定へ載る。
    world.BeginSceneSync();
    const physics::ColliderHandle rebound =
        world.SyncCollider(physics::ColliderHandle{ 99u, 99u }, Instance(sphere));
    world.EndSceneSync();

    EXPECT_EQ(rebound.slot, first.slot);
}

TEST_F(WorldSceneSyncTest, EndSceneSyncReleasesTheSlotOfAColliderThatStoppedSyncing)
{
    physics::SphereCollider removed(1.0f);
    physics::SphereCollider added(1.0f);

    world.BeginSceneSync();
    const physics::ColliderHandle first = world.SyncCollider({}, Instance(removed));
    world.EndSceneSync();

    world.BeginSceneSync();
    world.EndSceneSync();

    world.BeginSceneSync();
    const physics::ColliderHandle reused = world.SyncCollider({}, Instance(added));
    world.EndSceneSync();

    EXPECT_EQ(reused.slot, first.slot);
    EXPECT_NE(reused.generation, first.generation);
}

TEST_F(WorldSceneSyncTest, SyncColliderPushesTheShapeInertiaIntoItsBody)
{
    physics::SphereCollider small(1.0f);
    physics::SphereCollider large(2.0f);
    physics::RigidBody smallBody;
    physics::RigidBody largeBody;
    smallBody.SetMass(1.0f);
    largeBody.SetMass(1.0f);

    world.BeginSceneSync();
    world.SyncCollider({}, Instance(small, &smallBody));
    world.SyncCollider({}, Instance(large, &largeBody));
    world.EndSceneSync();

    smallBody.ApplyAngularImpulse(math::Vector3::UP);
    largeBody.ApplyAngularImpulse(math::Vector3::UP);

    // 形状を渡さないと «質量だけの等方慣性» のままで、大きい球も小さい球も同じ勢いで回る。
    // 同期の時点で形状由来の慣性を入れ直すのが World の役目。
    EXPECT_GT(smallBody.GetAngularVelocity().y, largeBody.GetAngularVelocity().y);
}

// --- Volume -----------------------------------------------------------------

TEST_F(WorldSceneSyncTest, SyncVolumeRejectsANullVolume)
{
    world.BeginSceneSync();
    const physics::VolumeHandle handle = world.SyncVolume({}, nullptr);
    world.EndSceneSync();

    EXPECT_FALSE(handle.IsValid());
}

TEST_F(WorldSceneSyncTest, EndSceneSyncDestroysVolumesThatStoppedSyncing)
{
    int alive = 0;

    world.BeginSceneSync();
    world.SyncVolume({}, std::make_unique<TrackingVolume>(alive));
    world.EndSceneSync();
    ASSERT_EQ(alive, 1);

    world.BeginSceneSync();
    world.EndSceneSync();

    // Volume は PhysicsSystem が毎フレーム作り直す使い捨て。流れて来なくなった時点で
    // World が捨てないと、消えた VolumeComponent の効果が世界に残り続ける。
    EXPECT_EQ(alive, 0);
}

TEST_F(WorldSceneSyncTest, SyncVolumeReplacesTheInstanceInThatSlotWithoutLeaking)
{
    int alive = 0;

    world.BeginSceneSync();
    const physics::VolumeHandle first = world.SyncVolume({}, std::make_unique<TrackingVolume>(alive));
    world.EndSceneSync();

    world.BeginSceneSync();
    const physics::VolumeHandle second =
        world.SyncVolume(first, std::make_unique<TrackingVolume>(alive));
    world.EndSceneSync();

    EXPECT_EQ(second.slot, first.slot);
    EXPECT_EQ(second.generation, first.generation);
    // 毎フレーム作り直しても生存数は 1 のまま。増えるなら前フレームぶんが捨てられていない。
    EXPECT_EQ(alive, 1);
}

// --- 制約 -------------------------------------------------------------------

TEST_F(WorldSceneSyncTest, AddConstraintTakesOwnershipOfTheConstraint)
{
    physics::RigidBody a;
    physics::RigidBody b;

    world.AddConstraint(std::make_unique<physics::DistanceConstraint>(&a, &b, 1.0f));

    ASSERT_EQ(world.GetConstraints().size(), 1u);
    ASSERT_NE(world.GetConstraints()[0], nullptr);
    EXPECT_EQ(world.GetConstraints()[0]->GetType(), physics::ConstraintType::DISTANCE);
}

} // namespace fbzz::tests
