/// @file    ClothAssetTests.cpp
/// @brief   布アセットの seam 対応と破損データの拒否、保存を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/ClothAsset.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <limits>

namespace fbzz::tests {
class ClothAssetTest : public testkit::EngineFixture {
protected:
    renderer::Mesh m_mesh;
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        const math::Vector3 positions[]{{0,0,0}, {0,-1,0}, {1,0,0}, {1,0,0}, {0,-1,0}, {1,-1,0}};
        for (const auto& p : positions) m_mesh.cpuVertices.push_back({p, {0,0,1}, {1,0,0}, {p.x, -p.y}});
        m_mesh.cpuVertices[3].uv = {0,0};
        m_mesh.cpuIndices = {0,1,2,3,4,5};
    }
    asset::ClothAsset Sewn()
    {
        const uint32_t mapping[]{0,1,2,2,1,3};
        asset::ClothAsset result;
        EXPECT_TRUE(asset::CreateClothAsset(m_mesh, mapping, result));
        return result;
    }
    asset::Skeleton SkinMesh()
    {
        m_mesh.isSkinned = true;
        for (const auto& vertex : m_mesh.cpuVertices) {
            renderer::SkinnedVertex skin;
            skin.position = vertex.position;
            skin.normal = vertex.normal;
            skin.tangent = vertex.tangent;
            skin.uv = vertex.uv;
            skin.boneIndices[0] = 0;
            skin.boneIndices[1] = 1;
            skin.boneIndices[2] = 1;
            skin.boneWeights[0] = 2.0f;
            skin.boneWeights[1] = 1.0f;
            skin.boneWeights[2] = 1.0f;
            m_mesh.cpuSkinnedVertices.push_back(skin);
        }
        asset::Skeleton skeleton;
        skeleton.bones.push_back({"Left", 0, math::Matrix4::Identity()});
        skeleton.bones.push_back({"Right", 1, math::Matrix4::Translate({-1,0,0})});
        skeleton.referencePose = {math::Matrix4::Translate({0,0,2}), math::Matrix4::Translate({0,0,2})};
        return skeleton;
    }
};

TEST_F(ClothAssetTest, ExplicitMappingSewsSeamWithoutChangingUVs)
{
    auto cloth = Sewn();
    ASSERT_EQ(cloth.particles.size(), 4u);
    ASSERT_EQ(cloth.vertices.size(), 6u);
    EXPECT_EQ(cloth.renderToParticle[2], cloth.renderToParticle[3]);
    EXPECT_NE(cloth.vertices[2].uv.x, cloth.vertices[3].uv.x);
    EXPECT_TRUE(asset::ValidateClothAsset(cloth));
}

TEST_F(ClothAssetTest, IdentityMappingDoesNotWeldCoincidentDisconnectedCloth)
{
    asset::ClothAsset cloth;
    ASSERT_TRUE(asset::CreateClothAsset(m_mesh, {}, cloth));
    EXPECT_EQ(cloth.particles.size(), 6u);
    EXPECT_NE(cloth.renderToParticle[2], cloth.renderToParticle[3]);
}

TEST_F(ClothAssetTest, RoundTripPreservesMappingPinsAndVertexAttributes)
{
    auto cloth = Sewn();
    cloth.pins = {0,2};
    cloth.vertices[0].color = {0.2f,0.3f,0.4f,0.5f};
    testkit::TempDir temp{"clothasset"};
    ASSERT_TRUE(temp.IsValid());
    const std::string path = temp.File("mesh.cloth").generic_string();
    ASSERT_TRUE(asset::SaveClothAssetToFile(path, cloth));
    asset::ClothAsset loaded;
    ASSERT_TRUE(asset::LoadClothAssetFromFile(path, loaded));
    EXPECT_EQ(loaded.renderToParticle, cloth.renderToParticle);
    EXPECT_EQ(loaded.pins, cloth.pins);
    EXPECT_EQ(loaded.indices, cloth.indices);
    EXPECT_FLOAT_EQ(loaded.vertices[0].color.w, 0.5f);
    EXPECT_FLOAT_EQ(loaded.vertices[3].uv.x, 0.0f);
    const uint64_t revision = loaded.revision;
    ASSERT_TRUE(asset::LoadClothAssetFromFile(path, loaded));
    EXPECT_GT(loaded.revision, revision);
}

TEST_F(ClothAssetTest, InvalidMappingAndPinsAreRejected)
{
    auto cloth = Sewn();
    cloth.renderToParticle[0] = 99;
    EXPECT_FALSE(asset::ValidateClothAsset(cloth));
    cloth = Sewn();
    cloth.pins = {0,0};
    EXPECT_FALSE(asset::ValidateClothAsset(cloth));
    cloth.pins = {99};
    EXPECT_FALSE(asset::ValidateClothAsset(cloth));
    cloth = Sewn();
    cloth.particles.push_back({5,5,5});
    EXPECT_FALSE(asset::ValidateClothAsset(cloth));
    cloth = Sewn();
    cloth.vertices[3].position.x += 0.1f;
    EXPECT_FALSE(asset::ValidateClothAsset(cloth));
}

TEST_F(ClothAssetTest, InvalidGeometryDoesNotReplaceExistingAsset)
{
    auto cloth = Sewn();
    m_mesh.cpuIndices = {0,0,1};
    EXPECT_FALSE(asset::CreateClothAsset(m_mesh, {}, cloth));
    EXPECT_EQ(cloth.particles.size(), 4u);
    m_mesh.isSkinned = true;
    EXPECT_FALSE(asset::CreateClothAsset(m_mesh, {}, cloth));
    cloth.vertices[0].uv.x = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(asset::ValidateClothAsset(cloth));
}

TEST_F(ClothAssetTest, FailedSaveAndLoadPreservePreviousContent)
{
    auto cloth = Sewn();
    testkit::TempDir temp{"clothinvalid"};
    ASSERT_TRUE(temp.IsValid());
    const std::string path = temp.File("mesh.cloth").generic_string();
    ASSERT_TRUE(asset::SaveClothAssetToFile(path, cloth));
    cloth.indices[0] = 99;
    EXPECT_FALSE(asset::SaveClothAssetToFile(path, cloth));
    ASSERT_TRUE(asset::LoadClothAssetFromFile(path, cloth));
    EXPECT_EQ(cloth.indices[0], 0u);
    ASSERT_TRUE(util::FileSystem::WriteText(path, "version = 99\n"));
    EXPECT_FALSE(asset::LoadClothAssetFromFile(path, cloth));
    EXPECT_EQ(cloth.particles.size(), 4u);
    ASSERT_TRUE(util::FileSystem::WriteText(path, "version = 1\nparticles = [[0,0,0]]\nvertices=[]\nindices=[-1]\nrender_to_particle=[]\npins=[]\n"));
    EXPECT_FALSE(asset::LoadClothAssetFromFile(path, cloth));
    EXPECT_EQ(cloth.particles.size(), 4u);
}

TEST_F(ClothAssetTest, SkinWeightsTransferMergeNormalizeAndBlendInverseBind)
{
    const auto skeleton = SkinMesh();
    const uint32_t mapping[]{0,1,2,2,1,3};
    asset::ClothAsset cloth;
    ASSERT_TRUE(asset::CreateSkinnedClothAsset(m_mesh, skeleton, mapping, cloth));
    ASSERT_EQ(cloth.skinWeights.size(), 4u);
    EXPECT_FLOAT_EQ(cloth.skinWeights[0].weights[0], 0.5f);
    EXPECT_FLOAT_EQ(cloth.skinWeights[0].weights[1], 0.5f);
    EXPECT_FLOAT_EQ(cloth.skinWeights[0].weights[2], 0.0f);
    EXPECT_NEAR(cloth.particles[0].z, 2.0f, 1.0e-6f);
    const std::array<math::Matrix4, 2> world{math::Matrix4::Translate({0,0,2}), math::Matrix4::Translate({1,0,4})};
    std::vector<math::Vector3> positions;
    ASSERT_TRUE(asset::EvaluateClothSkinning(cloth, world, positions));
    EXPECT_VEC3_NEAR(positions[0], (math::Vector3{0,0,3}), 1.0e-6f);
    EXPECT_VEC3_NEAR(positions[2], (math::Vector3{1,0,3}), 1.0e-6f);
    EXPECT_NE(cloth.vertices[2].uv.x, cloth.vertices[3].uv.x);
}

TEST_F(ClothAssetTest, SkinRoundTripPreservesNamesMatricesAndWeights)
{
    const auto skeleton = SkinMesh();
    asset::ClothAsset cloth;
    ASSERT_TRUE(asset::CreateSkinnedClothAsset(m_mesh, skeleton, {}, cloth));
    testkit::TempDir temp{"clothskin"};
    ASSERT_TRUE(temp.IsValid());
    const auto path = temp.File("skin.cloth").generic_string();
    ASSERT_TRUE(asset::SaveClothAssetToFile(path, cloth));
    asset::ClothAsset loaded;
    ASSERT_TRUE(asset::LoadClothAssetFromFile(path, loaded));
    ASSERT_EQ(loaded.skinBones.size(), 2u);
    EXPECT_EQ(loaded.skinBones[1].name, "Right");
    EXPECT_FLOAT_EQ(loaded.skinBones[1].inverseBind.m[0][3], -1.0f);
    EXPECT_EQ(loaded.skinWeights[0].weights, cloth.skinWeights[0].weights);
    EXPECT_EQ(loaded.skinWeights[0].boneIndices, cloth.skinWeights[0].boneIndices);
    EXPECT_VEC3_NEAR(loaded.skinWeights[0].position, cloth.skinWeights[0].position, 1.0e-6f);
}

TEST_F(ClothAssetTest, InvalidSkinWeightsNamesAndSeamRejectWithoutReplacingOutput)
{
    auto skeleton = SkinMesh();
    m_mesh.isSkinned = false;
    auto cloth = Sewn();
    m_mesh.isSkinned = true;
    const uint32_t mapping[]{0,1,2,2,1,3};
    m_mesh.cpuSkinnedVertices[3].boneWeights[0] = 4.0f;
    EXPECT_FALSE(asset::CreateSkinnedClothAsset(m_mesh, skeleton, mapping, cloth));
    EXPECT_TRUE(cloth.skinWeights.empty());
    m_mesh.cpuSkinnedVertices[3].boneWeights[0] = -1.0f;
    EXPECT_FALSE(asset::CreateSkinnedClothAsset(m_mesh, skeleton, {}, cloth));
    m_mesh.cpuSkinnedVertices[3].boneWeights[0] = 2.0f;
    m_mesh.cpuSkinnedVertices[3].boneIndices[0] = 99;
    EXPECT_FALSE(asset::CreateSkinnedClothAsset(m_mesh, skeleton, {}, cloth));
    m_mesh.cpuSkinnedVertices[3].boneIndices[0] = 0;
    skeleton.bones[1].name = "Left";
    EXPECT_FALSE(asset::CreateSkinnedClothAsset(m_mesh, skeleton, {}, cloth));
    EXPECT_EQ(cloth.particles.size(), 4u);
}

TEST_F(ClothAssetTest, InvalidSkinPoseDoesNotPartiallyReplacePositions)
{
    const auto skeleton = SkinMesh();
    asset::ClothAsset cloth;
    ASSERT_TRUE(asset::CreateSkinnedClothAsset(m_mesh, skeleton, {}, cloth));
    std::vector<math::Vector3> positions{{7,8,9}};
    std::array<math::Matrix4, 2> world{math::Matrix4::Identity(), math::Matrix4::Identity()};
    world[1].m[0][0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(asset::EvaluateClothSkinning(cloth, world, positions));
    ASSERT_EQ(positions.size(), 1u);
    EXPECT_VEC3_NEAR(positions[0], (math::Vector3{7,8,9}), 1.0e-6f);
    cloth.skinWeights[0].weights[0] = 0.0f;
    EXPECT_FALSE(asset::ValidateClothAsset(cloth));
}

TEST_F(ClothAssetTest, VersionOneStaticClothRemainsReadable)
{
    auto cloth = Sewn();
    testkit::TempDir temp{"clothv1"};
    ASSERT_TRUE(temp.IsValid());
    const auto path = temp.File("legacy.cloth").generic_string();
    ASSERT_TRUE(asset::SaveClothAssetToFile(path, cloth));
    std::string text;
    ASSERT_TRUE(util::FileSystem::ReadText(path, text));
    const auto version = text.find("version = 3");
    ASSERT_NE(version, std::string::npos);
    text.replace(version, 11, "version = 1");
    ASSERT_TRUE(util::FileSystem::WriteText(path, text));
    asset::ClothAsset loaded;
    ASSERT_TRUE(asset::LoadClothAssetFromFile(path, loaded));
    EXPECT_TRUE(loaded.skinWeights.empty());
    EXPECT_EQ(loaded.renderToParticle, cloth.renderToParticle);
}
}
