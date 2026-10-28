/// @file    CylinderGroundRegressionTests.cpp
/// @brief   剛体を持たない大きな床と小さい円柱の接触方向・落下停止を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/CollisionPair.hpp>
#include <Physics/CylinderCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include <Physics/PhysicsSolver.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/World.hpp>

#include <algorithm>
#include <initializer_list>
#include <vector>

namespace fbzz::tests {

class CylinderGroundRegressionTest : public testkit::Fixture {
protected:
    /// @note MIXED_SHAPES 256 の index 3 は 16 列の (-5.4, y, -9)。床の full size は (27.2, 1, 27.2)。
    /// @note 床の body は nullptr のままにし、両側の剛体位置による法線補正に依存しない。
    void Prepare(float height)
    {
        m_material.restitution = 0.0f;
        m_material.staticFriction = 0.5f;
        m_material.dynamicFriction = 0.4f;
        m_floor.Update({0.0f, -0.5f, 0.0f}, math::Quaternion::Identity());
        m_body.SetMass(1.0f);
        m_body.SetPosition({-5.4f, height, -9.0f});
        m_body.SetRotation(math::Quaternion::Identity());
        m_body.SetVelocity(math::Vector3::ZERO);
        m_body.SetAngularVelocity(math::Vector3::ZERO);
        m_body.m_allowSleeping = false;
        m_body.m_linearDrag = 0.0f;
        m_body.m_angularDrag = 0.0f;
        m_body.SetInertiaFromCollider(&m_cylinder);
        m_cylinder.Update(m_body.GetPosition(), m_body.GetRotation());
        m_ground.collider = &m_floor;
        m_ground.material = &m_material;
        m_falling.collider = &m_cylinder;
        m_falling.body = &m_body;
        m_falling.material = &m_material;
    }

    physics::OBBCollider m_floor{{13.6f, 0.5f, 13.6f}};
    physics::CylinderCollider m_cylinder{0.25f, 0.25f};
    physics::RigidBody m_body;
    physics::PhysicsMaterial m_material;
    physics::ColliderInstance m_ground;
    physics::ColliderInstance m_falling;
};

TEST_F(CylinderGroundRegressionTest, BodylessFloorContactPushesCylinderUpInBothOperandOrders)
{
    Prepare(0.24f);
    physics::PhysicsSolver solver;
    for (const bool swapped : {false, true}) {
        const std::vector<physics::CollisionPair> pairs{
            swapped ? physics::CollisionPair{&m_falling, &m_ground}
                    : physics::CollisionPair{&m_ground, &m_falling}};
        std::vector<physics::ContactPoint> contacts;
        solver.NarrowPhase(pairs, contacts);

        ASSERT_FALSE(contacts.empty()) << "swapped=" << swapped;
        const math::Vector3 expectedNormal = swapped ? math::Vector3::UP : -math::Vector3::UP;
        for (const physics::ContactPoint& contact : contacts) {
            EXPECT_VEC3_NEAR(contact.normal, expectedNormal, testkit::kLooseTolerance);
            EXPECT_NEAR(contact.depth, 0.01f, testkit::kLooseTolerance);
            EXPECT_EQ(contact.bodyA, swapped ? &m_body : nullptr);
            EXPECT_EQ(contact.bodyB, swapped ? nullptr : &m_body);
        }
    }
}

TEST_F(CylinderGroundRegressionTest, FallingCylinderStaysAboveBodylessLargeFloorFor181FixedSteps)
{
    Prepare(2.0f);
    physics::World world;
    world.SetGravity({0.0f, -9.81f, 0.0f});
    world.BeginSceneSync();
    world.SyncBody({}, &m_body);
    world.SyncCollider({}, m_ground);
    world.SyncCollider({}, m_falling);
    world.EndSceneSync();

    /// @note activation 1 step + warmup 60 + measure 120 と同じ固定刻みで、測定 runner の接地境界を検証する。
    constexpr float FIXED_SECONDS = 1.0f / 60.0f;
    constexpr int FIXED_STEPS = 181;
    constexpr float MIN_CENTER_Y = 0.10f;
    float minHeight = m_body.GetPosition().y;
    bool observedContact = false;
    for (int step = 0; step < FIXED_STEPS; ++step) {
        world.Step(FIXED_SECONDS);
        ASSERT_TRUE(m_body.IsFiniteState()) << "step=" << step;
        minHeight = std::min(minHeight, m_body.GetPosition().y);
        observedContact = observedContact || !world.GetEnterEvents().empty() || !world.GetStayEvents().empty();
        ASSERT_GE(m_body.GetPosition().y, MIN_CENTER_Y) << "step=" << step;
    }

    EXPECT_TRUE(observedContact);
    EXPECT_GE(minHeight, MIN_CENTER_Y);
    EXPECT_LT(m_body.GetPosition().y, 2.0f - 0.0001f);
    EXPECT_LE(m_body.GetPosition().y, 8.0f);
}

} /// @note namespace fbzz::tests
