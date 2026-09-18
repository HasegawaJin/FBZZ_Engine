/// @file    ClothSolverTests.cpp
/// @brief   布の固定点・接触・風・入力検証・再現性を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <TestKit/TestKit.hpp>
#include <Physics/Cloth/ClothSolver.hpp>
#include <array>
#include <cmath>
#include <limits>

namespace fbzz::tests {

class ClothSolverTest : public testkit::Fixture {
protected:
    physics::ClothSolver m_solver;
    std::array<math::Vector3, 4> m_positions{{{0, 1, 0}, {1, 1, 0}, {0, 0, 0}, {1, 0, 0}}};
    std::array<uint32_t, 6> m_indices{0, 2, 1, 1, 2, 3};
    std::array<float, 4> m_masses{0, 0, 1, 1};
    void SetUp() override
    {
        ASSERT_TRUE(m_solver.Initialize(m_positions, m_indices, m_masses));
    }
    void Simulate(int steps)
    {
        for (int i = 0; i < steps; ++i) ASSERT_TRUE(m_solver.Step(1.0f / 60.0f));
    }
    void FoldLayers(bool pinFirst = false, float restSeparation = 0.5f)
    {
        const std::array<math::Vector3, 6> positions{{{-0.5f,0,0}, {0.5f,0,0}, {-0.5f,1,0},
            {-0.5f,0,restSeparation}, {0.5f,0,restSeparation}, {-0.5f,1,restSeparation}}};
        const std::array<uint32_t, 6> indices{0,1,2,3,4,5};
        const float w = pinFirst ? 0.0f : 1.0f;
        const std::array<float, 6> masses{w,w,w,1,1,1};
        ASSERT_TRUE(m_solver.Initialize(positions, indices, masses));
        physics::ClothSettings settings;
        settings.gravity = {};
        settings.dragCoefficient = 0.0f;
        settings.damping = 1000000.0f;
        settings.stretchCompliance = settings.bendCompliance = 1000000.0f;
        settings.substeps = 1;
        settings.iterations = 4;
        ASSERT_TRUE(m_solver.SetSettings(settings));
        std::vector<physics::ClothMotionConstraint> motion;
        for (uint32_t i = 0; i < 6; ++i) motion.push_back({i, positions[i % 3], positions[i % 3], 0.0f});
        ASSERT_TRUE(m_solver.Step(1.0f / 60.0f, {}, motion));
        settings.selfCollisionDistance = 0.1f;
        ASSERT_TRUE(m_solver.SetSettings(settings));
    }
    /// @brief 固定した三角形の上へ、別の三角形を落とす。
    /// @param dropEdge true なら辺同士が十字に交わる配置、false なら 1 質点だけが三角形の内部へ落ちる配置。
    void DropOntoFixedTriangle(bool dropEdge, bool faces, bool continuous, float gravity)
    {
        const std::array<math::Vector3, 6> vertexDrop{{{0,0,0}, {1,0,0}, {0,0,1},
            {0.25f,0.2f,0.25f}, {3,0.2f,0.25f}, {3,0.2f,3}}};
        const std::array<math::Vector3, 6> edgeDrop{{{0,0,-1}, {0,0,1}, {0,-1,0.5f},
            {-1,0.2f,0}, {1,0.2f,0}, {0,1.2f,0}}};
        const std::array<uint32_t, 6> indices{0,1,2,3,4,5};
        /// @note 質点落下では遠い 2 点を固定し、落ちる質点の辺が固定三角形の辺と交差しないようにする。
        const std::array<float, 6> masses{0,0,0,1,dropEdge ? 1.0f : 0.0f,dropEdge ? 1.0f : 0.0f};
        ASSERT_TRUE(m_solver.Initialize(dropEdge ? edgeDrop : vertexDrop, indices, masses));
        physics::ClothSettings settings;
        settings.gravity = {0, gravity, 0};
        settings.dragCoefficient = 0.0f;
        if (!dropEdge) settings.stretchCompliance = settings.bendCompliance = 1000000.0f;
        settings.selfCollisionDistance = 0.05f;
        settings.selfCollisionFaces = faces;
        settings.continuousCollision = continuous;
        settings.substeps = 1;
        settings.iterations = 4;
        ASSERT_TRUE(m_solver.SetSettings(settings));
    }
};

TEST_F(ClothSolverTest, PinsStayFixedAndStretchRemainsBounded)
{
    Simulate(120);
    EXPECT_VEC3_NEAR(m_solver.Positions()[0], m_positions[0], 1.0e-6f);
    EXPECT_VEC3_NEAR(m_solver.Positions()[1], m_positions[1], 1.0e-6f);
    EXPECT_NEAR((m_solver.Positions()[2] - m_solver.Positions()[0]).Length(), 1.0f, 0.01f);
    EXPECT_NEAR((m_solver.Positions()[3] - m_solver.Positions()[1]).Length(), 1.0f, 0.01f);
}

TEST_F(ClothSolverTest, PinTargetsMoveAndResetRestoresInitialState)
{
    const math::Vector3 target{0, 2, 0};
    ASSERT_TRUE(m_solver.SetPinTarget(0, target));
    EXPECT_FALSE(m_solver.SetPinTarget(2, target));
    Simulate(1);
    EXPECT_VEC3_NEAR(m_solver.Positions()[0], target, 1.0e-6f);
    m_solver.Reset();
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_VEC3_NEAR(m_solver.Positions()[i], m_positions[i], 1.0e-6f);
        EXPECT_VEC3_NEAR(m_solver.Velocities()[i], math::Vector3::ZERO, 1.0e-6f);
    }
}

TEST_F(ClothSolverTest, InvalidTopologyDoesNotReplaceExistingCloth)
{
    auto invalid = m_indices;
    invalid[0] = 99;
    EXPECT_FALSE(m_solver.Initialize(m_positions, invalid, m_masses));
    invalid = {0, 0, 1, 1, 2, 3};
    EXPECT_FALSE(m_solver.Initialize(m_positions, invalid, m_masses));
    const std::array<uint32_t, 9> duplicate{0, 2, 1, 1, 2, 3, 0, 2, 1};
    EXPECT_FALSE(m_solver.Initialize(m_positions, duplicate, m_masses));
    EXPECT_EQ(m_solver.Positions().size(), 4u);
    EXPECT_VEC3_NEAR(m_solver.Positions()[0], m_positions[0], 1.0e-6f);
}

TEST_F(ClothSolverTest, InvalidSettingsAndStepsLeaveStateUntouched)
{
    auto settings = m_solver.Settings();
    settings.substeps = 0;
    EXPECT_FALSE(m_solver.SetSettings(settings));
    settings = m_solver.Settings();
    settings.windVelocity.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(m_solver.SetSettings(settings));
    EXPECT_FALSE(m_solver.Step(0.0f));
    EXPECT_FALSE(m_solver.Step(-1.0f));
    EXPECT_FALSE(m_solver.Step(1.0f));
    physics::ClothContact plane;
    plane.type = physics::ClothContactType::PLANE;
    plane.normal = {};
    EXPECT_FALSE(m_solver.Step(1.0f / 60.0f, std::span(&plane, 1)));
    EXPECT_VEC3_NEAR(m_solver.Positions()[2], m_positions[2], 1.0e-6f);
}

TEST_F(ClothSolverTest, PlanePreservesThicknessAndDoesNotMovePins)
{
    physics::ClothContact plane;
    plane.type = physics::ClothContactType::PLANE;
    plane.offset = 0.2f;
    for (int i = 0; i < 60; ++i)
        ASSERT_TRUE(m_solver.Step(1.0f / 60.0f, std::span(&plane, 1)));
    EXPECT_GE(m_solver.Positions()[2].y, 0.2f + m_solver.Settings().thickness - 1.0e-5f);
    EXPECT_VEC3_NEAR(m_solver.Positions()[0], m_positions[0], 1.0e-6f);
}

TEST_F(ClothSolverTest, SphereAndZeroLengthCapsulePushParticlesOut)
{
    for (auto type : {physics::ClothContactType::SPHERE, physics::ClothContactType::CAPSULE}) {
        m_solver.Reset();
        physics::ClothContact contact;
        contact.type = type;
        contact.a = m_positions[2];
        contact.b = contact.a;
        contact.radius = 0.25f;
        ASSERT_TRUE(m_solver.Step(1.0f / 60.0f, std::span(&contact, 1)));
        EXPECT_GE((m_solver.Positions()[2] - contact.a).Length(), 0.25499f);
    }
}

TEST_F(ClothSolverTest, WindPushesDownwindForEitherTriangleWinding)
{
    auto settings = m_solver.Settings();
    settings.gravity = {};
    settings.windVelocity = {0, 0, 2};
    ASSERT_TRUE(m_solver.SetSettings(settings));
    Simulate(10);
    EXPECT_GT(m_solver.Positions()[2].z, 0.0f);
    const auto expected = m_solver.Positions();
    std::array<uint32_t, 6> reversed{0, 1, 2, 1, 3, 2};
    ASSERT_TRUE(m_solver.Initialize(m_positions, reversed, m_masses));
    Simulate(10);
    EXPECT_GT(m_solver.Positions()[2].z, 0.0f);
    EXPECT_NEAR(m_solver.Positions()[2].z, expected[2].z, 0.002f);
}

TEST_F(ClothSolverTest, SameInputsAreReproducibleAfterReset)
{
    Simulate(30);
    const auto first = m_solver.Positions();
    m_solver.Reset();
    Simulate(30);
    for (size_t i = 0; i < first.size(); ++i)
        EXPECT_VEC3_NEAR(m_solver.Positions()[i], first[i], 1.0e-6f);
}

TEST_F(ClothSolverTest, OverflowRollsBackTheStep)
{
    auto settings = m_solver.Settings();
    settings.gravity = {0, -std::numeric_limits<float>::max(), 0};
    ASSERT_TRUE(m_solver.SetSettings(settings));
    EXPECT_FALSE(m_solver.Step(1.0f / 60.0f));
    EXPECT_VEC3_NEAR(m_solver.Positions()[2], m_positions[2], 1.0e-6f);
    EXPECT_VEC3_NEAR(m_solver.Velocities()[2], math::Vector3::ZERO, 1.0e-6f);
}

TEST_F(ClothSolverTest, MotionSphereBoundsFreeParticleAndOverridesConflictingContact)
{
    physics::ClothMotionConstraint motion{2, {0, 0, 0}, {0, 0, 0}, 0.1f};
    physics::ClothContact plane;
    plane.type = physics::ClothContactType::PLANE;
    plane.offset = 0.5f;
    for (int i = 0; i < 60; ++i) {
        ASSERT_TRUE(m_solver.Step(1.0f / 60.0f, std::span(&plane, 1), std::span(&motion, 1)));
        EXPECT_LE((m_solver.Positions()[2] - motion.center).Length(), 0.10001f);
    }
}

TEST_F(ClothSolverTest, ZeroRadiusInterpolatesAnchorAndRestoresMassWhenRemoved)
{
    auto settings = m_solver.Settings();
    settings.gravity = {};
    settings.dragCoefficient = 0.0f;
    ASSERT_TRUE(m_solver.SetSettings(settings));
    physics::ClothMotionConstraint motion{2, m_positions[2], {0, 0, 0.1f}, 0.0f};
    ASSERT_TRUE(m_solver.Step(1.0f / 60.0f, {}, std::span(&motion, 1)));
    EXPECT_VEC3_NEAR(m_solver.Positions()[2], motion.center, 1.0e-6f);
    EXPECT_NEAR(m_solver.Velocities()[2].z, 6.0f, 0.001f);
    ASSERT_TRUE(m_solver.Step(1.0f / 60.0f));
    EXPECT_GT(m_solver.Positions()[2].z, 0.1f);
}

TEST_F(ClothSolverTest, MotionCanOverridePinOnlyWithZeroRadius)
{
    physics::ClothMotionConstraint motion{0, m_positions[0], {0, 1.1f, 0}, 0.0f};
    ASSERT_TRUE(m_solver.Step(1.0f / 60.0f, {}, std::span(&motion, 1)));
    EXPECT_VEC3_NEAR(m_solver.Positions()[0], motion.center, 1.0e-6f);
    motion.radius = 0.1f;
    EXPECT_FALSE(m_solver.Step(1.0f / 60.0f, {}, std::span(&motion, 1)));
    EXPECT_VEC3_NEAR(m_solver.Positions()[0], motion.center, 1.0e-6f);
}

TEST_F(ClothSolverTest, InvalidMotionConstraintsRejectWholeStep)
{
    std::array<physics::ClothMotionConstraint, 2> motion{{{2, {}, {}, 0.1f}, {2, {}, {}, 0.1f}}};
    EXPECT_FALSE(m_solver.Step(1.0f / 60.0f, {}, motion));
    motion[1].particle = 99;
    EXPECT_FALSE(m_solver.Step(1.0f / 60.0f, {}, motion));
    motion[1].particle = 3;
    motion[1].radius = -1.0f;
    EXPECT_FALSE(m_solver.Step(1.0f / 60.0f, {}, motion));
    motion[1].radius = 0.1f;
    motion[1].center.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(m_solver.Step(1.0f / 60.0f, {}, motion));
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_VEC3_NEAR(m_solver.Positions()[i], m_positions[i], 1.0e-6f);
        EXPECT_VEC3_NEAR(m_solver.Velocities()[i], math::Vector3::ZERO, 1.0e-6f);
    }
}

TEST_F(ClothSolverTest, SelfCollisionSeparatesFoldedLayersAndPreservesCenterOfMass)
{
    FoldLayers();
    ASSERT_TRUE(m_solver.Step(1.0f / 60.0f));
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_GE((m_solver.Positions()[i] - m_solver.Positions()[i + 3]).Length(), 0.09999f);
        EXPECT_NEAR(m_solver.Positions()[i].z + m_solver.Positions()[i + 3].z, 0.0f, 1.0e-6f);
    }
    const auto first = m_solver.Positions();
    FoldLayers();
    ASSERT_TRUE(m_solver.Step(1.0f / 60.0f));
    for (size_t i = 0; i < first.size(); ++i) EXPECT_VEC3_NEAR(m_solver.Positions()[i], first[i], 1.0e-6f);
}

TEST_F(ClothSolverTest, SelfCollisionDoesNotMovePinsAndCanBeDisabled)
{
    FoldLayers(true);
    ASSERT_TRUE(m_solver.Step(1.0f / 60.0f));
    EXPECT_NEAR(m_solver.Positions()[0].z, 0.0f, 1.0e-6f);
    EXPECT_GE(m_solver.Positions()[3].z, 0.09999f);
    FoldLayers();
    auto settings = m_solver.Settings();
    settings.selfCollisionDistance = 0.0f;
    ASSERT_TRUE(m_solver.SetSettings(settings));
    ASSERT_TRUE(m_solver.Step(1.0f / 60.0f));
    EXPECT_NEAR((m_solver.Positions()[0] - m_solver.Positions()[3]).Length(), 0.0f, 1.0e-6f);
}

TEST_F(ClothSolverTest, SelfCollisionExcludesRestNeighborsAndConnectedVertices)
{
    FoldLayers(false, 0.05f);
    ASSERT_TRUE(m_solver.Step(1.0f / 60.0f));
    EXPECT_NEAR((m_solver.Positions()[0] - m_solver.Positions()[3]).Length(), 0.0f, 1.0e-6f);
    ASSERT_TRUE(m_solver.Initialize(m_positions, m_indices, m_masses));
    auto settings = m_solver.Settings();
    settings.selfCollisionDistance = 0.1f;
    ASSERT_TRUE(m_solver.SetSettings(settings));
    const std::array<physics::ClothMotionConstraint, 2> motion{{
        {2, m_positions[0], m_positions[0], 0.0f}, {3, m_positions[1], m_positions[1], 0.0f}}};
    ASSERT_TRUE(m_solver.Step(1.0f / 60.0f, {}, motion));
    ASSERT_TRUE(m_solver.Step(1.0f / 60.0f));
    EXPECT_VEC3_NEAR(m_solver.Positions()[2], m_solver.Positions()[0], 1.0e-6f);
    EXPECT_VEC3_NEAR(m_solver.Positions()[3], m_solver.Positions()[1], 1.0e-6f);
}

TEST_F(ClothSolverTest, InvalidSelfCollisionSettingsPreserveExistingSettings)
{
    auto settings = m_solver.Settings();
    settings.selfCollisionDistance = -1.0f;
    EXPECT_FALSE(m_solver.SetSettings(settings));
    settings.selfCollisionDistance = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(m_solver.SetSettings(settings));
    settings.selfCollisionDistance = 0.1f;
    settings.selfCollisionStiffness = 1.1f;
    EXPECT_FALSE(m_solver.SetSettings(settings));
    EXPECT_FLOAT_EQ(m_solver.Settings().selfCollisionDistance, 0.0f);
}

TEST_F(ClothSolverTest, VertexStopsAboveTriangleInteriorOnlyWithFaceContacts)
{
    for (bool faces : {false, true}) {
        DropOntoFixedTriangle(false, faces, false, -9.81f);
        Simulate(60);
        const auto& p = m_solver.Positions()[3];
        if (faces) {
            EXPECT_GE(p.y, 0.05f - 1.0e-3f);
            EXPECT_LT(p.y, 0.2f);
        } else {
            EXPECT_LT(p.y, 0.0f);
        }
        EXPECT_VEC3_NEAR(m_solver.Positions()[0], (math::Vector3{0, 0, 0}), 1.0e-6f);
    }
}

TEST_F(ClothSolverTest, EdgeRestsOnCrossingEdgeWithFaceContacts)
{
    DropOntoFixedTriangle(true, true, false, -9.81f);
    Simulate(60);
    const float middle = 0.5f * (m_solver.Positions()[3].y + m_solver.Positions()[4].y);
    EXPECT_GE(middle, 0.05f - 2.0e-3f);
    EXPECT_LT(middle, 0.2f);
}

TEST_F(ClothSolverTest, ContinuousCollisionStopsTunnelingThroughTriangleAndEdge)
{
    for (bool edge : {false, true}) {
        for (bool continuous : {false, true}) {
            /// @note 1 substep で 0.55 m 落ちるため、離散の近接判定だけでは裏へ抜ける。
            DropOntoFixedTriangle(edge, true, continuous, -2000.0f);
            ASSERT_TRUE(m_solver.Step(1.0f / 60.0f));
            const float height = edge ? 0.5f * (m_solver.Positions()[3].y + m_solver.Positions()[4].y)
                                      : m_solver.Positions()[3].y;
            if (continuous) EXPECT_GE(height, 0.05f - 2.0e-3f) << "edge=" << edge;
            else EXPECT_LT(height, 0.0f) << "edge=" << edge;
        }
    }
}

TEST_F(ClothSolverTest, ContinuousCollisionIsReproducible)
{
    DropOntoFixedTriangle(true, true, true, -2000.0f);
    ASSERT_TRUE(m_solver.Step(1.0f / 60.0f));
    const auto first = m_solver.Positions();
    DropOntoFixedTriangle(true, true, true, -2000.0f);
    ASSERT_TRUE(m_solver.Step(1.0f / 60.0f));
    for (size_t i = 0; i < first.size(); ++i) EXPECT_VEC3_NEAR(m_solver.Positions()[i], first[i], 1.0e-6f);
}

TEST_F(ClothSolverTest, ContinuousCollisionStopsTunnelingThroughSphereAndCapsule)
{
    for (auto type : {physics::ClothContactType::SPHERE, physics::ClothContactType::CAPSULE}) {
        for (bool continuous : {false, true}) {
            ASSERT_TRUE(m_solver.Initialize(m_positions, m_indices, m_masses));
            physics::ClothSettings settings;
            settings.gravity = {0, -2000.0f, 0};
            settings.dragCoefficient = 0.0f;
            settings.stretchCompliance = settings.bendCompliance = 1000000.0f;
            settings.substeps = 1;
            settings.continuousCollision = continuous;
            ASSERT_TRUE(m_solver.SetSettings(settings));
            physics::ClothContact contact;
            contact.type = type;
            contact.a = {type == physics::ClothContactType::CAPSULE ? -0.1f : 0.0f, -0.3f, 0};
            contact.b = {type == physics::ClothContactType::CAPSULE ? 0.1f : 0.0f, -0.3f, 0};
            contact.radius = 0.05f;
            ASSERT_TRUE(m_solver.Step(1.0f / 60.0f, std::span(&contact, 1)));
            const auto& p = m_solver.Positions()[2];
            if (continuous) {
                EXPECT_NEAR(p.y, -0.3f + 0.05f + settings.thickness, 1.0e-3f);
                EXPECT_GE(m_solver.Velocities()[2].y, -1.0e-3f);
            } else {
                EXPECT_LT(p.y, -0.35f);
            }
        }
    }
}

}
