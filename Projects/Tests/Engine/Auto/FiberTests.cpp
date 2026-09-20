/// @file    FiberTests.cpp
/// @brief   繊維設定の保存、不正 material 値、Fin の継ぎ目と失敗時保持を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/FiberMaterialSettings.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Renderer/FiberGeometry.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Scene/Components/FiberInteractorComponent.hpp>
#include <Engine/Scene/Systems/RenderPasses/Geometry/FiberRenderPass.hpp>
#include <cstring>
#include <limits>

namespace fbzz::tests {
class FiberTest : public testkit::EngineFixture {};

namespace {
renderer::Mesh FiberTriangle()
{
    renderer::Mesh mesh;
    mesh.cpuVertices.resize(3);
    mesh.cpuVertices[0].position = {0, 0, 0};
    mesh.cpuVertices[1].position = {1, 0, 0};
    mesh.cpuVertices[2].position = {0, 0, 1};
    for (auto& v : mesh.cpuVertices) v.normal = {0, 1, 0};
    mesh.cpuIndices = {0, 2, 1};
    return mesh;
}
} // namespace

TEST_F(FiberTest, ClampsMaterialAndRejectsNonFiniteValues)
{
    asset::MaterialAsset material;
    material.params["fiberLength"] = {-5.0f};
    material.params["fiberDensity"] = {5.0f};
    material.params["maxBend"] = {std::numeric_limits<float>::infinity()};
    material.params["fiberFrequency"] = {std::numeric_limits<float>::quiet_NaN()};
    material.params["rootColor"] = {0.2f, std::numeric_limits<float>::quiet_NaN(), 0.1f};
    const auto settings = asset::ResolveFiberMaterial(&material);
    const auto defaults = asset::ResolveFiberMaterial(nullptr);
    EXPECT_FLOAT_EQ(settings.m_length, 0.0f);
    EXPECT_FLOAT_EQ(settings.m_density, 1.0f);
    EXPECT_FLOAT_EQ(settings.m_maxBend, defaults.m_maxBend);
    EXPECT_FLOAT_EQ(settings.m_frequency, defaults.m_frequency);
    EXPECT_FLOAT_EQ(settings.m_rootColor.x, defaults.m_rootColor.x);
}

TEST_F(FiberTest, FurLookParametersDefaultOffAndClamp)
{
    const auto defaults = asset::ResolveFiberMaterial(nullptr);
    EXPECT_FLOAT_EQ(defaults.m_clumping, 0.0f);
    EXPECT_FLOAT_EQ(defaults.m_secondarySpecular, 0.0f);
    asset::MaterialAsset material;
    material.params["clumping"] = {3.0f};
    material.params["clumpTwist"] = {-9.0f};
    material.params["specularShift"] = {std::numeric_limits<float>::quiet_NaN()};
    const auto settings = asset::ResolveFiberMaterial(&material);
    EXPECT_FLOAT_EQ(settings.m_clumping, 1.0f);
    EXPECT_FLOAT_EQ(settings.m_clumpTwist, -2.0f);
    EXPECT_FLOAT_EQ(settings.m_specularShift, defaults.m_specularShift);
}

TEST_F(FiberTest, ComponentDefaultsFitCharacterBudget)
{
    const scene::FiberComponent fiber;
    EXPECT_EQ(fiber.m_shellCount, 16);
    EXPECT_TRUE(fiber.m_distanceLod);
    EXPECT_LE(fiber.m_shadowShellCount, fiber.m_shellCount);
}

TEST_F(FiberTest, RejectsWrongParameterArity)
{
    asset::MaterialAsset material;
    material.params["fiberLength"] = {1.0f, 2.0f};
    material.params["tipColor"] = {1.0f};
    const auto settings = asset::ResolveFiberMaterial(&material);
    const auto defaults = asset::ResolveFiberMaterial(nullptr);
    EXPECT_FLOAT_EQ(settings.m_length, defaults.m_length);
    EXPECT_FLOAT_EQ(settings.m_tipColor.y, defaults.m_tipColor.y);
}

TEST_F(FiberTest, BuildsBoundaryFinsWithFixedRootsAndUnitHeightTips)
{
    const auto mesh = FiberTriangle();
    renderer::FiberFinMesh fins;
    ASSERT_TRUE(renderer::BuildFiberFins(mesh, fins));
    EXPECT_EQ(fins.m_vertices.size(), 30u);
    EXPECT_EQ(fins.m_indices.size(), 72u);
    for (size_t edge = 0; edge < 3; ++edge) {
        EXPECT_FLOAT_EQ(fins.m_vertices[edge * 10].m_height, 0.0f);
        EXPECT_FLOAT_EQ(fins.m_vertices[edge * 10 + 8].m_height, 1.0f);
        EXPECT_VEC3_NEAR(fins.m_vertices[edge * 10].m_position,
                         fins.m_vertices[edge * 10 + 8].m_position, 1.0e-6f);
    }
    for (auto index : fins.m_indices) EXPECT_LT(index, fins.m_vertices.size());
}

TEST_F(FiberTest, WeldsUvSeamsWithoutDuplicatingSharedEdges)
{
    auto mesh = FiberTriangle();
    mesh.cpuVertices.push_back(mesh.cpuVertices[1]);
    mesh.cpuVertices.push_back(mesh.cpuVertices[2]);
    mesh.cpuVertices.push_back(mesh.cpuVertices[0]);
    mesh.cpuVertices[3].uv = {0.7f, 0.8f};
    mesh.cpuVertices[4].uv = {0.1f, 0.2f};
    mesh.cpuVertices[5].position = {1, 0, 1};
    mesh.cpuIndices.insert(mesh.cpuIndices.end(), {3, 4, 5});
    renderer::FiberFinMesh fins;
    ASSERT_TRUE(renderer::BuildFiberFins(mesh, fins));
    EXPECT_EQ(fins.m_vertices.size(), 50u);
    EXPECT_EQ(fins.m_indices.size(), 120u);
}

TEST_F(FiberTest, KeepsOutputWhenIndicesAreInvalid)
{
    auto mesh = FiberTriangle();
    mesh.cpuIndices[1] = 99;
    renderer::FiberFinMesh fins;
    fins.m_indices = {17};
    EXPECT_FALSE(renderer::BuildFiberFins(mesh, fins));
    ASSERT_EQ(fins.m_indices.size(), 1u);
    EXPECT_EQ(fins.m_indices[0], 17u);
}

TEST_F(FiberTest, RejectsNonManifoldEdgesAndNonFinitePositions)
{
    auto mesh = FiberTriangle();
    mesh.cpuIndices = {0, 2, 1, 0, 2, 1, 0, 2, 1};
    renderer::FiberFinMesh fins;
    EXPECT_FALSE(renderer::BuildFiberFins(mesh, fins));
    mesh = FiberTriangle();
    mesh.cpuVertices[0].position.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(renderer::BuildFiberFins(mesh, fins));
}

TEST_F(FiberTest, SkipsDegenerateFacesAndRejectsSkinnedInput)
{
    auto mesh = FiberTriangle();
    mesh.cpuIndices = {0, 0, 0};
    renderer::FiberFinMesh fins;
    ASSERT_TRUE(renderer::BuildFiberFins(mesh, fins));
    EXPECT_TRUE(fins.m_vertices.empty());
    mesh = FiberTriangle();
    mesh.isSkinned = true;
    EXPECT_FALSE(renderer::BuildFiberFins(mesh, fins));
}

TEST_F(FiberTest, PreservesFiberModesAndMaterialThroughSceneSave)
{
    testkit::TempDir temp{"fiber"};
    ASSERT_TRUE(temp.IsValid());
    scene::Scene source;
    auto& go = source.CreateGameObject("FiberSurface");
    auto& fiber = go.AddComponent<scene::FiberComponent>();
    fiber.m_mode = scene::FiberRenderMode::HYBRID;
    fiber.m_shellCount = 37;
    fiber.m_enabled = false;
    fiber.m_motionHistories.emplace_back();
    fiber.m_motionHistories.back().Advance(42, renderer::FiberDeformationState{});
    fiber.m_materialPath = "guid:739d861ca4e047919d17c941c10191c8|Assets/Materials/Fiber/Grass.mat";
    fiber.m_shadowShellCount = 3;
    fiber.m_colorTint = {0.5f, 0.25f, 1.0f};
    fiber.m_lengthScale = 1.5f;
    fiber.m_densityScale = 0.5f;
    fiber.m_clumpingScale = 2.0f;
    fiber.m_maskPath = "Assets/Textures/FurMask.png";
    const auto path = temp.File("fiber.scene").generic_string();
    ASSERT_TRUE(scene::SceneSerializer::Save(source, path));
    const auto restored = scene::SceneSerializer::LoadData(path);
    ASSERT_NE(restored, nullptr);
    const auto entities = restored->GetEntities<scene::FiberComponent>();
    ASSERT_EQ(entities.size(), 1u);
    const auto* loaded = restored->GetComponent<scene::FiberComponent>(entities[0]);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->m_mode, scene::FiberRenderMode::HYBRID);
    EXPECT_EQ(loaded->m_shellCount, 37);
    EXPECT_FALSE(loaded->m_enabled);
    EXPECT_TRUE(loaded->m_motionHistories.empty());
    EXPECT_EQ(loaded->m_materialPath, fiber.m_materialPath);
    EXPECT_EQ(loaded->m_shadowShellCount, 3);
    EXPECT_VEC3_NEAR(loaded->m_colorTint, fiber.m_colorTint, 1.0e-6f);
    EXPECT_FLOAT_EQ(loaded->m_lengthScale, 1.5f);
    EXPECT_FLOAT_EQ(loaded->m_densityScale, 0.5f);
    EXPECT_FLOAT_EQ(loaded->m_clumpingScale, 2.0f);
    EXPECT_EQ(loaded->m_maskPath, fiber.m_maskPath);
}

TEST_F(FiberTest, AdvancesWindHistoryEvenWhenWorldIsStatic)
{
    renderer::FiberMotionHistory history;
    renderer::FiberDeformationState state;
    state.m_time = 1.0f;
    state.m_wind = {1.0f, 0.0f, 0.0f};
    history.Advance(10, state);
    EXPECT_FALSE(history.m_valid);
    state.m_time = 2.0f;
    state.m_wind = {2.0f, 0.0f, 0.0f};
    history.Advance(11, state);
    EXPECT_TRUE(history.m_valid);
    EXPECT_FLOAT_EQ(history.m_previous.m_time, 1.0f);
    EXPECT_FLOAT_EQ(history.m_previous.m_wind.x, 1.0f);
    EXPECT_FLOAT_EQ(history.m_current.m_wind.x, 2.0f);
}

TEST_F(FiberTest, DoesNotAdvanceHistoryTwiceInOneFrame)
{
    renderer::FiberMotionHistory history;
    renderer::FiberDeformationState state;
    state.m_time = 1.0f;
    history.Advance(1, state);
    state.m_time = 2.0f;
    history.Advance(2, state);
    state.m_time = 3.0f;
    history.Advance(2, state);
    EXPECT_TRUE(history.m_valid);
    EXPECT_FLOAT_EQ(history.m_previous.m_time, 1.0f);
    EXPECT_FLOAT_EQ(history.m_current.m_time, 2.0f);
}

TEST_F(FiberTest, ResetsHistoryAfterGapRewindAndTopologyChanges)
{
    renderer::FiberMotionHistory history;
    renderer::FiberDeformationState state;
    state.m_time = 1.0f;
    history.Advance(1, state);
    history.Advance(3, state);
    EXPECT_FALSE(history.m_valid);
    state.m_time = 0.0f;
    history.Advance(4, state);
    EXPECT_FALSE(history.m_valid);
    state.m_mode = 2;
    history.Advance(5, state);
    EXPECT_FALSE(history.m_valid);
    state.m_shellCount = 64;
    history.Advance(6, state);
    EXPECT_FALSE(history.m_valid);
    history.Advance(7, state);
    EXPECT_TRUE(history.m_valid);
    history.Advance(1, state);
    EXPECT_FALSE(history.m_valid);
}

TEST_F(FiberTest, KeepsIndependentViewHistory)
{
    renderer::FiberMotionHistory first;
    renderer::FiberMotionHistory second;
    renderer::FiberDeformationState state;
    state.m_time = 1.0f;
    first.Advance(1, state);
    state.m_time = 2.0f;
    second.Advance(2, state);
    first.Advance(2, state);
    EXPECT_TRUE(first.m_valid);
    EXPECT_FALSE(second.m_valid);
    EXPECT_FLOAT_EQ(first.m_previous.m_time, 1.0f);
    EXPECT_FLOAT_EQ(second.m_previous.m_time, 2.0f);
}

TEST_F(FiberTest, CarriesBoneWeightsIntoEveryFinHeight)
{
    auto mesh=FiberTriangle();
    mesh.isSkinned=true;
    for (const auto& vertex:mesh.cpuVertices) {
        renderer::SkinnedVertex skinned;
        skinned.position=vertex.position;
        skinned.normal=vertex.normal;
        skinned.boneIndices[0]=7;
        skinned.boneWeights[0]=0.75f;
        skinned.boneIndices[1]=9;
        skinned.boneWeights[1]=0.25f;
        mesh.cpuSkinnedVertices.push_back(skinned);
    }
    renderer::FiberFinMesh fins;
    ASSERT_TRUE(renderer::BuildFiberFins(mesh,fins));
    ASSERT_FALSE(fins.m_vertices.empty());
    for (const auto& vertex:fins.m_vertices) {
        EXPECT_EQ(vertex.m_boneIndices[0],7u);
        EXPECT_EQ(vertex.m_boneIndices[1],9u);
        EXPECT_FLOAT_EQ(vertex.m_boneWeights[0],0.75f);
        EXPECT_FLOAT_EQ(vertex.m_boneWeights[1],0.25f);
    }
}

TEST_F(FiberTest, BuildsContinuousBladesOverTriangleInterior)
{
    const auto mesh=FiberTriangle();
    std::vector<renderer::FiberBladeRoot> blades;
    ASSERT_TRUE(renderer::BuildFiberBlades(mesh,40,0.02f,17,blades));
    ASSERT_EQ(blades.size(),20u);
    for (const auto& blade:blades) {
        EXPECT_GT(blade.m_position.x,0.0f);
        EXPECT_GT(blade.m_position.z,0.0f);
        EXPECT_LT(blade.m_position.x+blade.m_position.z,1.0f);
        EXPECT_GE(blade.m_height,0.65f);
        EXPECT_LE(blade.m_height,1.0f);
        EXPECT_GE(blade.m_rank,0.0f);
        EXPECT_LT(blade.m_rank,1.0f);
        /// @note 2 枚のリボンは法線に直交する十字で、幅の半分の長さを持つ。
        EXPECT_NEAR(blade.m_side0.Length(),0.01f,1.0e-5f);
        EXPECT_NEAR(blade.m_side1.Length(),0.01f,1.0e-5f);
        EXPECT_NEAR(math::Vector3::Dot(blade.m_side0,blade.m_side1),0.0f,1.0e-6f);
        EXPECT_NEAR(math::Vector3::Dot(blade.m_side0,blade.m_normal),0.0f,1.0e-6f);
    }
    std::vector<renderer::FiberBladeRoot> again;
    ASSERT_TRUE(renderer::BuildFiberBlades(mesh,40,0.02f,17,again));
    ASSERT_EQ(again.size(),blades.size());
    for (size_t i=0;i<blades.size();++i)
        EXPECT_EQ(std::memcmp(&blades[i],&again[i],sizeof(renderer::FiberBladeRoot)),0);
    /// @note GPU へ送るのは 1 葉 64 バイト。以前の 28 頂点 + 72 インデックス (3312 バイト) を CPU で作らない。
    EXPECT_EQ(sizeof(renderer::FiberBladeRoot),64u);
}

TEST_F(FiberTest, VertexAlphaDensityKeepsWhiteMeshRootsIdentical)
{
    const auto mesh=FiberTriangle();
    std::vector<renderer::FiberBladeRoot> legacy;
    std::vector<renderer::FiberBladeRoot> flagOff;
    std::vector<renderer::FiberBladeRoot> flagOn;
    ASSERT_TRUE(renderer::BuildFiberBlades(mesh,40,0.02f,17,legacy));
    ASSERT_TRUE(renderer::BuildFiberBlades(mesh,40,0.02f,17,flagOff,false));
    ASSERT_TRUE(renderer::BuildFiberBlades(mesh,40,0.02f,17,flagOn,true));
    ASSERT_EQ(flagOff.size(),legacy.size());
    ASSERT_EQ(flagOn.size(),legacy.size()) << "A = 1 の面では棄却の乱数を引かない";
    for (size_t i=0;i<legacy.size();++i) {
        EXPECT_EQ(std::memcmp(&legacy[i],&flagOff[i],sizeof(renderer::FiberBladeRoot)),0);
        EXPECT_EQ(std::memcmp(&legacy[i],&flagOn[i],sizeof(renderer::FiberBladeRoot)),0);
    }
}

TEST_F(FiberTest, VertexAlphaDensityDropsBladesWhereAlphaIsZero)
{
    auto mesh=FiberTriangle();
    for (auto& vertex:mesh.cpuVertices) vertex.color.w=0.0f;
    std::vector<renderer::FiberBladeRoot> blades;
    ASSERT_TRUE(renderer::BuildFiberBlades(mesh,40,0.02f,17,blades,true));
    EXPECT_TRUE(blades.empty());
    std::vector<renderer::FiberBladeRoot> ignored;
    ASSERT_TRUE(renderer::BuildFiberBlades(mesh,40,0.02f,17,ignored,false));
    EXPECT_EQ(ignored.size(),20u) << "フラグが false なら A を読まない";
}

TEST_F(FiberTest, RejectsInvalidBladeInputWithoutReplacingOutput)
{
    auto mesh=FiberTriangle();
    std::vector<renderer::FiberBladeRoot> output(1);
    output[0].m_rank=0.25f;
    EXPECT_FALSE(renderer::BuildFiberBlades(mesh,100000,0.02f,1,output));
    EXPECT_FALSE(renderer::BuildFiberBlades(mesh,10,std::numeric_limits<float>::quiet_NaN(),1,output));
    mesh.cpuIndices[0]=99;
    EXPECT_FALSE(renderer::BuildFiberBlades(mesh,10,0.02f,1,output));
    ASSERT_EQ(output.size(),1u);
    EXPECT_FLOAT_EQ(output[0].m_rank,0.25f);
}

TEST_F(FiberTest, DistanceLodIsBoundedAndMonotonic)
{
    int last=64;
    for (int distance=0;distance<=60;++distance) {
        const int count=renderer::FiberLodShellCount(64,8,static_cast<float>(distance),8,40);
        EXPECT_LE(count,last);
        EXPECT_GE(count,8);
        last=count;
    }
    EXPECT_EQ(renderer::FiberLodShellCount(64,8,0,8,40),64);
    EXPECT_EQ(renderer::FiberLodShellCount(64,8,40,8,40),8);
    EXPECT_EQ(renderer::FiberLodShellCount(0,90,10,10,10),1);
}

TEST_F(FiberTest, ContactCountsStopAtTheLastSlotThatCanStillBend)
{
    scene::FiberContactCB data;
    scene::UpdateFiberContactCounts(data, 10.0f);
    EXPECT_EQ(data.m_count, 0u) << "接触が無いシーンでは頂点ごとの走査を 1 回もしない";
    EXPECT_EQ(data.m_previousCount, 0u);

    /// @note 枠 0 は回復済み、枠 2 は回復中、枠 5 はまだ発生していない。
    data.m_centers[0] = { 0, 0, 0, 1 };
    data.m_times[0] = { 1.0f, 1.0f, 2.0f, 0 };
    data.m_centers[2] = { 0, 0, 0, 1 };
    data.m_times[2] = { 9.5f, 1.0f, 2.0f, 0 };
    data.m_centers[5] = { 0, 0, 0, 1 };
    data.m_times[5] = { 11.0f, 1.0f, 2.0f, 0 };
    data.m_centers[32 + 7] = { 0, 0, 0, 1 };
    data.m_times[32 + 7] = { 0.0f, 1.0f, 0.05f, 0 };
    scene::UpdateFiberContactCounts(data, 10.0f);
    EXPECT_EQ(data.m_count, 3u);
    /// @note 前フレームは評価時刻が履歴でずれるので、回復済みでも半径を持つ枠までは走査する。
    EXPECT_EQ(data.m_previousCount, 8u);

    data.m_times[2].x = 7.0f;
    scene::UpdateFiberContactCounts(data, 10.0f);
    EXPECT_EQ(data.m_count, 0u) << "回復しきった枠だけなら走査しない";
}

TEST_F(FiberTest, SavesBladeTerrainLodAndInteractorSettings)
{
    testkit::TempDir temp{"fiber-blade"};
    ASSERT_TRUE(temp.IsValid());
    scene::Scene source;
    auto& go=source.CreateGameObject("BladePatch");
    auto& fiber=go.AddComponent<scene::FiberComponent>();
    fiber.m_mode=scene::FiberRenderMode::BLADE;
    fiber.m_distanceLod=true;
    fiber.m_bladeDensity=600;
    fiber.m_bladeWidth=0.025f;
    fiber.m_terrainPatchCells=3;
    fiber.m_terrainLayer=2;
    fiber.m_terrainLayerThreshold=0.6f;
    fiber.m_flowChannels=0b101;
    auto& actor=go.AddComponent<scene::FiberInteractorComponent>();
    actor.m_radius=0.7f;
    actor.m_recoverySeconds=3;
    const auto path=temp.File("blade.scene").generic_string();
    ASSERT_TRUE(scene::SceneSerializer::Save(source,path));
    auto restored=scene::SceneSerializer::LoadData(path);
    ASSERT_NE(restored,nullptr);
    const auto entities=restored->GetEntities<scene::FiberComponent>();
    ASSERT_EQ(entities.size(),1u);
    const auto* loaded=restored->GetComponent<scene::FiberComponent>(entities[0]);
    ASSERT_NE(loaded,nullptr);
    EXPECT_EQ(loaded->m_mode,scene::FiberRenderMode::BLADE);
    EXPECT_TRUE(loaded->m_distanceLod);
    EXPECT_FLOAT_EQ(loaded->m_bladeDensity,600);
    EXPECT_EQ(loaded->m_terrainPatchCells,3);
    EXPECT_EQ(loaded->m_terrainLayer,2);
    EXPECT_FLOAT_EQ(loaded->m_terrainLayerThreshold,0.6f);
    EXPECT_EQ(loaded->m_flowChannels,0b101);
    EXPECT_EQ(scene::FiberComponent{}.m_flowChannels,-1);
    EXPECT_EQ(scene::FiberComponent{}.m_terrainLayer,-1) << "既定は地形全体";
    EXPECT_FLOAT_EQ(scene::FiberComponent{}.m_terrainLayerThreshold,0.25f);
    const auto* contact=restored->GetComponent<scene::FiberInteractorComponent>(entities[0]);
    ASSERT_NE(contact,nullptr);
    EXPECT_FLOAT_EQ(contact->m_radius,0.7f);
    EXPECT_FLOAT_EQ(contact->m_recoverySeconds,3);
}

} // namespace fbzz::tests
