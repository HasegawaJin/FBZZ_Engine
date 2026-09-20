/// @file    ClothWindTests.cpp
/// @brief   空間的な風の重心サンプル・加算・固定点・失敗時の巻き戻しを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include <TestKit/TestKit.hpp>
#include <Physics/Cloth/ClothSolver.hpp>
#include <limits>

namespace fbzz::tests {
class ClothWindTest : public testkit::Fixture {
protected:
    const std::array<math::Vector3,6> m_positions{{{0,0,0},{1,0,0},{0,1,0},{10,0,0},{11,0,0},{10,1,0}}};
    const std::array<uint32_t,6> m_indices{0,1,2,3,4,5};
    const std::array<float,6> m_masses{0,1,1,1,1,1};
    void Prepare(physics::ClothSolver& solver, math::Vector3 wind = {})
    {
        ASSERT_TRUE(solver.Initialize(m_positions,m_indices,m_masses));
        physics::ClothSettings settings;
        settings.gravity = {}; settings.damping = 0;
        settings.windVelocity = wind;
        settings.substeps = 4; settings.iterations = 1;
        ASSERT_TRUE(solver.SetSettings(settings));
    }
};
TEST_F(ClothWindTest, ConstantSamplerAddsToTheExistingUniformWind)
{
    physics::ClothSolver reference,sampled;
    Prepare(reference,{0,0,3});
    Prepare(sampled,{0,0,1});
    ASSERT_TRUE(reference.Step(0.01f));
    ASSERT_TRUE(sampled.Step(0.01f,{},{},[](const math::Vector3&) { return math::Vector3{0,0,2}; }));
    for (size_t i = 0; i < m_positions.size(); ++i) {
        EXPECT_VEC3_NEAR(sampled.Positions()[i],reference.Positions()[i],1.0e-6f);
        EXPECT_VEC3_NEAR(sampled.Velocities()[i],reference.Velocities()[i],1.0e-6f);
    }
}
TEST_F(ClothWindTest, SamplesCurrentTriangleCentersEachSubstepAndKeepsPins)
{
    physics::ClothSolver solver;
    Prepare(solver);
    std::vector<math::Vector3> centers;
    ASSERT_TRUE(solver.Step(0.01f,{},{},[&](const math::Vector3& center) {
        centers.push_back(center);
        return center.x < 5 ? math::Vector3{0,0,2} : math::Vector3{};
    }));
    ASSERT_EQ(centers.size(),8u);
    EXPECT_VEC3_NEAR(centers[0],(math::Vector3{1.0f/3,1.0f/3,0}),1.0e-6f);
    EXPECT_GT(centers[2].z,0);
    EXPECT_GT(solver.Positions()[1].z,0);
    EXPECT_VEC3_NEAR(solver.Positions()[0],m_positions[0],1.0e-6f);
    for (size_t i = 3; i < 6; ++i) EXPECT_VEC3_NEAR(solver.Positions()[i],m_positions[i],1.0e-6f);
    ASSERT_TRUE(solver.Step(0.01f));
    EXPECT_EQ(centers.size(),8u);
}
TEST_F(ClothWindTest, InvalidSampleRollsBackEarlierTrianglesAndCanRecover)
{
    physics::ClothSolver solver;
    Prepare(solver);
    int calls = 0;
    EXPECT_FALSE(solver.Step(0.01f,{},{},[&](const math::Vector3&) {
        return ++calls == 2 ? math::Vector3{std::numeric_limits<float>::quiet_NaN(),0,0} : math::Vector3{0,0,10};
    }));
    EXPECT_EQ(calls,2);
    EXPECT_TRUE(solver.ContactResponses().empty());
    for (size_t i = 0; i < m_positions.size(); ++i) {
        EXPECT_VEC3_NEAR(solver.Positions()[i],m_positions[i],1.0e-6f);
        EXPECT_VEC3_NEAR(solver.Velocities()[i],math::Vector3{},1.0e-6f);
    }
    ASSERT_TRUE(solver.Step(0.01f,{},{},[](const math::Vector3&) { return math::Vector3{0,0,2}; }));
    EXPECT_GT(solver.Positions()[1].z,0);
}
}
