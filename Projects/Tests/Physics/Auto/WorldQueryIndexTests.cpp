/// @file    WorldQueryIndexTests.cpp
/// @brief   空間索引による候補削減と形状変更・同期・再入問い合わせの互換性を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include <TestKit/TestKit.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/World.hpp>
#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace fbzz::tests {
namespace {
class QuerySphere final : public physics::SphereCollider {
public:
    explicit QuerySphere(float radius) : SphereCollider(radius) {}
    physics::ColliderType GetType() const override
    {
        ++m_shapeTests;
        return physics::ColliderType::SPHERE;
    }
    mutable size_t m_shapeTests = 0;
};

class WorldQueryIndexTest : public testkit::Fixture {
protected:
    physics::World m_world;
    std::vector<std::unique_ptr<QuerySphere>> m_shapes;

    void SetUp() override
    {
        Fixture::SetUp();
        EXPECT_TRUE(m_world.OverlapSphere({}, 1).empty());
    }

    QuerySphere& Add(const math::Vector3& position)
    {
        auto shape = std::make_unique<QuerySphere>(1.0f);
        shape->Update(position, math::Quaternion::Identity());
        m_shapes.push_back(std::move(shape));
        return *m_shapes.back();
    }

    void Sync()
    {
        m_world.BeginSceneSync();
        for (const auto& shape : m_shapes) {
            physics::ColliderInstance instance;
            instance.collider = shape.get();
            m_world.SyncCollider({}, instance);
        }
        m_world.EndSceneSync();
        for (const auto& shape : m_shapes) shape->m_shapeTests = 0;
    }

    void DistantShapes()
    {
        for (int i = 0; i < 63; ++i) Add({50.0f + i * 4.0f, 0.0f, 10.0f});
    }

    size_t ShapeTests() const
    {
        size_t count = 0;
        for (const auto& shape : m_shapes) count += shape->m_shapeTests;
        return count;
    }
};

TEST_F(WorldQueryIndexTest, PrunesDistantShapesForAllFourQueryFamilies)
{
    auto& target = Add({0, 0, 10});
    DistantShapes();
    Sync();
    physics::World::RaycastHit hit;
    ASSERT_TRUE(m_world.Raycast({}, math::Vector3::FORWARD, 20, hit));
    EXPECT_EQ(hit.collider, &target);
    EXPECT_NEAR(hit.distance, 9, testkit::kTolerance);
    const auto all = m_world.RaycastAll({}, math::Vector3::FORWARD, 20);
    ASSERT_EQ(all.size(), 1u);
    ASSERT_TRUE(m_world.SphereCast({}, 1, math::Vector3::FORWARD, 20, hit));
    EXPECT_EQ(hit.collider, &target);
    EXPECT_NEAR(hit.distance, 8, testkit::kTolerance);
    const auto overlaps = m_world.OverlapSphere({0, 0, 10}, 1);
    ASSERT_EQ(overlaps.size(), 1u);
    EXPECT_EQ(overlaps[0]->collider, &target);
    EXPECT_LE(ShapeTests(), 4u);
}

TEST_F(WorldQueryIndexTest, FindsEveryHitAndPreservesNearestRegistrationTie)
{
    auto& first = Add({0, 0, 10});
    auto& tied = Add({0, 0, 10});
    auto& near = Add({0, 0, 5});
    DistantShapes();
    Sync();
    physics::World::RaycastHit hit;
    ASSERT_TRUE(m_world.Raycast({}, math::Vector3::FORWARD, 20, hit));
    EXPECT_EQ(hit.collider, &near);
    ASSERT_TRUE(m_world.Raycast({}, math::Vector3::FORWARD, 20, hit,
        [&near](const auto& instance) { return instance.collider != &near; }));
    EXPECT_EQ(hit.collider, &first);
    const auto all = m_world.RaycastAll({}, math::Vector3::FORWARD, 20);
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0].collider, &near);
    EXPECT_NEAR(all[1].distance, 9, testkit::kTolerance);
    EXPECT_NEAR(all[2].distance, 9, testkit::kTolerance);
    ASSERT_TRUE(m_world.SphereCast({}, 1, math::Vector3::FORWARD, 20, hit,
        [&near](const auto& instance) { return instance.collider != &near; }));
    EXPECT_EQ(hit.collider, &first);
    EXPECT_NE(hit.collider, &tied);
}

TEST_F(WorldQueryIndexTest, ObservesDirectShapeMovementAndRadiusChangesWithoutAnExtraStep)
{
    auto& target = Add({30, 0, 10});
    DistantShapes();
    Sync();
    physics::World::RaycastHit hit;
    EXPECT_FALSE(m_world.Raycast({}, math::Vector3::FORWARD, 20, hit));
    target.Update({0, 0, 10}, math::Quaternion::Identity());
    ASSERT_TRUE(m_world.Raycast({}, math::Vector3::FORWARD, 20, hit));
    EXPECT_EQ(hit.collider, &target);
    target.m_radius = 3;
    ASSERT_TRUE(m_world.Raycast({}, math::Vector3::FORWARD, 20, hit));
    EXPECT_NEAR(hit.distance, 7, testkit::kTolerance);
    EXPECT_EQ(m_world.OverlapSphere({0, 0, 6}, 1).size(), 1u);
    Sync();
    ASSERT_TRUE(m_world.SphereCast({}, 1, math::Vector3::FORWARD, 20, hit));
    EXPECT_NEAR(hit.distance, 6, testkit::kTolerance);
    EXPECT_EQ(target.m_shapeTests, 1u);
}

TEST_F(WorldQueryIndexTest, RefitsMovedShapesAndRemovesUnregisteredShapes)
{
    auto& target = Add({0, 0, 10});
    DistantShapes();
    Sync();
    physics::World::RaycastHit hit;
    ASSERT_TRUE(m_world.Raycast({}, math::Vector3::FORWARD, 20, hit));
    target.Update({0, 0, -10}, math::Quaternion::Identity());
    Sync();
    EXPECT_FALSE(m_world.Raycast({}, math::Vector3::FORWARD, 20, hit));
    ASSERT_TRUE(m_world.Raycast({}, -math::Vector3::FORWARD, 20, hit));
    EXPECT_EQ(hit.collider, &target);
    m_world.BeginSceneSync();
    m_world.EndSceneSync();
    EXPECT_FALSE(m_world.Raycast({}, -math::Vector3::FORWARD, 20, hit));
    EXPECT_TRUE(m_world.OverlapSphere({}, 100).empty());
    Sync();
    ASSERT_TRUE(m_world.Raycast({}, -math::Vector3::FORWARD, 20, hit));
    EXPECT_EQ(hit.collider, &target);
}

TEST_F(WorldQueryIndexTest, AllowsNestedQueriesInsideAFilterWithoutLosingCandidates)
{
    Add({0, 0, 5});
    Add({0, 0, 10});
    DistantShapes();
    Sync();
    size_t nestedCalls = 0;
    const auto all = m_world.RaycastAll({}, math::Vector3::FORWARD, 20, [this, &nestedCalls](const auto&) {
        const auto overlaps = m_world.OverlapSphere({0, 0, 5}, 1);
        EXPECT_EQ(overlaps.size(), 1u);
        ++nestedCalls;
        return true;
    });
    EXPECT_EQ(all.size(), 2u);
    EXPECT_EQ(nestedCalls, 2u);
}

TEST_F(WorldQueryIndexTest, KeepsInfiniteDistanceAndExactBoundaryHits)
{
    auto& target = Add({0, 0, 10});
    DistantShapes();
    Sync();
    physics::World::RaycastHit hit;
    ASSERT_TRUE(m_world.Raycast({}, math::Vector3::FORWARD, std::numeric_limits<float>::infinity(), hit));
    EXPECT_EQ(hit.collider, &target);
    EXPECT_TRUE(m_world.Raycast({}, math::Vector3::FORWARD, 9, hit));
    EXPECT_FALSE(m_world.Raycast({}, math::Vector3::FORWARD, 8, hit));
    EXPECT_EQ(m_world.OverlapSphere({0, 0, 8}, 1).size(), 1u);
}
} /// @note namespace
TEST_F(WorldQueryIndexTest, RetiresDestroyedIndexedShapesBeforeFiltersAndEveryQueryFamily)
{
    auto* removed = &Add({0, 0, 10});
    auto& survivor = Add({0, 0, 15});
    DistantShapes();
    Sync();
    m_shapes.erase(m_shapes.begin());
    const auto liveOnly = [removed](const auto& instance) {
        EXPECT_NE(instance.collider, removed);
        return true;
    };
    physics::World::RaycastHit hit;
    ASSERT_TRUE(m_world.Raycast({}, math::Vector3::FORWARD, 20, hit, liveOnly));
    EXPECT_EQ(hit.collider, &survivor);
    const auto all = m_world.RaycastAll({}, math::Vector3::FORWARD, 20);
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].collider, &survivor);
    ASSERT_TRUE(m_world.SphereCast({}, 1, math::Vector3::FORWARD, 20, hit));
    EXPECT_EQ(hit.collider, &survivor);
    EXPECT_TRUE(m_world.OverlapSphere({0, 0, 10}, 1).empty());
    m_world.Step(0);
    EXPECT_TRUE(m_world.OverlapSphere({0, 0, 10}, 1).empty());
}

TEST_F(WorldQueryIndexTest, ReusedShapeAddressRequiresANewRegistrationGeneration)
{
    std::optional<physics::SphereCollider> shape(std::in_place, 1.0f);
    shape->Update({0, 0, 10}, math::Quaternion::Identity());
    DistantShapes();
    m_world.BeginSceneSync();
    const auto first = m_world.SyncCollider({}, {&*shape});
    for (const auto& far : m_shapes) m_world.SyncCollider({}, {far.get()});
    m_world.EndSceneSync();
    m_world.BeginSceneSync();
    shape.reset();
    shape.emplace(1.0f);
    shape->Update({0, 0, 10}, math::Quaternion::Identity());

    physics::World::RaycastHit hit;
    EXPECT_FALSE(m_world.Raycast({}, math::Vector3::FORWARD, 20, hit));
    EXPECT_TRUE(m_world.RaycastAll({}, math::Vector3::FORWARD, 20).empty());
    EXPECT_FALSE(m_world.SphereCast({}, 1, math::Vector3::FORWARD, 20, hit));
    EXPECT_TRUE(m_world.OverlapSphere({0, 0, 10}, 1).empty());

    const auto replacement = m_world.SyncCollider(first, {&*shape});
    m_world.EndSceneSync();
    EXPECT_EQ(replacement.slot, first.slot);
    EXPECT_NE(replacement.generation, first.generation);
    ASSERT_TRUE(m_world.Raycast({}, math::Vector3::FORWARD, 20, hit));
    EXPECT_EQ(hit.collider, &*shape);
}

TEST_F(WorldQueryIndexTest, CopiesDoNotKeepTheOriginalRegistrationAliveAcrossWorldMoves)
{
    auto& original = Add({0, 0, 10});
    physics::SphereCollider copy(original);
    DistantShapes();
    Sync();
    physics::World moved = std::move(m_world);
    m_shapes.erase(m_shapes.begin());

    physics::World::RaycastHit hit;
    EXPECT_FALSE(moved.Raycast({}, math::Vector3::FORWARD, 20, hit));
    moved.BeginSceneSync();
    moved.SyncCollider({}, {&copy});
    moved.EndSceneSync();
    ASSERT_TRUE(moved.Raycast({}, math::Vector3::FORWARD, 20, hit));
    EXPECT_EQ(hit.collider, &copy);
}

TEST_F(WorldQueryIndexTest, DestroyedBodyRetiresItsShapeAndConstraintBeforeTheNextStep)
{
    auto body = std::make_unique<physics::RigidBody>();
    body->m_useGravity = false;
    auto& shape = Add({0, 0, 10});
    DistantShapes();
    m_world.BeginSceneSync();
    m_world.SyncBody({}, body.get());
    m_world.SyncCollider({}, {&shape, body.get()});
    for (size_t index = 1; index < m_shapes.size(); ++index) m_world.SyncCollider({}, {m_shapes[index].get()});
    m_world.EndSceneSync();
    auto constraint = std::make_unique<physics::DistanceConstraint>(body.get(), nullptr, 1.0f);
    m_world.AddConstraint(std::move(constraint));
    body.reset();

    physics::World::RaycastHit hit;
    EXPECT_FALSE(m_world.Raycast({}, math::Vector3::FORWARD, 20, hit));
    m_world.Step(0.01f);
    EXPECT_TRUE(m_world.GetConstraints().empty());
    EXPECT_TRUE(m_world.OverlapSphere({0, 0, 10}, 1).empty());
}

} /// @note namespace fbzz::tests
