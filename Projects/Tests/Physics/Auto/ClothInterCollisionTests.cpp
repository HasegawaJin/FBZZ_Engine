/// @file    ClothInterCollisionTests.cpp
/// @brief   独立した Cloth 間の面・辺の近接、厚み付き CCD と固定更新履歴を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include <TestKit/TestKit.hpp>
#include <Physics/Cloth/ClothSolver.hpp>

namespace fbzz::tests {
class ClothInterCollisionTest : public testkit::Fixture {
protected:
    std::array<physics::ClothSolver,2> m_solvers;
    std::array<physics::ClothInteraction,2> m_group;
    std::array<math::Vector3,3> m_floor;
    static constexpr float DT = 1.0f/60.0f;
    void Prepare(bool edge, float height, float gravity, bool faces, bool continuous, int substeps = 1, bool freeFloor = false)
    {
        m_floor = edge ? std::array<math::Vector3,3>{{{0,0,-1},{0,0,1},{0,-1,0.5f}}}
                       : std::array<math::Vector3,3>{{{0,0,0},{1,0,0},{0,0,1}}};
        const std::array<math::Vector3,3> top = edge
            ? std::array<math::Vector3,3>{{{-1,height,0},{1,height,0},{0,1+height,0}}}
            : std::array<math::Vector3,3>{{{0.25f,height,0.25f},{3,height,0.25f},{3,height,3}}};
        const std::array<uint32_t,3> indices{0,1,2};
        const float floorMass = freeFloor ? 1.0f : 0.0f;
        const std::array<float,3> a{floorMass,floorMass,floorMass}, b{1,(edge || freeFloor) ? 1.0f : 0.0f,(edge || freeFloor) ? 1.0f : 0.0f};
        ASSERT_TRUE(m_solvers[0].Initialize(m_floor,indices,a));
        ASSERT_TRUE(m_solvers[1].Initialize(top,indices,b));
        physics::ClothSettings settings;
        settings.gravity = {}; settings.damping = 0; settings.dragCoefficient = 0;
        settings.stretchCompliance = settings.bendCompliance = 1.0e8f;
        settings.substeps = substeps; settings.iterations = 1;
        ASSERT_TRUE(m_solvers[0].SetSettings(settings));
        settings.gravity = {0,gravity,0};
        ASSERT_TRUE(m_solvers[1].SetSettings(settings));
        m_group = {{{&m_solvers[0],0.05f,0,0xffffffffu,{},false,false},
                    {&m_solvers[1],0.05f,1,0xffffffffu,{},faces,continuous}}};
    }
    void Step()
    {
        ASSERT_TRUE(m_solvers[0].Step(DT));
        ASSERT_TRUE(m_solvers[1].Step(DT));
    }
    float Height(bool edge)
    {
        return edge ? (m_solvers[1].Positions()[0].y+m_solvers[1].Positions()[1].y)*0.5f : m_solvers[1].Positions()[0].y;
    }
};

TEST_F(ClothInterCollisionTest, FacesAndEdgesFillGapsBetweenParticleSpheres)
{
    for (bool edge : {false,true}) for (bool faces : {false,true}) {
        Prepare(edge,0.02f,0,faces,false);
        Step();
        ASSERT_TRUE(physics::ClothSolver::SolveInterCollision(m_group,DT));
        if (faces) EXPECT_GE(Height(edge),0.0499f);
        else EXPECT_NEAR(Height(edge),0.02f,1.0e-6f);
        for (size_t i = 0; i < 3; ++i) EXPECT_VEC3_NEAR(m_solvers[0].Positions()[i],m_floor[i],1.0e-6f);
    }
}
TEST_F(ClothInterCollisionTest, ContinuousContactsStopFastPointAndEdgeTunneling)
{
    for (bool edge : {false,true}) for (bool continuous : {false,true}) {
        Prepare(edge,0.2f,-2000,true,continuous);
        Step();
        ASSERT_LT(Height(edge),0);
        ASSERT_TRUE(physics::ClothSolver::SolveInterCollision(m_group,DT));
        if (continuous) EXPECT_GE(Height(edge),0.0499f);
        else EXPECT_LT(Height(edge),0);
    }
}
TEST_F(ClothInterCollisionTest, CcdUsesWholeFixedStepAndImpliesFaceContacts)
{
    Prepare(false,0.2f,-5000,false,true,16);
    Step();
    ASSERT_LT(Height(false),0);
    ASSERT_TRUE(physics::ClothSolver::SolveInterCollision(m_group,DT));
    EXPECT_GE(Height(false),0.0499f);
}
TEST_F(ClothInterCollisionTest, MovingPinnedFacePushesOtherClothAndKeepsItsTargets)
{
    Prepare(false,0.2f,0,true,true);
    for (uint32_t i = 0; i < 3; ++i) ASSERT_TRUE(m_solvers[0].SetPinTarget(i,m_floor[i]+math::Vector3{0,0.5f,0}));
    Step();
    ASSERT_TRUE(physics::ClothSolver::SolveInterCollision(m_group,DT));
    EXPECT_GE(Height(false),0.5499f);
    for (size_t i = 0; i < 3; ++i) EXPECT_VEC3_NEAR(m_solvers[0].Positions()[i],(m_floor[i]+math::Vector3{0,0.5f,0}),1.0e-6f);
}
TEST_F(ClothInterCollisionTest, MasksAndZeroDistanceExcludePrimitiveCcd)
{
    for (bool zeroDistance : {false,true}) {
        Prepare(false,0.2f,-2000,true,true);
        if (zeroDistance) m_group[0].distance = 0;
        else m_group[0].mask = 0;
        Step();
        const auto before = m_solvers[1].Positions();
        ASSERT_TRUE(physics::ClothSolver::SolveInterCollision(m_group,DT));
        for (size_t i = 0; i < 3; ++i) EXPECT_VEC3_NEAR(m_solvers[1].Positions()[i],before[i],1.0e-6f);
    }
}
TEST_F(ClothInterCollisionTest, PrimitiveCorrectionsPreserveMomentumOfFreeCloths)
{
    Prepare(false,0.02f,0,true,false,1,true);
    Step();
    ASSERT_TRUE(physics::ClothSolver::SolveInterCollision(m_group,DT));
    math::Vector3 momentum{};
    for (const auto& velocity : m_solvers[1].Velocities()) momentum += velocity;
    for (const auto& velocity : m_solvers[0].Velocities()) momentum += velocity;
    EXPECT_VEC3_NEAR(momentum,math::Vector3{},1.0e-4f);
    EXPECT_GT(Height(false),0.02f);
}
TEST_F(ClothInterCollisionTest, MotionLimitWinsOverIncompatibleContinuousContact)
{
    Prepare(false,0.2f,-2000,true,true);
    ASSERT_TRUE(m_solvers[0].Step(DT));
    const math::Vector3 center{0.25f,-0.3f,0.25f};
    const std::array<physics::ClothMotionConstraint,1> motion{{{0,center,center,0.01f}}};
    ASSERT_TRUE(m_solvers[1].Step(DT,{},motion));
    m_group[1].motion = motion;
    ASSERT_TRUE(physics::ClothSolver::SolveInterCollision(m_group,DT));
    EXPECT_LE((m_solvers[1].Positions()[0]-center).Length(),0.01001f);
}
TEST_F(ClothInterCollisionTest, ContinuousContactsAreReproducible)
{
    Prepare(true,0.2f,-2000,true,true);
    Step();
    ASSERT_TRUE(physics::ClothSolver::SolveInterCollision(m_group,DT));
    const auto first = m_solvers[1].Positions();
    Prepare(true,0.2f,-2000,true,true);
    Step();
    ASSERT_TRUE(physics::ClothSolver::SolveInterCollision(m_group,DT));
    for (size_t i = 0; i < 3; ++i) EXPECT_VEC3_NEAR(m_solvers[1].Positions()[i],first[i],1.0e-6f);
}
TEST_F(ClothInterCollisionTest, CoplanarMotionCannotEnterTriangleThroughItsBoundary)
{
    Prepare(false,0,0,true,true);
    const std::array<math::Vector3,3> p{{{1.2f,0,0.25f},{3,0,0.25f},{3,0,3}}};
    const std::array<uint32_t,3> indices{0,1,2};
    const std::array<float,3> masses{1,0,0};
    ASSERT_TRUE(m_solvers[1].Initialize(p,indices,masses));
    auto settings = m_solvers[1].Settings(); settings.gravity = {-5000,0,0};
    ASSERT_TRUE(m_solvers[1].SetSettings(settings));
    Step();
    ASSERT_TRUE(physics::ClothSolver::SolveInterCollision(m_group,DT));
    const auto& point = m_solvers[1].Positions()[0];
    EXPECT_GE(point.x+point.z,1.07f);
}
TEST_F(ClothInterCollisionTest, NearlyParallelEdgesStillHaveAContinuousContact)
{
    Prepare(true,0.2f,-2000,true,true);
    const std::array<math::Vector3,3> floor{{{-1,0,0},{1,0,0},{0,-1,0}}};
    const std::array<math::Vector3,3> top{{{-1,0.2f,-0.0001f},{1,0.2f,0.0001f},{0,1.2f,1}}};
    const std::array<uint32_t,3> indices{0,1,2};
    const std::array<float,3> fixed{0,0,0}, free{1,1,1};
    ASSERT_TRUE(m_solvers[0].Initialize(floor,indices,fixed));
    ASSERT_TRUE(m_solvers[1].Initialize(top,indices,free));
    m_group[0].distance = m_group[1].distance = 0.00001f;
    Step();
    ASSERT_TRUE(physics::ClothSolver::SolveInterCollision(m_group,DT));
    EXPECT_GE(Height(true),0.000009f);
}
TEST_F(ClothInterCollisionTest, PrimitiveBudgetFailureRestoresEveryParticipant)
{
    Prepare(false,0.02f,0,true,false,1,true);
    std::vector<math::Vector3> positions;
    std::vector<uint32_t> indices;
    std::vector<float> masses;
    for (uint32_t t = 0; t < 500; ++t) {
        positions.insert(positions.end(),{{0.25f,0.02f,0.25f},{3,0.02f,0.25f},{3,0.02f,3}});
        indices.insert(indices.end(),{t*3,t*3+1,t*3+2});
        masses.insert(masses.end(),{1,1,1});
    }
    ASSERT_TRUE(m_solvers[1].Initialize(positions,indices,masses));
    Step();
    const auto first = m_solvers[0].Positions(), second = m_solvers[1].Positions();
    const auto velocityA = m_solvers[0].Velocities(), velocityB = m_solvers[1].Velocities();
    ASSERT_FALSE(physics::ClothSolver::SolveInterCollision(m_group,DT));
    for (size_t i = 0; i < first.size(); ++i) {
        EXPECT_VEC3_NEAR(m_solvers[0].Positions()[i],first[i],1.0e-6f);
        EXPECT_VEC3_NEAR(m_solvers[0].Velocities()[i],velocityA[i],1.0e-6f);
    }
    for (size_t i = 0; i < second.size(); ++i) {
        EXPECT_VEC3_NEAR(m_solvers[1].Positions()[i],second[i],1.0e-6f);
        EXPECT_VEC3_NEAR(m_solvers[1].Velocities()[i],velocityB[i],1.0e-6f);
    }
}
}
