/// @file    ClothRenderBindingTests.cpp
/// @brief   低解像度布から描画頂点への変形転写と形式互換を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/ClothAsset.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <limits>

namespace fbzz::tests {
class ClothRenderBindingTest : public testkit::EngineFixture {
protected:
    asset::ClothAsset m_low;
    renderer::Mesh m_high;
    void SetUp() override
    {
        EngineFixture::SetUp();
        renderer::Mesh low;
        for (const math::Vector3 p : {math::Vector3{0,0,0}, {2,0,0}, {0,2,0}})
            low.cpuVertices.push_back({p,{0,0,1},{1,0,0},{}});
        low.cpuIndices = {0,1,2};
        ASSERT_TRUE(asset::CreateClothAsset(low,{},m_low));
        m_low.pins = {0};
        m_low.skinBones = {{"Root",math::Matrix4::Identity()}};
        for (const auto& p : m_low.particles) m_low.skinWeights.push_back({p,{0,0,0,0},{1,0,0,0}});
        for (const math::Vector3 p : {math::Vector3{0,0,0.1f}, {2,0,0.1f}, {0,2,0.1f}, {0.5f,0.5f,0.1f}})
            m_high.cpuVertices.push_back({p,{0,0,1},{1,0,0},{p.x,p.y}});
        m_high.cpuIndices = {0,1,3,1,2,3,2,0,3};
    }
};
TEST_F(ClothRenderBindingTest, PreservesRestDetailAndFollowsRigidRotationAndDeformation)
{
    asset::ClothAsset bound;
    ASSERT_TRUE(asset::BindClothRenderMesh(m_low,m_high,0.2f,bound));
    EXPECT_EQ(bound.particles.size(),3u);
    EXPECT_EQ(bound.vertices.size(),4u);
    EXPECT_EQ(bound.pins,m_low.pins);
    EXPECT_EQ(bound.skinWeights.size(),3u);
    EXPECT_TRUE(bound.renderToParticle.empty());
    std::vector<math::Vector3> rendered;
    ASSERT_TRUE(asset::EvaluateClothRenderBindings(bound.renderBindings,bound.particles,bound.particles,rendered));
    for (size_t i = 0; i < rendered.size(); ++i) EXPECT_VEC3_NEAR(rendered[i],m_high.cpuVertices[i].position,1.0e-6f);
    auto rotated = bound.particles;
    for (auto& p : rotated) p = {p.x+3,-p.z+4,p.y+5};
    ASSERT_TRUE(asset::EvaluateClothRenderBindings(bound.renderBindings,bound.particles,rotated,rendered));
    EXPECT_VEC3_NEAR(rendered[3],(math::Vector3{3.5f,3.9f,5.5f}),1.0e-5f);
    rotated = bound.particles;
    rotated[1].x = 4;
    ASSERT_TRUE(asset::EvaluateClothRenderBindings(bound.renderBindings,bound.particles,rotated,rendered));
    EXPECT_VEC3_NEAR(rendered[3],(math::Vector3{1,0.5f,0.1f}),1.0e-5f);
}
TEST_F(ClothRenderBindingTest, RoundTripPreservesSeparateTopologyAndRejectsBrokenBinding)
{
    asset::ClothAsset bound;
    ASSERT_TRUE(asset::BindClothRenderMesh(m_low,m_high,0.2f,bound));
    testkit::TempDir temp{"clothbinding"};
    const auto path = temp.File("bound.cloth").generic_string();
    ASSERT_TRUE(asset::SaveClothAssetToFile(path,bound));
    asset::ClothAsset loaded;
    ASSERT_TRUE(asset::LoadClothAssetFromFile(path,loaded));
    EXPECT_EQ(loaded.simulationIndices,(std::vector<uint32_t>{0,1,2}));
    EXPECT_EQ(loaded.indices,m_high.cpuIndices);
    EXPECT_EQ(loaded.renderBindings.size(),4u);
    EXPECT_FLOAT_EQ(loaded.vertices[3].uv.x,0.5f);
    loaded.renderBindings[0].particles[2] = 99;
    EXPECT_FALSE(asset::ValidateClothAsset(loaded));
    EXPECT_FALSE(asset::SaveClothAssetToFile(path,loaded));
    ASSERT_TRUE(asset::LoadClothAssetFromFile(path,loaded));
    loaded.renderBindings[0].barycentric.x = -1;
    EXPECT_FALSE(asset::ValidateClothAsset(loaded));
    loaded = bound;
    loaded.renderBindings[0].offset.z += 1;
    EXPECT_FALSE(asset::ValidateClothAsset(loaded));
}
TEST_F(ClothRenderBindingTest, FailedBindAndNonFinitePoseAreTransactionalAndCollapsedFaceIsFinite)
{
    asset::ClothAsset bound = m_low;
    EXPECT_FALSE(asset::BindClothRenderMesh(m_low,m_high,0.01f,bound));
    EXPECT_EQ(bound.vertices.size(),3u);
    ASSERT_TRUE(asset::BindClothRenderMesh(m_low,m_high,0.2f,bound));
    auto pose = bound.particles;
    pose[0].x = std::numeric_limits<float>::quiet_NaN();
    std::vector<math::Vector3> rendered{{7,8,9}};
    EXPECT_FALSE(asset::EvaluateClothRenderBindings(bound.renderBindings,bound.particles,pose,rendered));
    ASSERT_EQ(rendered.size(),1u);
    EXPECT_VEC3_NEAR(rendered[0],(math::Vector3{7,8,9}),1.0e-6f);
    pose.assign(3,{1,2,3});
    ASSERT_TRUE(asset::EvaluateClothRenderBindings(bound.renderBindings,bound.particles,pose,rendered));
    EXPECT_VEC3_NEAR(rendered[3],(math::Vector3{1,2,3.1f}),1.0e-6f);
}
TEST_F(ClothRenderBindingTest, VersionTwoKeepsSkinWeights)
{
    testkit::TempDir temp{"clothv2"};
    const auto path = temp.File("legacy.cloth").generic_string();
    ASSERT_TRUE(asset::SaveClothAssetToFile(path,m_low));
    std::string text;
    ASSERT_TRUE(util::FileSystem::ReadText(path,text));
    const auto version = text.find("version = 3");
    ASSERT_NE(version,std::string::npos);
    text.replace(version,11,"version = 2");
    ASSERT_TRUE(util::FileSystem::WriteText(path,text));
    asset::ClothAsset loaded;
    ASSERT_TRUE(asset::LoadClothAssetFromFile(path,loaded));
    EXPECT_EQ(loaded.skinBones.size(),1u);
    EXPECT_EQ(loaded.skinWeights.size(),3u);
    EXPECT_TRUE(loaded.renderBindings.empty());
}
}
