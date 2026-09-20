/// @file    ClothAdvancedTests.cpp
/// @brief   二面角曲げ、接触の力積収支、別 Cloth の衝突を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include <TestKit/TestKit.hpp>
#include <Physics/Cloth/ClothSolver.hpp>
#include <cmath>
#include <limits>

namespace fbzz::tests {
class ClothAdvancedTest : public testkit::Fixture {
protected:
    physics::ClothSettings Quiet()
    {
        physics::ClothSettings settings;
        settings.gravity = {};
        settings.damping = 0;
        settings.dragCoefficient = 0;
        settings.substeps = 1;
        settings.iterations = 8;
        return settings;
    }
    void Triangle(physics::ClothSolver& solver, float z, float inverseMass = 1)
    {
        const std::array<math::Vector3,3> p{{{0,0,z},{1,0,z},{0,1,z}}};
        const std::array<uint32_t,3> indices{0,1,2};
        const std::array<float,3> masses{inverseMass,inverseMass,inverseMass};
        ASSERT_TRUE(solver.Initialize(p,indices,masses));
        ASSERT_TRUE(solver.SetSettings(Quiet()));
        ASSERT_TRUE(solver.Step(0.01f));
    }
};
TEST_F(ClothAdvancedTest, SignedDihedralRelaxesBothFoldDirectionsAndKeepsPins)
{
    for (float sign : {-1.0f,1.0f}) {
        physics::ClothSolver solver;
        const std::array<math::Vector3,4> p{{{0,1,0},{0,-1,0},{0,0,0},{1,0,0}}};
        const std::array<uint32_t,6> triangles{0,2,3,1,3,2};
        const std::array<float,4> masses{1,0,0,0};
        ASSERT_TRUE(solver.Initialize(p,triangles,masses));
        auto settings = Quiet();
        settings.stretchCompliance = 1.0e8f;
        settings.bendCompliance = 0;
        settings.dihedralBending = true;
        settings.damping = 1000000;
        ASSERT_TRUE(solver.SetSettings(settings));
        const math::Vector3 folded{0,1,sign*0.8f};
        const std::array<physics::ClothMotionConstraint,1> motion{{{0,folded,folded,0}}};
        ASSERT_TRUE(solver.Step(0.01f,{},motion));
        ASSERT_TRUE(solver.Step(0.01f));
        EXPECT_NEAR(solver.Positions()[0].z,0,0.001f);
        for (size_t i = 1; i < 4; ++i) EXPECT_VEC3_NEAR(solver.Positions()[i],p[i],1.0e-6f);
    }
}
TEST_F(ClothAdvancedTest, DihedralPreservesNonPlanarRestAndDoesNotResistPlanarStretch)
{
    physics::ClothSolver solver;
    std::array<math::Vector3,4> p{{{0,1,0.5f},{0,-1,0},{0,0,0},{1,0,0}}};
    const std::array<uint32_t,6> triangles{0,2,3,1,3,2};
    const std::array<float,4> masses{1,1,1,1};
    ASSERT_TRUE(solver.Initialize(p,triangles,masses));
    auto settings = Quiet(); settings.dihedralBending = true; settings.bendCompliance = 0;
    ASSERT_TRUE(solver.SetSettings(settings));
    ASSERT_TRUE(solver.Step(0.01f));
    for (size_t i = 0; i < 4; ++i) EXPECT_VEC3_NEAR(solver.Positions()[i],p[i],1.0e-6f);
    p[0].z = 0;
    ASSERT_TRUE(solver.Initialize(p,triangles,masses));
    settings.stretchCompliance = 1.0e12f; settings.damping = 1000000;
    ASSERT_TRUE(solver.SetSettings(settings));
    std::vector<physics::ClothMotionConstraint> motion;
    for (uint32_t i = 0; i < 4; ++i) motion.push_back({i,p[i]*2,p[i]*2,0});
    ASSERT_TRUE(solver.Step(0.01f,{},motion));
    ASSERT_TRUE(solver.Step(0.01f));
    for (size_t i = 0; i < 4; ++i) EXPECT_VEC3_NEAR(solver.Positions()[i],p[i]*2,1.0e-5f);
}
TEST_F(ClothAdvancedTest, SmallValidTrianglesRemainSupportedWithEitherBendingModel)
{
    const std::array<math::Vector3,4> p{{{0,0.0002f,0},{0,-0.0002f,0},{0,0,0},{0.0002f,0,0}}};
    const std::array<uint32_t,6> triangles{0,2,3,1,3,2};
    const std::array<float,4> masses{1,1,1,1};
    for (bool dihedral : {false,true}) {
        physics::ClothSolver solver;
        ASSERT_TRUE(solver.Initialize(p,triangles,masses));
        auto settings = Quiet(); settings.dihedralBending = dihedral;
        ASSERT_TRUE(solver.SetSettings(settings));
        ASSERT_TRUE(solver.Step(0.01f));
        for (size_t i = 0; i < p.size(); ++i) EXPECT_VEC3_NEAR(solver.Positions()[i],p[i],1.0e-8f);
    }
}
TEST_F(ClothAdvancedTest, ContactResponseBalancesLinearAndAngularImpulse)
{
    physics::ClothSolver solver;
    const std::array<math::Vector3,3> p{{{0,0.005f,0},{1,0.005f,0},{0,0.005f,1}}};
    const std::array<uint32_t,3> indices{0,1,2};
    const std::array<float,3> masses{1,1,1};
    ASSERT_TRUE(solver.Initialize(p,indices,masses));
    auto settings = Quiet(); settings.gravity = {0,-10,0}; settings.friction = 0;
    ASSERT_TRUE(solver.SetSettings(settings));
    physics::ClothContact plane; plane.type = physics::ClothContactType::PLANE; plane.inverseMass = 0.5f;
    ASSERT_TRUE(solver.Step(0.1f,{&plane,1}));
    ASSERT_EQ(solver.ContactResponses().size(),1u);
    math::Vector3 momentum{}, torque{};
    for (size_t i = 0; i < 3; ++i) {
        const auto impulse = solver.Velocities()[i]-math::Vector3{0,-1,0};
        momentum += impulse;
        torque += math::Vector3::Cross(p[i],impulse);
    }
    EXPECT_GT(momentum.y,0);
    EXPECT_VEC3_NEAR(momentum+solver.ContactResponses()[0].impulse,math::Vector3{},1.0e-5f);
    EXPECT_VEC3_NEAR(torque+solver.ContactResponses()[0].angularImpulse,math::Vector3{},1.0e-5f);
    plane.velocity.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(solver.Step(0.1f,{&plane,1}));
    EXPECT_TRUE(solver.ContactResponses().empty());
}
TEST_F(ClothAdvancedTest, FrictionUsesRotatingSurfaceVelocity)
{
    physics::ClothSolver solver;
    const std::array<math::Vector3,3> p{{{1,0.005f,0},{2,0.005f,0},{1,0.005f,1}}};
    const std::array<uint32_t,3> indices{0,1,2}; const std::array<float,3> masses{1,1,1};
    ASSERT_TRUE(solver.Initialize(p,indices,masses));
    auto settings = Quiet(); settings.gravity = {0,-1,0}; settings.friction = 1;
    ASSERT_TRUE(solver.SetSettings(settings));
    physics::ClothContact plane; plane.type = physics::ClothContactType::PLANE; plane.angularVelocity = {0,2,0};
    ASSERT_TRUE(solver.Step(0.01f,{&plane,1}));
    EXPECT_NEAR(solver.Velocities()[0].z,-2,1.0e-5f);
    EXPECT_NEAR(solver.Velocities()[1].z,-4,1.0e-5f);
}
TEST_F(ClothAdvancedTest, InterCollisionSeparatesByMassAndHonorsMasks)
{
    physics::ClothSolver a,b;
    Triangle(a,0,1); Triangle(b,0.02f,0.5f);
    std::array<physics::ClothInteraction,2> group{{{&a,0.1f,0,0u,{}},{&b,0.1f,1,~0u,{}}}};
    ASSERT_TRUE(physics::ClothSolver::SolveInterCollision(group,0.01f));
    EXPECT_NEAR(a.Positions()[0].z,0,1.0e-6f);
    group[0].mask = ~0u;
    ASSERT_TRUE(physics::ClothSolver::SolveInterCollision(group,0.01f));
    EXPECT_NEAR((a.Positions()[0]-b.Positions()[0]).Length(),0.1f,1.0e-5f);
    EXPECT_NEAR(a.Positions()[0].z+2*b.Positions()[0].z,0.04f,1.0e-5f);
    EXPECT_VEC3_NEAR(a.Velocities()[0]+b.Velocities()[0]*2,math::Vector3{},1.0e-5f);
}
TEST_F(ClothAdvancedTest, InterCollisionKeepsAnchorsAndRejectsInvalidGroupTransactionally)
{
    physics::ClothSolver a,b;
    Triangle(a,0,0); Triangle(b,0.02f);
    std::array<physics::ClothInteraction,2> group{{{&a,0.1f,0,~0u,{}},{&b,0.1f,1,~0u,{}}}};
    ASSERT_TRUE(physics::ClothSolver::SolveInterCollision(group,0.01f));
    EXPECT_NEAR(a.Positions()[0].z,0,1.0e-6f);
    EXPECT_NEAR(b.Positions()[0].z,0.1f,1.0e-5f);
    const auto before = b.Positions(); group[1].layer = 32;
    EXPECT_FALSE(physics::ClothSolver::SolveInterCollision(group,0.01f));
    for (size_t i = 0; i < before.size(); ++i) EXPECT_VEC3_NEAR(b.Positions()[i],before[i],1.0e-6f);
}
}
