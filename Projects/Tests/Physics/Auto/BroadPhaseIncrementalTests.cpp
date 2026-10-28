/// @file    BroadPhaseIncrementalTests.cpp
/// @brief   境界・所属・状態が変わる連続呼出しでも候補集合と寿命を保つ。
/// @author  Hasegawa Jin
/// @date    2026-10-22
#include <TestKit/TestKit.hpp>
#include <Physics/PhysicsSolver.hpp>
#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {
using Pair = std::pair<std::size_t, std::size_t>;

struct Ball {
    physics::SphereCollider shape{1.0f};
    physics::RigidBody body;
    Ball(const math::Vector3& position) {
        shape.Update(position, math::Quaternion::Identity());
        body.SetMass(1.0f);
    }
    physics::ColliderInstance Instance() {
        physics::ColliderInstance instance;
        instance.collider = &shape;
        instance.body = &body;
        return instance;
    }
};

struct Input {
    std::vector<std::unique_ptr<Ball>> balls;
    std::vector<physics::ColliderInstance> instances;
    explicit Input(int count) {
        for (int index = 0; index < count; ++index) {
            balls.push_back(std::make_unique<Ball>(math::Vector3{
                static_cast<float>(index % 8) * 3.0f,
                static_cast<float>(index / 8) * 3.0f, 0.0f}));
            instances.push_back(balls.back()->Instance());
        }
    }
};

std::vector<Pair> Expected(const std::vector<physics::ColliderInstance>& input,
                         const std::function<bool(int, int)>& filter)
{
    std::vector<Pair> expected;
    for (std::size_t first = 0; first < input.size(); ++first) {
        const auto& a = input[first];
        if (!a.collider) continue;
        for (std::size_t second = first + 1; second < input.size(); ++second) {
            const auto& b = input[second];
            if (!b.collider || (a.body && a.body == b.body)) continue;
            if (filter && !filter(a.layer, b.layer)) continue;
            const auto inactive = [](const physics::ColliderInstance& value) {
                return !value.body || value.body->IsStatic() || value.body->IsSleeping();
            };
            if (!a.isTrigger && !b.isTrigger && inactive(a) && inactive(b)) continue;
            if (a.collider->GetAABB().Overlaps(b.collider->GetAABB())) expected.emplace_back(first, second);
        }
    }
    return expected;
}

void ExpectCurrent(physics::PhysicsSolver& solver, const std::vector<physics::ColliderInstance>& input,
                   const std::function<bool(int, int)>& filter = {})
{
    std::vector<physics::CollisionPair> output;
    solver.BroadPhase(input, output, filter);
    std::vector<Pair> actual;
    for (const auto& pair : output) {
        const auto first = static_cast<std::size_t>(pair.colliderA - input.data());
        const auto second = static_cast<std::size_t>(pair.colliderB - input.data());
        ASSERT_LT(first, input.size());
        ASSERT_LT(second, input.size());
        ASSERT_NE(first, second);
        actual.emplace_back((std::min)(first, second), (std::max)(first, second));
    }
    std::sort(actual.begin(), actual.end());
    EXPECT_EQ(actual, Expected(input, filter));
}
} /// @note namespace

class BroadPhaseIncrementalTest : public testkit::Fixture {};

TEST_F(BroadPhaseIncrementalTest, RefitsOneMovedLeafAndItsSharedAncestors)
{
    Input input(96);
    physics::PhysicsSolver solver;
    ExpectCurrent(solver, input.instances);

    input.balls[0]->shape.Update({21.0f, 30.0f, 0.0f}, math::Quaternion::Identity());
    ExpectCurrent(solver, input.instances);
    input.balls[1]->shape.Update({21.5f, 30.0f, 0.0f}, math::Quaternion::Identity());
    ExpectCurrent(solver, input.instances);
    input.balls[0]->shape.Update({-30.0f, 0.0f, 0.0f}, math::Quaternion::Identity());
    ExpectCurrent(solver, input.instances);
}

TEST_F(BroadPhaseIncrementalTest, DirectRadiusChangesRefreshTheRetainedBounds)
{
    Input input(32);
    physics::PhysicsSolver solver;
    ExpectCurrent(solver, input.instances);

    input.balls[4]->shape.m_radius = 30.0f;
    ExpectCurrent(solver, input.instances);
    input.balls[4]->shape.m_radius = 0.1f;
    ExpectCurrent(solver, input.instances);
}

TEST_F(BroadPhaseIncrementalTest, DenseMotionAndTeleportsKeepExactlyTheBruteForceCandidates)
{
    Input input(64);
    physics::PhysicsSolver solver;
    for (int frame = 0; frame < 16; ++frame) {
        for (std::size_t index = 0; index < input.balls.size(); ++index) {
            const auto slot = (index * 13 + static_cast<std::size_t>(frame) * 7) % input.balls.size();
            input.balls[index]->shape.Update({static_cast<float>(slot % 8) * 1.2f,
                static_cast<float>(slot / 8) * 1.2f, frame % 2 ? 100.0f : 0.0f}, math::Quaternion::Identity());
        }
        ExpectCurrent(solver, input.instances);
    }
}

TEST_F(BroadPhaseIncrementalTest, RechecksSleepStaticTriggerSharedBodyAndLayerStateWithoutMotion)
{
    Input input(16);
    for (auto& ball : input.balls) ball->shape.Update({}, math::Quaternion::Identity());
    physics::PhysicsSolver solver;
    ExpectCurrent(solver, input.instances);

    for (auto& ball : input.balls) ball->body.Sleep();
    ExpectCurrent(solver, input.instances);
    input.balls[0]->body.WakeUp();
    ExpectCurrent(solver, input.instances);
    for (auto& ball : input.balls) ball->body.m_isStatic = true;
    input.instances[2].isTrigger = true;
    input.instances[3].body = input.instances[2].body;
    input.instances[4].body = nullptr;
    input.instances[5].layer = 2;
    bool allow = false;
    const auto filter = [&allow](int first, int second) { return allow || first == second; };
    ExpectCurrent(solver, input.instances, filter);
    allow = true;
    ExpectCurrent(solver, input.instances, filter);
}

TEST_F(BroadPhaseIncrementalTest, RemovalInsertionReorderingAndVectorRelocationUseOnlyCurrentInstances)
{
    Input input(32);
    physics::PhysicsSolver solver;
    ExpectCurrent(solver, input.instances);

    input.instances[4].collider = nullptr;
    ExpectCurrent(solver, input.instances);
    input.instances.erase(input.instances.begin() + 9);
    ExpectCurrent(solver, input.instances);
    input.instances.push_back(input.balls[9]->Instance());
    std::reverse(input.instances.begin(), input.instances.end());
    input.instances.reserve(1024);
    ExpectCurrent(solver, input.instances);
    input.instances.clear();
    ExpectCurrent(solver, input.instances);
    input.instances.push_back(input.balls[1]->Instance());
    ExpectCurrent(solver, input.instances);
    input.instances.push_back(input.balls[2]->Instance());
    ExpectCurrent(solver, input.instances);
}

TEST_F(BroadPhaseIncrementalTest, ReplacementAtTheSameIndexDoesNotRetainOldShapeOrBodyPointers)
{
    Input input(32);
    physics::PhysicsSolver solver;
    ExpectCurrent(solver, input.instances);

    input.balls[7] = std::make_unique<Ball>(math::Vector3{0.5f, 0.0f, 0.0f});
    input.instances[7] = input.balls[7]->Instance();
    ExpectCurrent(solver, input.instances);
}

TEST_F(BroadPhaseIncrementalTest, IndependentCopiedAndMovedSolversDoNotShareMutableTrees)
{
    Input first(32);
    Input second(48);
    physics::PhysicsSolver solver;
    ExpectCurrent(solver, first.instances);
    auto copied = solver;
    physics::PhysicsSolver assigned;
    assigned = solver;
    auto moved = std::move(solver);

    second.balls[0]->shape.m_radius = 100.0f;
    ExpectCurrent(copied, second.instances);
    ExpectCurrent(assigned, first.instances);
    ExpectCurrent(moved, first.instances);
    ExpectCurrent(solver, second.instances);
    moved = std::move(copied);
    ExpectCurrent(moved, first.instances);
}

TEST_F(BroadPhaseIncrementalTest, InvalidBoundsDoNotHideValidPairsAndCanRecover)
{
    Input input(16);
    physics::PhysicsSolver solver;
    ExpectCurrent(solver, input.instances);

    input.balls[0]->shape.m_radius = std::numeric_limits<float>::infinity();
    ExpectCurrent(solver, input.instances);
    input.balls[0]->shape.m_radius = std::numeric_limits<float>::quiet_NaN();
    ExpectCurrent(solver, input.instances);
    input.balls[0]->shape.m_radius = 1.0f;
    ExpectCurrent(solver, input.instances);
}

TEST_F(BroadPhaseIncrementalTest, AppendsPairsWithoutDiscardingTheCallersExistingOutput)
{
    Input input(2);
    input.balls[1]->shape.Update({0.5f, 0.0f, 0.0f}, math::Quaternion::Identity());
    physics::PhysicsSolver solver;
    std::vector<physics::CollisionPair> pairs(1);
    solver.BroadPhase(input.instances, pairs, {});

    ASSERT_EQ(pairs.size(), 2u);
    EXPECT_EQ(pairs.front().colliderA, nullptr);
    EXPECT_EQ(pairs.front().colliderB, nullptr);
}
} /// @note namespace fbzz::tests
