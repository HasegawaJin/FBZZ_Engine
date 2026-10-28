/// @file    WorldSceneSyncTests.cpp
/// @brief   World の毎フレーム同期 (BeginSceneSync 〜 EndSceneSync) が、ハンドルと寿命を正しく扱うことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-09
/// @note Scene 側は毎フレーム «今このシーンに居るもの» を World へ流し込み、流れてこなかったものは
/// @note 消えたとみなされる。この同期が緩むと、削除したはずの剛体が当たり判定に残る / 生きている
/// @note コンポーネントが毎フレーム新しい slot を掴んで pool が肥大化する、という形で壊れる。
/// @note どちらもフレームを跨いだ後にしか症状が出ないため、ハンドルの再利用規則をここで固定する。
#include <TestKit/TestKit.hpp>
#include <array>
#include <limits>

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

/// @note 生存数を外から数えられる Volume。World が所有権を持つため、破棄の観測は
/// @note ポインタではなくカウンタで行う (破棄後のポインタは触れない)。
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

} /// @note namespace

class WorldSceneSyncTest : public testkit::Fixture {
protected:
    physics::World world;
};

/// @name 剛体

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

    /// @note Component のコピーや初期化順でハンドルが失われることがある。同じ RigidBody* が
    /// @note すでに World に居るなら、新しい slot を切らずに元の slot へ繋ぎ直す。
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

    /// @note removed を流さない = シーンから消えた
    world.BeginSceneSync();
    world.EndSceneSync();

    world.BeginSceneSync();
    const physics::BodyHandle reused = world.SyncBody({}, &added);
    world.EndSceneSync();

    /// @note slot は再利用するが世代を上げる。上げないと、消えた側の古いハンドルが
    /// @note 新しい剛体を指してしまい «別のオブジェクトを操作する» ことになる。
    EXPECT_EQ(reused.slot, first.slot);
    EXPECT_NE(reused.generation, first.generation);
}

/// @name コライダー

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

    /// @note 繋ぎ直さないと、毎フレーム新しい slot が積まれて同じ形状が二重に衝突判定へ載る。
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

    /// @note 形状を渡さないと «質量だけの等方慣性» のままで、大きい球も小さい球も同じ勢いで回る。
    /// @note 同期の時点で形状由来の慣性を入れ直すのが World の役目。
    EXPECT_GT(smallBody.GetAngularVelocity().y, largeBody.GetAngularVelocity().y);
}

TEST_F(WorldSceneSyncTest, MovingSyncedStaticTriggerEmitsExitWithoutAnActiveBody)
{
    physics::SphereCollider trigger(1.0f);
    physics::SphereCollider other(1.0f);
    trigger.Update(math::Vector3::ZERO, math::Quaternion::Identity());
    other.Update(math::Vector3{ 1.5f, 0.0f, 0.0f }, math::Quaternion::Identity());
    physics::ColliderInstance triggerInstance = Instance(trigger);
    triggerInstance.isTrigger = true;

    world.BeginSceneSync();
    const physics::ColliderHandle triggerHandle = world.SyncCollider({}, triggerInstance);
    const physics::ColliderHandle otherHandle = world.SyncCollider({}, Instance(other));
    world.EndSceneSync();
    world.Step(1.0f / 60.0f);
    ASSERT_EQ(world.GetEnterEvents().size(), 1u);
    EXPECT_TRUE(world.GetEnterEvents()[0].isTrigger);
    EXPECT_TRUE(world.GetExitEvents().empty());

    world.BeginSceneSync();
    world.SyncCollider(triggerHandle, triggerInstance);
    world.SyncCollider(otherHandle, Instance(other));
    world.EndSceneSync();
    world.Step(1.0f / 60.0f);
    EXPECT_TRUE(world.GetEnterEvents().empty());
    ASSERT_EQ(world.GetStayEvents().size(), 1u);
    EXPECT_TRUE(world.GetStayEvents()[0].isTrigger);

    /// @note 有効ハンドル更新と既知形状の再接続の両方で、body のない静的形状の移動を再評価する。
    for (int route = 0; route < 2; ++route) {
        const physics::ColliderHandle syncedTrigger = route == 0 ? triggerHandle : physics::ColliderHandle{ 99u, 99u };
        const physics::ColliderHandle syncedOther = route == 0 ? otherHandle : physics::ColliderHandle{ 99u, 99u };
        other.Update(math::Vector3{ 5.0f, 0.0f, 0.0f }, math::Quaternion::Identity());
        world.BeginSceneSync();
        world.SyncCollider(syncedTrigger, triggerInstance);
        world.SyncCollider(syncedOther, Instance(other));
        world.EndSceneSync();
        world.Step(1.0f / 60.0f);
        EXPECT_TRUE(world.GetEnterEvents().empty());
        EXPECT_TRUE(world.GetStayEvents().empty());
        ASSERT_EQ(world.GetExitEvents().size(), 1u);
        EXPECT_TRUE(world.GetExitEvents()[0].isTrigger);
        EXPECT_TRUE((world.GetExitEvents()[0].colliderA == &trigger && world.GetExitEvents()[0].colliderB == &other) ||
                    (world.GetExitEvents()[0].colliderA == &other && world.GetExitEvents()[0].colliderB == &trigger));

        world.BeginSceneSync();
        world.SyncCollider(syncedTrigger, triggerInstance);
        world.SyncCollider(syncedOther, Instance(other));
        world.EndSceneSync();
        world.Step(1.0f / 60.0f);
        EXPECT_TRUE(world.GetExitEvents().empty());

        if (route == 0) {
            other.Update(math::Vector3{ 1.5f, 0.0f, 0.0f }, math::Quaternion::Identity());
            world.BeginSceneSync();
            world.SyncCollider(triggerHandle, triggerInstance);
            world.SyncCollider(otherHandle, Instance(other));
            world.EndSceneSync();
            world.Step(1.0f / 60.0f);
            ASSERT_EQ(world.GetEnterEvents().size(), 1u);
        }
    }
}

/// @name Volume

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

    /// @note Volume は PhysicsSystem が毎フレーム作り直す使い捨て。流れて来なくなった時点で
    /// @note World が捨てないと、消えた VolumeComponent の効果が世界に残り続ける。
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
    /// @note 毎フレーム作り直しても生存数は 1 のまま。増えるなら前フレームぶんが捨てられていない。
    EXPECT_EQ(alive, 1);
}

/// @name 制約

TEST_F(WorldSceneSyncTest, AddConstraintTakesOwnershipOfTheConstraint)
{
    physics::RigidBody a;
    physics::RigidBody b;

    world.AddConstraint(std::make_unique<physics::DistanceConstraint>(&a, &b, 1.0f));

    ASSERT_EQ(world.GetConstraints().size(), 1u);
    ASSERT_NE(world.GetConstraints()[0], nullptr);
    EXPECT_EQ(world.GetConstraints()[0]->GetType(), physics::ConstraintType::DISTANCE);
}

TEST_F(WorldSceneSyncTest, AddConstraintSurvivesASceneSync)
{
    /// @note AddConstraint は «置いたら消えない» 側。同期の掃除に巻き込まれると、
    /// @note Physics 単体で組んだ拘束 (Tests / ベンチ) が最初のフレームで全部消える。
    physics::RigidBody a;
    physics::RigidBody b;
    world.AddConstraint(std::make_unique<physics::DistanceConstraint>(&a, &b, 1.0f));

    world.BeginSceneSync();
    world.EndSceneSync();

    EXPECT_EQ(world.GetConstraints().size(), 1u);
}

TEST_F(WorldSceneSyncTest, SyncConstraintReusesTheSameSlotEveryFrame)
{
    physics::RigidBody a;
    physics::RigidBody b;

    world.BeginSceneSync();
    const physics::ConstraintHandle first =
        world.SyncConstraint({}, std::make_unique<physics::DistanceConstraint>(&a, &b, 1.0f));
    world.EndSceneSync();
    ASSERT_TRUE(first.IsValid());

    /// @note 2 フレーム目は «そのまま生かす» 申告だけ。作り直さないので基準姿勢を抱える
    /// @note Fixed / Hinge が効き続ける。
    physics::Constraint* live = world.FindConstraint(first);
    world.BeginSceneSync();
    EXPECT_TRUE(world.KeepConstraint(first));
    world.EndSceneSync();

    ASSERT_EQ(world.GetConstraints().size(), 1u);
    EXPECT_EQ(world.FindConstraint(first), live);
}

TEST_F(WorldSceneSyncTest, AnUnclaimedSyncedConstraintIsDropped)
{
    physics::RigidBody a;
    physics::RigidBody b;

    world.BeginSceneSync();
    const physics::ConstraintHandle handle =
        world.SyncConstraint({}, std::make_unique<physics::DistanceConstraint>(&a, &b, 1.0f));
    world.EndSceneSync();
    ASSERT_EQ(world.GetConstraints().size(), 1u);

    /// @note 申告しないフレームを 1 回挟むだけで解放される。Scene 側に «外す» 経路は要らない。
    world.BeginSceneSync();
    world.EndSceneSync();

    EXPECT_TRUE(world.GetConstraints().empty());
    EXPECT_EQ(world.FindConstraint(handle), nullptr);
    /// @note 世代が進むので、古いハンドルで生かし直すことはできない。
    EXPECT_FALSE(world.KeepConstraint(handle));
}

TEST_F(WorldSceneSyncTest, RemoveConstraintFreesTheSlotForReuse)
{
    physics::RigidBody a;
    physics::RigidBody b;

    world.BeginSceneSync();
    const physics::ConstraintHandle first =
        world.SyncConstraint({}, std::make_unique<physics::DistanceConstraint>(&a, &b, 1.0f));
    world.RemoveConstraint(first);
    const physics::ConstraintHandle second =
        world.SyncConstraint({}, std::make_unique<physics::DistanceConstraint>(&a, &b, 2.0f));
    world.EndSceneSync();

    EXPECT_EQ(second.slot, first.slot);
    EXPECT_NE(second.generation, first.generation);
    /// @note 空いた枠を使い回す。フレームごとに増えるなら pool が肥大化している。
    EXPECT_EQ(world.GetConstraints().size(), 1u);
}

TEST_F(WorldSceneSyncTest, BroadPhaseDebugDisabledDoesNotReadAdditionalShapeBoundsOrPublish)
{
    struct CountedSphere final : physics::SphereCollider {
        CountedSphere() : SphereCollider(1.0f) {}
        mutable int reads = 0;
        physics::AABB GetAABB() const override { ++reads; return SphereCollider::GetAABB(); }
    };
    CountedSphere first;
    CountedSphere second;
    second.Update({4.0f, 0.0f, 0.0f}, math::Quaternion::Identity());
    auto a = Instance(first);
    auto b = Instance(second);
    a.isTrigger = b.isTrigger = true;
    world.BeginSceneSync();
    const auto firstHandle = world.SyncCollider({}, a);
    const auto secondHandle = world.SyncCollider({}, b);
    world.EndSceneSync();
    first.reads = second.reads = 0;
    world.Step(1.0f / 60.0f);
    const int disabledReads = first.reads + second.reads;
    EXPECT_EQ(world.GetBroadPhaseDebugSnapshot(), nullptr);

    physics::BroadPhaseDebugRequest request;
    request.enabled = request.bounds = request.candidatePairs = true;
    world.SetBroadPhaseDebugRequest(request);
    world.BeginSceneSync();
    world.SyncCollider(firstHandle, a);
    world.SyncCollider(secondHandle, b);
    world.EndSceneSync();
    first.reads = second.reads = 0;
    world.Step(1.0f / 60.0f);
    EXPECT_EQ(first.reads + second.reads, disabledReads);
    ASSERT_NE(world.GetBroadPhaseDebugSnapshot(), nullptr);
    request.enabled = false;
    world.SetBroadPhaseDebugRequest(request);
    EXPECT_EQ(world.GetBroadPhaseDebugSnapshot(), nullptr);
    world.Step(1.0f / 60.0f);
    EXPECT_EQ(world.GetBroadPhaseDebugSnapshot(), nullptr);
}

TEST_F(WorldSceneSyncTest, BroadPhaseDebugReportsActualGroupedHierarchyAndCapsAllContent)
{
    std::array<physics::SphereCollider, 12> shapes{
        physics::SphereCollider{1.0f}, physics::SphereCollider{1.0f}, physics::SphereCollider{1.0f},
        physics::SphereCollider{1.0f}, physics::SphereCollider{1.0f}, physics::SphereCollider{1.0f},
        physics::SphereCollider{1.0f}, physics::SphereCollider{1.0f}, physics::SphereCollider{1.0f},
        physics::SphereCollider{1.0f}, physics::SphereCollider{1.0f}, physics::SphereCollider{1.0f}};
    physics::BroadPhaseDebugRequest request;
    request.enabled = request.bounds = request.candidatePairs = true;
    request.maxDepth = 64;
    request.maxNodes = 2;
    request.maxProxies = 3;
    request.maxPairs = 1;
    world.SetBroadPhaseDebugRequest(request);
    world.BeginSceneSync();
    for (auto& shape : shapes) {
        auto instance = Instance(shape);
        instance.isTrigger = true;
        world.SyncCollider({}, instance);
    }
    world.EndSceneSync();
    world.Step(1.0f / 60.0f);
    const auto snapshot = world.GetBroadPhaseDebugSnapshot();
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->structure, physics::BroadPhaseDebugStructure::BVH);
    ASSERT_EQ(snapshot->nodes.size(), 2u);
    EXPECT_FALSE(snapshot->nodes.front().leaf);
    EXPECT_EQ(snapshot->nodes.front().proxyCount, shapes.size());
    EXPECT_EQ(snapshot->nodes.front().sourceIndex, 0);
    EXPECT_EQ(snapshot->nodes.front().parent, -1);
    EXPECT_GT(snapshot->totalNodes, snapshot->nodes.size());
    EXPECT_EQ(snapshot->totalProxies, shapes.size());
    EXPECT_EQ(snapshot->totalPairs, shapes.size() * (shapes.size() - 1) / 2);
    ASSERT_EQ(snapshot->proxies.size(), 3u);
    ASSERT_EQ(snapshot->pairs.size(), 1u);
    EXPECT_TRUE(snapshot->pairs.front().handleA.IsValid());
    EXPECT_TRUE(snapshot->pairs.front().handleB.IsValid());
    EXPECT_TRUE(snapshot->nodesTruncated);
    EXPECT_TRUE(snapshot->proxiesTruncated);
    EXPECT_TRUE(snapshot->pairsTruncated);
    EXPECT_TRUE(snapshot->broadPhaseRan);
    EXPECT_TRUE(snapshot->candidatePairsAvailable);
    EXPECT_FALSE(snapshot->traversalStatusAvailable);
    EXPECT_LE(snapshot->examinedNodes, physics::BROAD_PHASE_DEBUG_MAX_WORK);
}

TEST_F(WorldSceneSyncTest, BroadPhaseDebugSelectionUsesActualAncestorsAndHonorsDepthAndHardLimits)
{
    std::array<physics::SphereCollider, 6> shapes{
        physics::SphereCollider{1.0f}, physics::SphereCollider{1.0f}, physics::SphereCollider{1.0f},
        physics::SphereCollider{1.0f}, physics::SphereCollider{1.0f}, physics::SphereCollider{1.0f}};
    physics::BroadPhaseDebugRequest request;
    request.enabled = request.bounds = request.candidatePairs = request.selectedOnly = true;
    request.minDepth = request.maxDepth = 1;
    request.maxNodes = request.maxProxies = request.maxPairs = std::numeric_limits<std::size_t>::max();
    world.BeginSceneSync();
    for (auto& shape : shapes) {
        auto instance = Instance(shape);
        instance.isTrigger = true;
        request.selected = world.SyncCollider({}, instance);
    }
    world.EndSceneSync();
    request.selectedWorldEpoch = world.GetBroadPhaseDebugWorldEpoch();
    world.SetBroadPhaseDebugRequest(request);
    world.Step(1.0f / 60.0f);
    const auto snapshot = world.GetBroadPhaseDebugSnapshot();
    ASSERT_NE(snapshot, nullptr);
    EXPECT_TRUE(snapshot->selectionValid);
    EXPECT_EQ(snapshot->request.maxNodes, physics::BROAD_PHASE_DEBUG_MAX_ITEMS);
    EXPECT_EQ(snapshot->request.maxProxies, physics::BROAD_PHASE_DEBUG_MAX_ITEMS);
    EXPECT_EQ(snapshot->request.maxPairs, physics::BROAD_PHASE_DEBUG_MAX_ITEMS);
    ASSERT_EQ(snapshot->nodes.size(), 1u);
    EXPECT_TRUE(snapshot->nodes.front().leaf);
    EXPECT_EQ(snapshot->nodes.front().depth, 1u);
    EXPECT_EQ(snapshot->nodes.front().proxyCount, 3u);
    ASSERT_EQ(snapshot->proxies.size(), 1u);
    EXPECT_EQ(snapshot->proxies.front().handle.slot, request.selected.slot);
    EXPECT_EQ(snapshot->proxies.front().handle.generation, request.selected.generation);
    ASSERT_EQ(snapshot->pairs.size(), shapes.size() - 1);
    for (const auto& pair : snapshot->pairs)
        EXPECT_TRUE(pair.handleA.slot == request.selected.slot || pair.handleB.slot == request.selected.slot);
}

TEST_F(WorldSceneSyncTest, BroadPhaseDebugInvalidBoundsDescribeBruteForceWithoutFabricatingHierarchy)
{
    physics::SphereCollider invalid(1.0f);
    physics::SphereCollider valid(1.0f);
    invalid.m_radius = std::numeric_limits<float>::quiet_NaN();
    auto trigger = Instance(invalid);
    trigger.isTrigger = true;
    physics::BroadPhaseDebugRequest request;
    request.enabled = request.bounds = request.candidatePairs = true;
    world.SetBroadPhaseDebugRequest(request);
    world.BeginSceneSync();
    world.SyncCollider({}, trigger);
    world.SyncCollider({}, Instance(valid));
    world.EndSceneSync();
    world.Step(1.0f / 60.0f);
    const auto snapshot = world.GetBroadPhaseDebugSnapshot();
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->structure, physics::BroadPhaseDebugStructure::BRUTE_FORCE);
    EXPECT_EQ(snapshot->totalNodes, 0u);
    EXPECT_EQ(snapshot->totalProxies, 2u);
    EXPECT_TRUE(snapshot->nodes.empty());
    ASSERT_EQ(snapshot->proxies.size(), 1u);
    EXPECT_EQ(snapshot->proxies.front().handle.slot, 2u);
    EXPECT_TRUE(snapshot->proxiesTruncated);
    EXPECT_TRUE(snapshot->pairs.empty());
    EXPECT_TRUE(snapshot->candidatePairsAvailable);
}

TEST_F(WorldSceneSyncTest, BroadPhaseDebugZeroBudgetsKeepMetadataAndClearOnSceneSync)
{
    physics::SphereCollider first(1.0f);
    physics::SphereCollider second(1.0f);
    physics::BroadPhaseDebugRequest request;
    request.enabled = request.bounds = request.candidatePairs = true;
    request.maxNodes = request.maxProxies = request.maxPairs = 0;
    request.maxDepth = std::numeric_limits<uint32_t>::max();
    world.SetBroadPhaseDebugRequest(request);
    auto trigger = Instance(first);
    trigger.isTrigger = true;
    world.BeginSceneSync();
    world.SyncCollider({}, trigger);
    world.SyncCollider({}, Instance(second));
    world.EndSceneSync();
    world.Step(1.0f / 60.0f);
    const auto snapshot = world.GetBroadPhaseDebugSnapshot();
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->request.maxDepth, physics::BROAD_PHASE_DEBUG_MAX_DEPTH);
    EXPECT_EQ(snapshot->totalNodes, 1u);
    EXPECT_EQ(snapshot->totalPairs, 1u);
    EXPECT_TRUE(snapshot->nodes.empty());
    EXPECT_TRUE(snapshot->proxies.empty());
    EXPECT_TRUE(snapshot->pairs.empty());
    EXPECT_EQ(snapshot->examinedNodes, 0u);
    EXPECT_EQ(snapshot->examinedProxies, 0u);
    EXPECT_EQ(snapshot->examinedPairs, 0u);
    world.BeginSceneSync();
    EXPECT_EQ(world.GetBroadPhaseDebugSnapshot(), nullptr);
    world.EndSceneSync();
    EXPECT_EQ(snapshot->totalPairs, 1u);
}

TEST_F(WorldSceneSyncTest, BroadPhaseDebugRejectsReusedGenerationAndRetainsOnlyValues)
{
    auto removed = std::make_unique<physics::SphereCollider>(1.0f);
    physics::BroadPhaseDebugRequest request;
    request.enabled = request.bounds = true;
    world.SetBroadPhaseDebugRequest(request);
    world.BeginSceneSync();
    const auto oldHandle = world.SyncCollider({}, Instance(*removed));
    world.EndSceneSync();
    world.Step(1.0f / 60.0f);
    const auto retained = world.GetBroadPhaseDebugSnapshot();
    ASSERT_NE(retained, nullptr);
    ASSERT_EQ(retained->proxies.size(), 1u);
    world.BeginSceneSync();
    world.EndSceneSync();
    removed.reset();

    physics::SphereCollider replacement(2.0f);
    request.selectedOnly = true;
    request.selected = oldHandle;
    request.selectedWorldEpoch = retained->worldEpoch;
    world.SetBroadPhaseDebugRequest(request);
    world.BeginSceneSync();
    const auto replacementHandle = world.SyncCollider({}, Instance(replacement));
    world.EndSceneSync();
    world.Step(1.0f / 60.0f);
    EXPECT_EQ(replacementHandle.slot, oldHandle.slot);
    EXPECT_NE(replacementHandle.generation, oldHandle.generation);
    const auto current = world.GetBroadPhaseDebugSnapshot();
    ASSERT_NE(current, nullptr);
    EXPECT_FALSE(current->selectionValid);
    EXPECT_TRUE(current->nodes.empty());
    EXPECT_TRUE(current->proxies.empty());
    EXPECT_EQ(retained->proxies.front().handle.generation, oldHandle.generation);
    EXPECT_FLOAT_EQ(retained->proxies.front().bounds.max.x, 1.0f);
}

TEST_F(WorldSceneSyncTest, BroadPhaseDebugWorldResetRejectsTheSameSlotAndGenerationFromOldWorld)
{
    physics::SphereCollider sphere(1.0f);
    physics::BroadPhaseDebugRequest request;
    request.enabled = request.bounds = true;
    world.SetBroadPhaseDebugRequest(request);
    world.BeginSceneSync();
    const auto oldHandle = world.SyncCollider({}, Instance(sphere));
    world.EndSceneSync();
    world.Step(1.0f / 60.0f);
    const auto retained = world.GetBroadPhaseDebugSnapshot();
    ASSERT_NE(retained, nullptr);
    world = physics::World{};
    EXPECT_NE(world.GetBroadPhaseDebugWorldEpoch(), retained->worldEpoch);
    EXPECT_EQ(world.GetBroadPhaseDebugSnapshot(), nullptr);
    request.selectedOnly = true;
    request.selected = oldHandle;
    request.selectedWorldEpoch = retained->worldEpoch;
    world.SetBroadPhaseDebugRequest(request);
    world.BeginSceneSync();
    const auto newHandle = world.SyncCollider({}, Instance(sphere));
    world.EndSceneSync();
    world.Step(1.0f / 60.0f);
    EXPECT_EQ(newHandle.slot, oldHandle.slot);
    EXPECT_EQ(newHandle.generation, oldHandle.generation);
    const auto current = world.GetBroadPhaseDebugSnapshot();
    ASSERT_NE(current, nullptr);
    EXPECT_FALSE(current->selectionValid);
    EXPECT_TRUE(current->proxies.empty());
    ASSERT_EQ(retained->proxies.size(), 1u);
    EXPECT_FLOAT_EQ(retained->proxies.front().bounds.max.x, 1.0f);
}

TEST_F(WorldSceneSyncTest, BroadPhaseDebugSkippedPassDoesNotPublishStaleCandidatePairs)
{
    physics::SphereCollider first(1.0f);
    physics::SphereCollider second(1.0f);
    auto trigger = Instance(first);
    trigger.isTrigger = true;
    physics::BroadPhaseDebugRequest request;
    request.enabled = request.candidatePairs = true;
    world.SetBroadPhaseDebugRequest(request);
    world.BeginSceneSync();
    world.SyncCollider({}, trigger);
    world.SyncCollider({}, Instance(second));
    world.EndSceneSync();
    world.Step(1.0f / 60.0f);
    const auto observed = world.GetBroadPhaseDebugSnapshot();
    ASSERT_NE(observed, nullptr);
    ASSERT_EQ(observed->pairs.size(), 1u);
    world.BeginSceneSync();
    world.EndSceneSync();
    world.Step(1.0f / 60.0f);
    world.BeginSceneSync();
    world.EndSceneSync();
    world.Step(1.0f / 60.0f);
    const auto skipped = world.GetBroadPhaseDebugSnapshot();
    ASSERT_NE(skipped, nullptr);
    EXPECT_FALSE(skipped->broadPhaseRan);
    EXPECT_FALSE(skipped->candidatePairsAvailable);
    EXPECT_EQ(skipped->structure, physics::BroadPhaseDebugStructure::UNAVAILABLE);
    EXPECT_TRUE(skipped->pairs.empty());
    EXPECT_GT(skipped->sequence, observed->sequence);
    EXPECT_EQ(observed->pairs.size(), 1u);
}

} /// @note namespace fbzz::tests
